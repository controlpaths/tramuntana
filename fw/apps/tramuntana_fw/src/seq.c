/*
 * Tramuntana - seq. See seq.h.
 *
 * The program runs on its own thread, above the API and capture threads,
 * so a step follows the previous one within microseconds. Short delays
 * and the first 2 ms of a WAIT_DIO are busy waits on the cycle counter;
 * anything longer sleeps (or polls once per tick), so a long program
 * doesn't starve USB and the rest of the firmware. The bus steps call the
 * same instrument functions the API uses, with the same locking.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

#include "seq.h"
#include "board.h"
#include "proto.h"
#include "i2c.h"
#include "spi.h"
#include "awg.h"
#include "logic.h"
#include "scope.h"
#include "watch.h"
#include "bridge.h"
#include "display.h"

LOG_MODULE_REGISTER(seq, LOG_LEVEL_INF);

#define BUSY_LIMIT_US 2000

static struct {
  uint8_t code[SEQ_MAX_CODE];
  uint8_t result[SEQ_MAX_RESULT];
  uint16_t len;
  uint32_t timeout_ms;
  int64_t deadline;
  struct seq_status st;
  volatile bool abort_req;
  struct k_sem go;
} sq;

K_THREAD_STACK_DEFINE(seq_stack, 2048);
static struct k_thread seq_thread_data;

/* Length of the instruction at 'pc' (opcode included), or -1 if it is
 * unknown or runs past the end of the program. */
static int op_len(uint16_t pc)
{
  const uint8_t *c = sq.code + pc;
  int rem = (int)sq.len - (int)pc;
  int n;

  switch (c[0] & ~SEQ_CONTINUE) {
  case SEQ_DIO_READ:
  case SEQ_ENDLOOP:
    n = 1;
    break;
  case SEQ_POWER:
  case SEQ_PULLUPS:
  case SEQ_ADC_READ:
  case SEQ_MARK:
  case SEQ_WATCH:
    n = 2;
    break;
  case SEQ_DIO_DIR:
  case SEQ_DIO_WRITE:
  case SEQ_I2C_READ:
  case SEQ_AWG_DC:
  case SEQ_LOOP:
    n = 3;
    break;
  case SEQ_DELAY_US:
  case SEQ_DELAY_MS:
    n = 5;
    break;
  case SEQ_WAIT_DIO:
    n = 7;
    break;
  case SEQ_I2C_WRITE:
    n = rem >= 3 ? 3 + c[2] : -1;
    break;
  case SEQ_I2C_WRITE_READ:
    n = rem >= 3 ? 3 + c[2] + 1 : -1;
    break;
  case SEQ_SPI_TXN:
    n = rem >= 2 ? 2 + c[1] + 1 : -1;
    break;
  case SEQ_UART_SEND:
    n = rem >= 2 ? 2 + c[1] : -1;
    break;
  case SEQ_SCOPE_ARM:
    n = 13;
    break;
  case SEQ_LOGIC_ARM:
    n = 17;
    break;
  default:
    return -1;
  }
  return (n > 0 && n <= rem) ? n : -1;
}

static bool expired(void)
{
  return sq.abort_req || k_uptime_get() > sq.deadline;
}

static int delay_us(uint32_t us)
{
  if (us > BUSY_LIMIT_US) {
    /* Sleep most of it in small slices (abortable), busy wait the rest. */
    uint32_t start = board_time_us();

    while (board_time_us() - start + BUSY_LIMIT_US < us) {
      if (expired()) {
        return -ECANCELED;
      }
      k_sleep(K_MSEC(MIN((us - (board_time_us() - start) - BUSY_LIMIT_US) / 1000 + 1, 50u)));
    }
    while (board_time_us() - start < us) {
    }
    return 0;
  }
  board_delay_cycles(board_ns_to_cycles(us * 1000u));
  return 0;
}

static int wait_dio(uint8_t mask, uint8_t value, uint32_t timeout_us, uint32_t *waited)
{
  uint32_t start = board_time_us();

  for (;;) {
    uint32_t el = board_time_us() - start;

    if ((board_dio_read() & mask) == (value & mask)) {
      *waited = el;
      return 0;
    }
    if (el >= timeout_us) {
      *waited = el;
      return -ETIMEDOUT;
    }
    if (el > BUSY_LIMIT_US) {
      if (expired()) {
        *waited = el;
        return -ECANCELED;
      }
      k_sleep(K_TICKS(1));
    }
  }
}

