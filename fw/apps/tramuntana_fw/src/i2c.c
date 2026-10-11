/*
 * Tramuntana - i2c. See i2c.h.
 *
 * Master: bit-banged on GPIO1 (SCL, DIO6) / GPIO0 (SDA, DIO7) straight
 * through the SIO registers. Open-drain is emulated the usual way: a
 * line is driven low by making the pad an output at 0, and released by
 * making it an input, so the external pull-ups (the target's, or the
 * board's own bias behind DIO_PU_ENABLE) bring it high. Clock stretching
 * is honoured by waiting for SCL to actually read high after releasing
 * it. Timing comes from board_delay_ns() (DWT cycle counter), so 100k /
 * 400k / 1M all come out close enough; the bus is never faster than
 * asked, which is what matters.
 *
 * Why not the hardware I2C0 block through Zephyr's driver: the I2CDriver
 * workflow the host API exposes (START, some bytes, a repeated START, a
 * few reads, then STOP - each as a separate call, with the bus held in
 * between) has no equivalent in the i2c_transfer() API, which always
 * runs a complete transaction. Bit-banging gives every primitive for the
 * cost of a few dozen lines, and I2C is slow enough that the CPU doesn't
 * notice.
 *
 * Sniffer: same recipe as spi.c - a PIO0 state machine watches both
 * lines and pushes one word per bus event (the SDA level on each rising
 * SCL edge, or a START/STOP code when SDA moves while SCL is high), a
 * DMA channel streams them into the arena ring and a thread decodes
 * them into EVT_I2C_MSG events: (START addr_rw)(ACK)(DATA)(ACK)...(STOP).
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/misc/pio_rpi_pico/pio_rpi_pico.h>
#include <zephyr/logging/log.h>
#include <hardware/dma.h>
#include <hardware/gpio.h>
#include <hardware/pio.h>
#include <string.h>

#undef pio0
#undef pio1

#include "i2c.h"
#include "board.h"
#include "arena.h"
#include "dmaring.h"
#include "display.h"
#include "proto.h"
#include "stamp.h"

LOG_MODULE_REGISTER(i2c_drv, LOG_LEVEL_INF);

#define SDA GPIO_I2C_SDA
#define SCL GPIO_I2C_SCL
#define I2C_STRETCH_TIMEOUT_US 10000

static struct {
  uint32_t hz;
  uint32_t half_cycles; /* half a SCL period, in sysclk cycles */
  uint32_t stretch_cycles;
  bool open;            /* START sent, no STOP yet */
  bool claimed;
  struct k_mutex lock;
} i2c;

/* --- bit-bang primitives --------------------------------------------- */

__ramfunc static void sda_low(void)
{
  gpio_set_dir(SDA, GPIO_OUT);
}

__ramfunc static void sda_release(void)
{
  gpio_set_dir(SDA, GPIO_IN);
}

__ramfunc static void scl_low(void)
{
  gpio_set_dir(SCL, GPIO_OUT);
}

__ramfunc static bool sda_read(void)
{
  return gpio_get(SDA);
}

/* Releases SCL and waits for it to actually rise (clock stretching). */
__ramfunc static int scl_release(void)
{
  uint32_t start;

  gpio_set_dir(SCL, GPIO_IN);
  start = board_cycles();
  while (!gpio_get(SCL)) {
    if ((uint32_t)(board_cycles() - start) > i2c.stretch_cycles) {
      return -ETIMEDOUT;
    }
  }
  return 0;
}

/* The high phase, counted from the moment SCL was released rather than
 * from when it read high: a slow pull-up then only shortens the time the
 * line spends at the rail, instead of adding its rise time to every
 * clock. A slave that stretches still gets all the time it wants (the
 * wait above), plus a quarter period once it lets go. */
__ramfunc static int scl_high_phase(void)
{
  uint32_t released = board_cycles();
  uint32_t seen;
  int ret = scl_release();

  if (ret != 0) {
    return ret;
  }
  seen = board_cycles();
  while ((uint32_t)(board_cycles() - released) < i2c.half_cycles ||
         (uint32_t)(board_cycles() - seen) < i2c.half_cycles / 4) {
    /* spin */
  }
  return 0;
}