/* Runs one step; record data goes to 'd' (*dn bytes). */
static int exec_step(const uint8_t *c, uint8_t *d, uint8_t *dn)
{
  uint8_t op = c[0] & ~SEQ_CONTINUE;
  size_t acked;
  uint32_t u;
  int ret = 0;

  *dn = 0;
  switch (op) {
  case SEQ_DELAY_US:
    return delay_us(rd32(c + 1));
  case SEQ_DELAY_MS:
    return delay_us(rd32(c + 1) * 1000u);
  case SEQ_POWER:
    board_power_set(c[1] != 0);
    return 0;
  case SEQ_PULLUPS:
    board_pullups_set(c[1] != 0);
    return 0;
  case SEQ_DIO_DIR:
    return board_dio_dir(c[1], c[2]);
  case SEQ_DIO_WRITE:
    return board_dio_write(c[1], c[2]);
  case SEQ_DIO_READ:
    d[0] = board_dio_read();
    *dn = 1;
    return 0;
  case SEQ_WAIT_DIO:
    ret = wait_dio(c[1], c[2], rd32(c + 3), &u);
    wr32(d, u);
    *dn = 4;
    return ret;
  case SEQ_I2C_WRITE:
    acked = 0;
    ret = i2c_drv_write(c[1], c + 3, c[2], &acked);
    d[0] = (uint8_t)acked;
    *dn = 1;
    return ret;
  case SEQ_I2C_READ:
    if (c[2] == 0) {
      return -EINVAL;
    }
    ret = i2c_drv_read(c[1], d, c[2]);
    *dn = ret == 0 ? c[2] : 0;
    return ret;
  case SEQ_I2C_WRITE_READ: {
    uint8_t wn = c[2], rn = c[3 + wn];

    if (rn == 0) {
      return -EINVAL;
    }
    ret = i2c_drv_write_read(c[1], c + 3, wn, d, rn);
    *dn = ret == 0 ? rn : 0;
    return ret;
  }
  case SEQ_SPI_TXN: {
    uint8_t wn = c[1], rn = c[2 + wn];

    ret = spi_drv_txn(c + 2, wn, d, rn);
    *dn = ret == 0 ? rn : 0;
    return ret;
  }
  case SEQ_UART_SEND:
    return bridge_target_write(c + 2, c[1]);
  case SEQ_AWG_DC:
    return awg_dc(rd16(c + 1));
  case SEQ_ADC_READ: {
    int mv = board_adc_read_mv(c[1]);

    if (mv < 0) {
      return mv;
    }
    wr16(d, (uint16_t)mv);
    *dn = 2;
    return 0;
  }
  case SEQ_MARK:
    d[0] = c[1];
    *dn = 1;
    return 0;
  case SEQ_WATCH:
    return watch_set(c[1]);
  case SEQ_LOGIC_ARM: {
    const uint8_t *p = c + 1;
    struct logic_params lp = {
      .n = rd32(p), .clkdiv = rd16(p + 4), .trig_mode = p[6], .trig_pin = p[7],
      .trig_mask = p[8], .trig_value = p[9], .pre = rd32(p + 10), .timeout_ms = rd16(p + 14),
    };

    ret = logic_arm(&lp, &u);
    wr32(d, ret == 0 ? u : 0);
    *dn = 4;
    return ret;
  }
  case SEQ_SCOPE_ARM: {
    const uint8_t *p = c + 1;
    struct scope_params sp = {
      .channels = p[0], .n = rd16(p + 1), .clkdiv = rd16(p + 3), .trig_mode = p[5],
      .trig_level = rd16(p + 6), .pre = rd16(p + 8), .timeout_ms = rd16(p + 10),
    };

    ret = scope_arm(&sp, &u);
    wr32(d, ret == 0 ? u : 0);
    *dn = 4;
    return ret;
  }
  default:
    return -EINVAL;
  }
}

static void record(uint16_t pc, uint8_t op, uint8_t status, const uint8_t *d, uint8_t dn)
{
  uint16_t at = sq.st.result_len;

  if (at + 9u + dn > SEQ_MAX_RESULT) {
    return; /* full: the status still tells how far it got */
  }
  wr16(sq.result + at, pc);
  sq.result[at + 2] = op;
  sq.result[at + 3] = status;
  wr32(sq.result + at + 4, board_time_us());
  sq.result[at + 8] = dn;
  memcpy(sq.result + at + 9, d, dn);
  sq.st.result_len = at + 9 + dn;
}

static void seq_execute(void)
{
  static uint8_t d[256];
  uint16_t pc = 0, loop_pc = 0;
  uint16_t loop_left = 0;
  bool in_loop = false;

  while (pc < sq.len) {
    int n = op_len(pc);
    const uint8_t *c = sq.code + pc;
    uint8_t op = c[0] & ~SEQ_CONTINUE;
    uint8_t dn;
    int ret;

    sq.st.pc = pc;
    if (n < 0) {
      sq.st.state = SEQ_INVALID;
      sq.st.error = PROTO_EINVAL;
      return;
    }
    if (sq.abort_req) {
      sq.st.state = SEQ_ABORTED;
      return;
    }
    if (k_uptime_get() > sq.deadline) {
      sq.st.state = SEQ_TIMEOUT;
      return;
    }
    if (op == SEQ_LOOP) {
      loop_pc = pc + n;
      loop_left = MAX(rd16(c + 1), 1);
      in_loop = true;
      pc += n;
      continue;
    }
    if (op == SEQ_ENDLOOP) {
      if (in_loop && --loop_left > 0) {
        pc = loop_pc;
      } else {
        in_loop = false;
        pc += n;
      }
      continue;
    }
    ret = exec_step(c, d, &dn);
    record(pc, c[0], (uint8_t)proto_errno_to_status(ret), d, dn);
    if (ret == -ECANCELED) {
      sq.st.state = sq.abort_req ? SEQ_ABORTED : SEQ_TIMEOUT;
      return;
    }
    if (ret != 0 && !(c[0] & SEQ_CONTINUE)) {
      sq.st.state = SEQ_FAILED;
      sq.st.error = (uint8_t)proto_errno_to_status(ret);
      return;
    }
    pc += n;
  }
  sq.st.pc = pc;
  sq.st.state = SEQ_DONE;
}

static void seq_thread(void *a, void *b, void *c)
{
  ARG_UNUSED(a);
  ARG_UNUSED(b);
  ARG_UNUSED(c);

  for (;;) {
    uint8_t evt[10];

    k_sem_take(&sq.go, K_FOREVER);
    ui_activity("SEQ run %uB", sq.len);
    led_set(LED_MAGENTA);
    seq_execute();
    led_flash(sq.st.state == SEQ_DONE ? LED_GREEN : LED_RED, 150);
    ui_activity("SEQ %s", sq.st.state == SEQ_DONE ? "done" : "stopped");
    wr32(evt, board_time_us());
    evt[4] = (uint8_t)sq.st.state;
    evt[5] = sq.st.error;
    wr16(evt + 6, sq.st.pc);
    wr16(evt + 8, sq.st.result_len);
    proto_send_event(EVT_SEQ_DONE, evt, sizeof(evt));
  }
}

int seq_load(uint16_t offset, const uint8_t *code, size_t n)
{
  if (sq.st.state == SEQ_RUNNING) {
    return -EBUSY;
  }
  if ((size_t)offset + n > SEQ_MAX_CODE) {
    return -EINVAL;
  }
  memcpy(sq.code + offset, code, n);
  return 0;
}

int seq_run(uint16_t len, uint32_t timeout_ms)
{
  if (sq.st.state == SEQ_RUNNING) {
    return -EBUSY;
  }
  if (len == 0 || len > SEQ_MAX_CODE) {
    return -EINVAL;
  }
  sq.len = len;
  sq.deadline = k_uptime_get() + (timeout_ms ? timeout_ms : 10000);
  sq.abort_req = false;
  sq.st.state = SEQ_RUNNING;
  sq.st.error = 0;
  sq.st.pc = 0;
  sq.st.result_len = 0;
  k_sem_give(&sq.go);
  return 0;
}

int seq_abort(void)
{
  if (sq.st.state == SEQ_RUNNING) {
    sq.abort_req = true;
  }
  return 0;
}

void seq_get_status(struct seq_status *out)
{
  *out = sq.st;
}

int seq_result(uint16_t offset, uint16_t n, uint8_t *out)
{
  if (sq.st.state == SEQ_RUNNING) {
    return -EBUSY;
  }
  if (offset >= sq.st.result_len) {
    return 0;
  }
  n = MIN(n, sq.st.result_len - offset);
  memcpy(out, sq.result + offset, n);
  return n;
}

void seq_init(void)
{
  k_sem_init(&sq.go, 0, 1);
  sq.st.state = SEQ_IDLE;
  k_thread_create(&seq_thread_data, seq_stack, K_THREAD_STACK_SIZEOF(seq_stack), seq_thread, NULL,
                  NULL, NULL, K_PRIO_PREEMPT(2), 0, K_NO_WAIT);
  k_thread_name_set(&seq_thread_data, "seq");
}