__ramfunc static void tick(void)
{
  board_delay_cycles(i2c.half_cycles);
}

static int i2c_claim(void)
{
  int ret;

  if (i2c.claimed && board_pin_owner(6) == OWNER_I2C && board_pin_owner(7) == OWNER_I2C) {
    return 0;
  }
  ret = board_pins_claim(DIO_MASK_I2C, OWNER_I2C, false);
  if (ret != 0) {
    return ret;
  }
  /* board.c leaves them as inputs reading 0 when driven: exactly what
   * the open-drain emulation needs (output value stays 0 forever). */
  gpio_put(SDA, 0);
  gpio_put(SCL, 0);
  sda_release();
  gpio_set_dir(SCL, GPIO_IN);
  i2c.claimed = true;
  i2c.open = false;
  return 0;
}

/* START (or repeated START if the bus is open). */
__ramfunc static int bb_start(void)
{
  int ret;

  if (i2c.open) {
    /* Repeated START: SCL is low after the last bit; release SDA, then
     * SCL, then pull SDA low while SCL is high. */
    sda_release();
    tick();
    ret = scl_release();
    if (ret != 0) {
      return ret;
    }
    tick();
  } else {
    sda_release();
    ret = scl_release();
    if (ret != 0) {
      return ret;
    }
    tick();
  }
  sda_low();
  tick();
  scl_low();
  tick();
  i2c.open = true;
  return 0;
}

__ramfunc static int bb_stop(void)
{
  int ret;

  sda_low();
  tick();
  ret = scl_release();
  tick();
  sda_release();
  tick();
  i2c.open = false;
  return ret;
}

/* Clocks one byte out, returns the ACK bit (true = ACKed). */
__ramfunc static int bb_write_byte(uint8_t byte, bool *ack)
{
  int b, ret;

  for (b = 7; b >= 0; b--) {
    if (byte & BIT(b)) {
      sda_release();
    } else {
      sda_low();
    }
    tick();
    ret = scl_high_phase();
    if (ret != 0) {
      return ret;
    }
    scl_low();
  }
  /* ACK clock: release SDA, sample while SCL high. */
  sda_release();
  tick();
  ret = scl_high_phase();
  if (ret != 0) {
    return ret;
  }
  *ack = !sda_read();
  scl_low();
  return 0;
}

__ramfunc static int bb_read_byte(uint8_t *byte, bool ack)
{
  uint8_t v = 0;
  int b, ret;

  sda_release();
  for (b = 7; b >= 0; b--) {
    tick();
    ret = scl_high_phase();
    if (ret != 0) {
      return ret;
    }
    if (sda_read()) {
      v |= BIT(b);
    }
    scl_low();
  }
  if (ack) {
    sda_low();
  } else {
    sda_release();
  }
  tick();
  ret = scl_high_phase();
  if (ret != 0) {
    return ret;
  }
  scl_low();
  sda_release();
  *byte = v;
  return 0;
}

/* --- public master API ------------------------------------------------ */

static void i2c_ui_update(void)
{
  if (i2c.hz >= 1000000) {
    ui_set(UI_ROW_I2C, "%u.%uMHz PU %s", i2c.hz / 1000000, (i2c.hz % 1000000) / 100000,
           board_pullups_get() ? "on" : "off");
  } else {
    ui_set(UI_ROW_I2C, "%ukHz PU %s", i2c.hz / 1000, board_pullups_get() ? "on" : "off");
  }
}

int i2c_drv_conf(uint32_t hz, bool pullups, uint32_t *actual_hz)
{
  if (hz < I2C_MIN_HZ || hz > I2C_MAX_HZ) {
    return -EINVAL;
  }
  k_mutex_lock(&i2c.lock, K_FOREVER);
  i2c.hz = hz;
  /* Half period; the GPIO/loop overhead only makes the bus a bit slower. */
  i2c.half_cycles = board_ns_to_cycles(500000000u / hz);
  board_pullups_set(pullups);
  k_mutex_unlock(&i2c.lock);
  if (actual_hz != NULL) {
    *actual_hz = hz;
  }
  i2c_ui_update();
  return 0;
}

uint32_t i2c_drv_hz(void)
{
  return i2c.hz;
}

/* Wraps a bus operation: claim pins, blue LED, run, LED result. The
 * scheduler is locked while the bus is being bit-banged so the higher
 * priority capture threads (which poll every few hundred us) don't
 * stretch individual clock phases - interrupts stay on, so USB and the
 * console are unaffected, and even a 4KB transfer at 100kHz is well
 * under half a second. */
#define I2C_OP(expr)                                                       \
  ({                                                                       \
    int _ret;                                                              \
    k_mutex_lock(&i2c.lock, K_FOREVER);                                    \
    _ret = i2c_claim();                                                    \
    if (_ret == 0) {                                                       \
      led_set(LED_BLUE);                                                   \
      k_sched_lock();                                                      \
      _ret = (expr);                                                       \
      k_sched_unlock();                                                    \
      led_flash(_ret == 0 ? LED_GREEN : LED_RED, 60);                      \
    }                                                                      \
    k_mutex_unlock(&i2c.lock);                                             \
    _ret;                                                                  \
  })

__ramfunc static int op_probe(uint8_t addr, bool *ack)
{
  int ret = bb_start();

  if (ret == 0) {
    ret = bb_write_byte((uint8_t)(addr << 1), ack);
  }
  bb_stop();
  return ret;
}

int i2c_drv_scan(uint8_t bitmap[16])
{
  int ret = 0;
  uint8_t addr;
  int found = 0;

  memset(bitmap, 0, 16);
  k_mutex_lock(&i2c.lock, K_FOREVER);
  ret = i2c_claim();
  if (ret != 0) {
    k_mutex_unlock(&i2c.lock);
    return ret;
  }
  led_set(LED_BLUE);
  for (addr = 0x08; addr <= 0x77; addr++) {
    bool ack = false;

    k_sched_lock();
    ret = op_probe(addr, &ack);
    k_sched_unlock();
    if (ret != 0) {
      break;
    }
    if (ack) {
      bitmap[addr / 8] |= BIT(addr % 8);
      found++;
    }
  }
  led_flash(ret == 0 ? LED_GREEN : LED_RED, 100);
  k_mutex_unlock(&i2c.lock);
  ui_activity("I2C scan:%d", found);
  return ret;
}

__ramfunc static int op_write(uint8_t addr, const uint8_t *data, size_t n, size_t *acked, bool stop)
{
  bool ack = false;
  size_t i;
  int ret = bb_start();

  *acked = 0;
  if (ret == 0) {
    ret = bb_write_byte((uint8_t)(addr << 1), &ack);
  }
  if (ret == 0 && !ack) {
    ret = -EIO;
  }
  for (i = 0; ret == 0 && i < n; i++) {
    ret = bb_write_byte(data[i], &ack);
    if (ret == 0 && !ack) {
      ret = -EIO;
      break;
    }
    (*acked)++;
  }
  if (stop || ret != 0) {
    bb_stop();
  }
  return ret;
}

__ramfunc static int op_read(uint8_t addr, uint8_t *data, size_t n)
{
  bool ack = false;
  size_t i;
  int ret = bb_start();

  if (ret == 0) {
    ret = bb_write_byte((uint8_t)((addr << 1) | 1), &ack);
  }
  if (ret == 0 && !ack) {
    ret = -EIO;
  }
  for (i = 0; ret == 0 && i < n; i++) {
    ret = bb_read_byte(&data[i], i + 1 < n);
  }
  bb_stop();
  return ret;
}

int i2c_drv_write(uint8_t addr, const uint8_t *data, size_t n, size_t *acked)
{
  size_t dummy;
  int ret;

  if (acked == NULL) {
    acked = &dummy;
  }
  ret = I2C_OP(op_write(addr, data, n, acked, true));
  ui_activity("I2C W %02X %uB", addr, (unsigned)n);
  return ret;
}

int i2c_drv_read(uint8_t addr, uint8_t *data, size_t n)
{
  int ret = I2C_OP(op_read(addr, data, n));

  ui_activity("I2C R %02X %uB", addr, (unsigned)n);
  return ret;
}

int i2c_drv_write_read(uint8_t addr, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
  size_t acked;
  int ret = I2C_OP(({
    int _r = op_write(addr, w, wn, &acked, false);

    if (_r == 0) {
      _r = op_read(addr, r, rn); /* repeated START */
    }
    _r;
  }));

  ui_activity("I2C WR %02X", addr);
  return ret;
}

int i2c_drv_start(uint8_t addr_rw, bool *ack)
{
  return I2C_OP(({
    int _r = bb_start();

    if (_r == 0) {
      _r = bb_write_byte(addr_rw, ack);
    }
    _r;
  }));
}

int i2c_drv_stop(void)
{
  return I2C_OP(bb_stop());
}

int i2c_drv_raw_write(const uint8_t *data, size_t n, size_t *acked)
{
  size_t dummy;

  if (acked == NULL) {
    acked = &dummy;
  }
  *acked = 0;
  if (!i2c.open) {
    return -EPERM;
  }
  return I2C_OP(({
    int _r = 0;
    size_t _i;
    bool _ack;

    for (_i = 0; _r == 0 && _i < n; _i++) {
      _r = bb_write_byte(data[_i], &_ack);
      if (_r == 0 && !_ack) {
        _r = -EIO;
        break;
      }
      (*acked)++;
    }
    _r;
  }));
}

int i2c_drv_raw_read(uint8_t *data, size_t n, bool nack_last)
{
  if (!i2c.open) {
    return -EPERM;
  }
  return I2C_OP(({
    int _r = 0;
    size_t _i;

    for (_i = 0; _r == 0 && _i < n; _i++) {
      _r = bb_read_byte(&data[_i], !(nack_last && _i + 1 == n));
    }
    _r;
  }));
}

bool i2c_drv_is_open(void)
{
  return i2c.open;
}

int i2c_drv_reset(void)
{
  return I2C_OP(({
    int _i, _r = 0;

    i2c.open = false;
    sda_release();
    for (_i = 0; _i < 9 && !sda_read(); _i++) {
      scl_low();
      tick();
      _r = scl_release();
      tick();
    }
    bb_stop();
    _r;
  }));
}

/* --- sniffer --------------------------------------------------------------- */

#define EV_START 2u
#define EV_STOP  3u
#define SNIFF_RING_WORDS (ARENA_RING_BYTES / 4)
#define SNIFF_EVT_MAX    (3 + 2 * 512)

static const char *const sniff_holder = "i2c-sniff";

static struct {
  bool active;
  bool stop_req;
  bool pullups_were_on;
  uint32_t pullups_seen;   /* board_pullups_changes() after our own set */
  PIO pio;
  uint sm;
  uint offset;
  uint16_t instr[24];
  struct pio_program program;
  struct dma_ring ring;
  uint32_t rd;
  uint32_t count;
  struct k_thread thread;
  struct k_sem done;
} sniff;

K_THREAD_STACK_DEFINE(i2c_sniff_stack, 2048);

/*
 *    0  mov osr, pins       ; \
 *    1  out x, 1            ;  | SCL low: poll until it rises
 *    2  out y, 1            ;  | (x = SDA, y = SCL)
 *    3  jmp !y, 0           ; /
 *    4  mov isr, null       ; \ rising edge: push the SDA bit it
 *    5  in x, 1             ;  | sampled (x)
 *    6  push noblock        ; /
 *    7  mov osr, pins       ; \
 *    8  out y, 1            ;  | SCL high: watch SDA (y) against its
 *    9  jmp x!=y, 13        ;  | value at the edge (x) - a change here is
 *   10  out y, 1            ;  | a START/STOP - and SCL (y again) for
 *   11  jmp !y, 0           ;  | the falling edge
 *   12  jmp 7               ; /
 *   13  mov x, y            ; SDA changed: remember the new level
 *   14  jmp !x, 19          ; fell -> START
 *   15  set y, 3            ; \
 *   16  mov isr, y          ;  | rose -> push STOP
 *   17  push noblock        ;  |
 *   18  jmp 7               ; /
 *   19  irq set 0           ; stamp it (stamp.c)
 *   20  set y, 2            ; \
 *   21  mov isr, y          ;  | push START
 *   22  push noblock        ;  |
 *   23  jmp 7               ; /
 */
static void i2c_sniff_program(void)
{
  uint16_t *i = sniff.instr;

  i[0] = pio_encode_mov(pio_osr, pio_pins);
  i[1] = pio_encode_out(pio_x, 1);
  i[2] = pio_encode_out(pio_y, 1);
  i[3] = pio_encode_jmp_not_y(0);
  i[4] = pio_encode_mov(pio_isr, pio_null);
  i[5] = pio_encode_in(pio_x, 1);
  i[6] = pio_encode_push(false, false);
  i[7] = pio_encode_mov(pio_osr, pio_pins);
  i[8] = pio_encode_out(pio_y, 1);
  i[9] = pio_encode_jmp_x_ne_y(13);
  i[10] = pio_encode_out(pio_y, 1);
  i[11] = pio_encode_jmp_not_y(0);
  i[12] = pio_encode_jmp(7);
  i[13] = pio_encode_mov(pio_x, pio_y);
  i[14] = pio_encode_jmp_not_x(19);
  i[15] = pio_encode_set(pio_y, EV_STOP);
  i[16] = pio_encode_mov(pio_isr, pio_y);
  i[17] = pio_encode_push(false, false);
  i[18] = pio_encode_jmp(7);
  i[19] = pio_encode_irq_set(false, STAMP_PIO_FLAG);
  i[20] = pio_encode_set(pio_y, EV_START);
  i[21] = pio_encode_mov(pio_isr, pio_y);
  i[22] = pio_encode_push(false, false);
  i[23] = pio_encode_jmp(7);

  sniff.program = (struct pio_program){
    .instructions = sniff.instr,
    .length = 24,
    .origin = -1,
    .pio_version = 0,
    .used_gpio_ranges = BIT(0),
  };
}

static uint32_t sniff_pending(void)
{
  uint32_t wr = dma_ring_write_offset(&sniff.ring) / 4;

  return (wr - sniff.rd) % SNIFF_RING_WORDS;
}

/* Ring index of the word sniff_get() returned last. */
static uint32_t sniff_last_index;

static bool sniff_get(uint32_t *word, bool *overflow)
{
  const uint32_t *ring = (const uint32_t *)arena_ring();
  uint32_t pending = sniff_pending();

  if (pending == 0) {
    return false;
  }
  if (pending > SNIFF_RING_WORDS - 8) {
    *overflow = true;
  }
  *word = ring[sniff.rd];
  sniff_last_index = sniff.rd;
  sniff.rd = (sniff.rd + 1) % SNIFF_RING_WORDS;
  return true;
}

static int sniff_hw_start(void)
{
  const struct device *pio_dev = DEVICE_DT_GET(DT_NODELABEL(pio0));
  size_t sm;
  int ret;
  pio_sm_config c;

  if (!device_is_ready(pio_dev)) {
    return -ENODEV;
  }
  sniff.pio = pio_rpi_pico_get_pio(pio_dev);
  i2c_sniff_program();
  if (!pio_can_add_program(sniff.pio, &sniff.program)) {
    return -ENOMEM;
  }
  ret = pio_rpi_pico_allocate_sm(pio_dev, &sm);
  if (ret < 0) {
    return ret;
  }
  sniff.sm = (uint)sm;
  sniff.offset = (uint)pio_add_program(sniff.pio, &sniff.program);
  pio_sm_set_consecutive_pindirs(sniff.pio, sniff.sm, SDA, 2, false);

  c = pio_get_default_sm_config();
  sm_config_set_wrap(&c, sniff.offset, sniff.offset + sniff.program.length - 1);
  sm_config_set_in_pins(&c, SDA);              /* bit0 SDA, bit1 SCL */
  sm_config_set_in_shift(&c, false, false, 32);
  sm_config_set_out_shift(&c, true, false, 32);
  sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
  sm_config_set_clkdiv(&c, 1.0f);
  pio_sm_init(sniff.pio, sniff.sm, sniff.offset, &c);
  pio_sm_clear_fifos(sniff.pio, sniff.sm);
  sniff.pio->fdebug = 1u << (PIO_FDEBUG_RXSTALL_LSB + sniff.sm);

  ret = dma_ring_start(&sniff.ring, arena_ring(), ARENA_RING_BYTES, 4,
                       pio_get_dreq(sniff.pio, sniff.sm, false), &sniff.pio->rxf[sniff.sm]);
  if (ret != 0) {
    pio_remove_program(sniff.pio, &sniff.program, sniff.offset);
    pio_sm_unclaim(sniff.pio, sniff.sm);
    return ret;
  }
  sniff.rd = 0;
  stamp_start(sniff.pio, &sniff.ring, SNIFF_RING_WORDS);
  pio_sm_set_enabled(sniff.pio, sniff.sm, true);
  return 0;
}

static void sniff_hw_stop(void)
{
  pio_sm_set_enabled(sniff.pio, sniff.sm, false);
  stamp_stop(sniff.pio);
  dma_ring_stop(&sniff.ring);
  pio_remove_program(sniff.pio, &sniff.program, sniff.offset);
  pio_sm_unclaim(sniff.pio, sniff.sm);
}

/* Event record types (proto.h EVT_I2C_MSG). */
#define REC_START 1
#define REC_ACK   2
#define REC_DATA  3
#define REC_STOP  4

static void sniff_thread(void *a, void *b, void *c)
{
  static uint8_t evt[SNIFF_EVT_MAX];
  size_t len = 0;         /* bytes used in evt (from index 1) */
  bool in_msg = false, first_byte = false, overflow = false, truncated = false, stamped = false;
  uint32_t bits = 0, shift = 0;
  uint32_t msg_bytes = 0;

  ARG_UNUSED(a);
  ARG_UNUSED(b);
  ARG_UNUSED(c);

  while (!sniff.stop_req) {
    uint32_t ev;

    if (!sniff_get(&ev, &overflow)) {
      k_sleep(K_MSEC(1));
      continue;
    }
    if (ev == EV_START) {
      uint32_t t_start;

      /* every START (repeated ones too) has a stamp: always consume it */
      if (!stamp_for(sniff_last_index, &t_start)) {
        t_start = board_time_us();
      }
      if (!in_msg) {
        stamped = proto_event_stamps;
        len = 1;
        if (stamped) {
          wr32(evt + 1, t_start);
          len = 5;
        }
        msg_bytes = 0;
        truncated = false;
        in_msg = true;
      }
      if (len + 2 <= SNIFF_EVT_MAX) {
        evt[len++] = REC_START;
        evt[len++] = 0; /* address filled in when the byte completes */
      } else {
        truncated = true;
      }
      bits = 0;
      shift = 0;
      first_byte = true;
      continue;
    }
    if (ev == EV_STOP) {
      if (in_msg) {
        bool rxstall = (sniff.pio->fdebug & (1u << (PIO_FDEBUG_RXSTALL_LSB + sniff.sm))) != 0;

        sniff.pio->fdebug = 1u << (PIO_FDEBUG_RXSTALL_LSB + sniff.sm);
        if (len + 2 <= SNIFF_EVT_MAX) {
          evt[len++] = REC_STOP;
          evt[len++] = 0;
        }
        evt[0] = (uint8_t)((overflow || rxstall) ? 1 : 0) | (truncated ? 2 : 0) |
                 (stamped ? EVT_FLAG_STAMPED : 0);
        proto_send_event(EVT_I2C_MSG, evt, len);
        sniff.count++;
        led_flash(evt[0] ? LED_RED : LED_GREEN, 40);
        ui_activity("I2C msg %uB", (unsigned)msg_bytes);
        if (sniff.count % 10 == 1) {
          ui_set(UI_ROW_I2C, "SNIFF %u msgs", sniff.count);
        }
        overflow = false;
      }
      in_msg = false;
      bits = 0;
      continue;
    }
    if (!in_msg) {
      continue; /* bits without a START: noise, or joined mid-message */
    }
    /* Data bit. */
    if (bits < 8) {
      shift = (shift << 1) | (ev & 1);
      bits++;
      if (bits == 8) {
        if (first_byte && len >= 2) {
          evt[len - 1] = (uint8_t)shift; /* the START record's addr_rw */
        } else if (len + 2 <= SNIFF_EVT_MAX) {
          evt[len++] = REC_DATA;
          evt[len++] = (uint8_t)shift;
        } else {
          truncated = true;
        }
        msg_bytes++;
      }
    } else {
      /* 9th bit: ACK (0) / NAK (1). */
      if (len + 2 <= SNIFF_EVT_MAX) {
        evt[len++] = REC_ACK;
        evt[len++] = (ev & 1) ? 0 : 1;
      }
      bits = 0;
      shift = 0;
      first_byte = false;
    }
  }
  k_sem_give(&sniff.done);
}

int i2c_sniff_start(bool pullups)
{
  int ret;

  if (sniff.active) {
    return -EBUSY;
  }
  ret = arena_ring_claim(sniff_holder);
  if (ret != 0) {
    return ret;
  }
  /* No pin claim (see spi.c): the PIO only listens, so the bit-banged
   * master keeps working and its own frames show up in the sniffer. */
  sniff.pullups_were_on = board_pullups_get();
  if (pullups) {
    board_pullups_set(true);
  }
  sniff.pullups_seen = board_pullups_changes();
  ret = sniff_hw_start();
  if (ret != 0) {
    arena_ring_release(sniff_holder);
    return ret;
  }
  sniff.count = 0;
  sniff.stop_req = false;
  sniff.active = true;
  k_sem_init(&sniff.done, 0, 1);
  k_thread_create(&sniff.thread, i2c_sniff_stack, K_THREAD_STACK_SIZEOF(i2c_sniff_stack),
                  sniff_thread, NULL, NULL, NULL, K_PRIO_PREEMPT(6), 0, K_NO_WAIT);
  k_thread_name_set(&sniff.thread, "i2c-sniff");
  led_background(LED_BLUE);
  ui_set(UI_ROW_I2C, "SNIFF listening");
  return 0;
}

int i2c_sniff_stop(void)
{
  if (!sniff.active) {
    return 0;
  }
  sniff.stop_req = true;
  k_sem_take(&sniff.done, K_MSEC(500));
  sniff_hw_stop();
  arena_ring_release(sniff_holder);
  if (board_pullups_changes() == sniff.pullups_seen) {
    board_pullups_set(sniff.pullups_were_on);
  }
  sniff.active = false;
  led_background(LED_OFF);
  i2c_ui_update();
  return 0;
}

bool i2c_sniff_active(void)
{
  return sniff.active;
}

uint32_t i2c_sniff_count(void)
{
  return sniff.count;
}

void i2c_drv_init(void)
{
  k_mutex_init(&i2c.lock);
  i2c.hz = 100000;
  i2c.half_cycles = board_ns_to_cycles(500000000u / i2c.hz);
  i2c.stretch_cycles = board_ns_to_cycles(I2C_STRETCH_TIMEOUT_US * 1000u);
  i2c_ui_update();
}

void i2c_ui_refresh(void)
{
  if (!sniff.active) {
    i2c_ui_update();
  }
}
