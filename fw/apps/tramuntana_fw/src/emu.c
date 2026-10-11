/*
 * Tramuntana - emu. See emu.h.
 *
 * Everything runs on PIO1 (PIO0 belongs to the sniffers and the logic
 * analyzer), with one interrupt line (PIO1_IRQ_0) into emu_isr().
 *
 * I2C, two state machines sharing the open-drain pins (output latch 0,
 * a line is pulled low by making it an output):
 *
 *   'bytes' (sm_a) - shifts 9-bit frames, never looks for START/STOP:
 *
 *      0  set y, 7
 *      1  out pindirs, 1     ; bit to drive (0 = release): data when the
 *                            ;   board transmits, 0 when it receives
 *      2  wait 1 gpio SCL
 *      3  in pins, 1         ; SDA as seen on the bus
 *      4  wait 0 gpio SCL
 *      5  jmp y-- 1
 *      6  set pindirs, 1     ; 8 bits done: hold SCL low (stretch)
 *      7  push block         ;   hand the byte to the CPU
 *      8  pull block         ;   and wait for its answer: bit 0 = ACK,
 *      9  out pindirs, 1     ;   bits 1..8 = next byte to transmit
 *     10  set pindirs, 0     ; release SCL: 9th clock
 *     11  wait 1 gpio SCL
 *     12  jmp pin 14         ; SDA high on the 9th clock = NAK: done
 *     13  wait 0 gpio SCL    ; (wrap to 0)
 *     14  jmp 14             ; parked until the CPU restarts it
 *
 *   SCL is only ever released at 10, right after this machine's own
 *   stretch: the pin direction is shared by both machines (last write
 *   wins), so releasing it on every bit - as an earlier version did -
 *   cancelled the stretch 'conditions' puts on SCL after a START, and a
 *   fast master then clocked the address in before the CPU had restarted
 *   this machine.
 *
 *   'conditions' (sm_b) - START / STOP detector (SDA moving while SCL is
 *   high). After a START it holds SCL low until the CPU has restarted
 *   'bytes' at the address byte, so a repeated START can't misalign it
 *   however fast the bus is:
 *
 *      0  irq set 1          ; STOP
 *      1  wait 1 gpio SDA    ; (entry)
 *      2  wait 0 gpio SDA    ; SDA fell...       <- wrap bottom
 *      3  jmp pin 6          ;   ...with SCL high: START
 *      4  wait 1 gpio SDA    ; SDA rose...
 *      5  jmp pin 0          ;   ...with SCL high: STOP, else (wrap) 2
 *      6  wait 0 gpio SCL
 *      7  set pindirs, 1     ; hold SCL
 *      8  irq wait 0         ; CPU: restart 'bytes', clear the flag
 *      9  set pindirs, 0
 *     10  wait 1 gpio SCL    ; let the first address bit's clock go by:
 *     11  wait 0 gpio SCL    ;   no START/STOP can happen inside it
 *     12  mov osr, pins      ; \ then pick up from SDA's real level:
 *     13  out x, 1           ;  | the master set the first bit while SCL
 *     14  jmp !x 4           ;  | was held, so watching for an SDA edge
 *     15  jmp 2              ; /  from 4 would mistake that for a STOP
 *
 *   (12..15 matter for fast masters: an Arduino at 400 kHz changes SDA
 *   0.5 us after the START's SCL edge, before the ISR has answered.)
 *
 * The CPU answers each byte from the ISR: whether to ACK it and, when
 * the target is reading, which byte comes next. The stretch covers the
 * ISR latency, so timing never depends on it.
 *
 * SPI, three state machines and two DMA channels:
 *
 *   'cs' (sm_c): wait 0 gpio CS / wait 1 gpio CS / irq set 0 - the end
 *   of a transaction, where the CPU applies writes and rearms the rest.
 *   'rx' (sm_a): MOSI, 8 bits per word into the RX FIFO, for the CPU.
 *   'tx' (sm_b): drives MISO. It shifts out 'status' while the command
 *   byte comes in, sampling MOSI itself; then it builds the address of
 *   that command's row in spi_table (X holds the table base >> 13, the
 *   command lands below it, 'in null, 5' scales by the 32-byte row) and
 *   pushes it. DMA channel A moves that word into channel B's
 *   READ_ADDR_TRIG, so channel B starts copying the row into the TX
 *   FIFO with no CPU involved, and 'tx' carries on shifting from it.
 *   spi_table[cmd] holds what a read command answers (regs from the
 *   register it addresses) and is kept in step with the register file.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/irq.h>
#include <zephyr/drivers/misc/pio_rpi_pico/pio_rpi_pico.h>
#include <zephyr/logging/log.h>
#include <hardware/dma.h>
#include <hardware/gpio.h>
#include <hardware/pio.h>
#include <string.h>

#undef pio0
#undef pio1

#include "emu.h"
#include "board.h"
#include "display.h"
#include "proto.h"

LOG_MODULE_REGISTER(emu, LOG_LEVEL_INF);

#define EMU_IRQ        PIO1_IRQ_0
#define EMU_IRQ_PRIO   1   /* same as stamp.c's PIO0 one */
#define FLAG_START     0   /* PIO irq flags */
#define FLAG_STOP      1
#define FLAG_CS_UP     0

#define SPI_TABLE_BYTES (256 * EMU_SPI_BURST)

static const struct device *const pio_dev = DEVICE_DT_GET(DT_NODELABEL(pio1));

static uint8_t regs[EMU_REGS];
static uint8_t ro_mask[EMU_REGS / 8];
static uint8_t spi_table[SPI_TABLE_BYTES] __aligned(SPI_TABLE_BYTES);

enum i2c_state {
  ST_IDLE,      /* no START seen: NAK everything */
  ST_ADDR,      /* next byte is the address */
  ST_WREG,      /* next byte is the register pointer */
  ST_WDATA,
  ST_READ,
  ST_IGNORE,    /* another device's transfer */
};

struct emu_log {
  uint32_t t_us;
  uint8_t kind;
  uint8_t flags;
  uint8_t addr;
  uint8_t reg;
  uint8_t n;
  uint8_t data[EMU_LOG_MAX];
};

#define LOG_READ      BIT(0)
#define LOG_NAK       BIT(1)
#define LOG_TRUNCATED BIT(2)
#define LOG_NO_START  BIT(3)   /* NAKed byte that came without a START */

K_MSGQ_DEFINE(emu_q, sizeof(struct emu_log), 16, 4);
K_THREAD_STACK_DEFINE(emu_stack, 1024);
static struct k_thread emu_thread_data;

struct prog {
  uint16_t instr[18];
  struct pio_program p;
  uint offset;
  bool loaded;
};

static struct {
  enum emu_kind kind;
  PIO pio;
  uint sm_a, sm_b, sm_c;
  uint8_t sm_claimed;    /* bit 0 a, 1 b, 2 c */
  struct prog prog_a, prog_b, prog_c;
  uint8_t pins;
  bool pullups_were_on;
  uint32_t pullups_seen;   /* board_pullups_changes() after our own set */
  /* i2c */
  uint8_t addr;
  enum i2c_state state;
  /* spi */
  struct emu_spi_params sp;
  int dma_a, dma_b;
  bool have_cmd;
  bool spi_read;
  /* current transfer */
  uint8_t ptr;
  uint8_t reg;
  uint16_t n;
  uint8_t data[EMU_LOG_MAX];
  uint32_t t_start;
  struct emu_stats st;
} emu;

static struct k_mutex emu_lock;

/* --- register file ------------------------------------------------------------ */

static inline bool reg_ro(uint8_t r)
{
  return (ro_mask[r >> 3] >> (r & 7)) & 1;
}

static inline bool spi_cmd_is_read(uint8_t cmd)
{
  return (cmd & emu.sp.read_mask) == emu.sp.read_value;
}

/* Rebuilds the table rows whose 32-byte window overlaps registers
 * r0..r0+n-1 (all of them for n >= 256). */
static void spi_table_update(uint8_t r0, uint32_t n)
{
  int c;

  for (c = 0; c < 256; c++) {
    uint8_t *row = &spi_table[c * EMU_SPI_BURST];
    uint8_t base = (uint8_t)c & emu.sp.addr_mask;
    int i;

    if (!spi_cmd_is_read((uint8_t)c)) {
      if (n >= EMU_REGS) {
        memset(row, 0xFF, EMU_SPI_BURST);
      }
      continue;
    }
    if (n < EMU_REGS && (uint8_t)(r0 - base) >= EMU_SPI_BURST && (uint8_t)(base - r0) >= n) {
      continue;
    }
    for (i = 0; i < EMU_SPI_BURST; i++) {
      row[i] = regs[(uint8_t)(base + i)];
    }
  }
}

void emu_regs_set(uint8_t offset, const uint8_t *data, size_t n)
{
  unsigned int key;
  size_t i;

  key = irq_lock();
  for (i = 0; i < n; i++) {
    regs[(uint8_t)(offset + i)] = data[i];
  }
  if (emu.kind == EMU_SPI) {
    spi_table_update(offset, MIN(n, (size_t)EMU_REGS));
  }
  irq_unlock(key);
}

void emu_regs_get(uint8_t offset, uint8_t *out, size_t n)
{
  size_t i;

  for (i = 0; i < n; i++) {
    out[i] = regs[(uint8_t)(offset + i)];
  }
}

void emu_ro_set(const uint8_t mask[32])
{
  memcpy(ro_mask, mask, sizeof(ro_mask));
}

/* --- transfer log (ISR -> thread -> EVT_EMU) ------------------------------------- */

static void log_transfer(bool read, bool nak)
{
  struct emu_log l;
  uint16_t i, keep = MIN(emu.n, (uint16_t)EMU_LOG_MAX);

  l.t_us = emu.t_start;
  l.kind = (uint8_t)emu.kind;
  l.flags = (read ? LOG_READ : 0) | (nak ? LOG_NAK : 0) | (emu.n > EMU_LOG_MAX ? LOG_TRUNCATED : 0);
  l.addr = emu.kind == EMU_I2C ? emu.addr : 0;
  l.reg = emu.reg;
  l.n = (uint8_t)keep;
  for (i = 0; i < keep; i++) {
    l.data[i] = read ? regs[(uint8_t)(emu.reg + i)] : emu.data[i];
  }
  (void)k_msgq_put(&emu_q, &l, K_NO_WAIT);
}

/* An I2C address byte the emulator didn't answer: another device's, or one
 * that arrived without a START (bus out of step). addr = byte >> 1, reg =
 * the raw byte. */
static void log_nak(uint8_t b, bool no_start)
{
  struct emu_log l = {
    .t_us = board_time_us(), .kind = EMU_I2C, .flags = LOG_NAK | (no_start ? LOG_NO_START : 0),
    .addr = b >> 1, .reg = b, .n = 0,
  };

  (void)k_msgq_put(&emu_q, &l, K_NO_WAIT);
}

static void emu_thread(void *a, void *b, void *c)
{
  ARG_UNUSED(a);
  ARG_UNUSED(b);
  ARG_UNUSED(c);

  for (;;) {
    struct emu_log l;
    uint8_t evt[8 + EMU_LOG_MAX];

    /* k_msgq_purge() (emu_stop) wakes a waiting reader with -ENOMSG */
    if (k_msgq_get(&emu_q, &l, K_FOREVER) != 0) {
      continue;
    }
    wr32(evt, l.t_us);
    evt[4] = l.kind;
    evt[5] = l.flags;
    evt[6] = l.addr;
    evt[7] = l.reg;
    memcpy(evt + 8, l.data, l.n);
    proto_send_event(EVT_EMU, evt, 8 + l.n);
    ui_activity("EMU %c %02X %uB", (l.flags & LOG_READ) ? 'R' : 'W', l.reg, l.n);
  }
}

/* --- I2C ---------------------------------------------------------------------- */

/* Answer word for 'bytes': bit 0 = ACK, bits 1..8 = the next byte to
 * transmit, MSB first, as pin directions (1 = pull low = a 0 bit). */
static inline uint32_t i2c_answer(bool ack, int tx)
{
  uint32_t w = ack ? 1 : 0;
  int i;

  if (tx >= 0) {
    for (i = 0; i < 8; i++) {
      if (!((tx >> (7 - i)) & 1)) {
        w |= BIT(1 + i);
      }
    }
  }
  return w;
}

static uint32_t i2c_on_byte(uint8_t b)
{
  switch (emu.state) {
  case ST_ADDR:
    emu.t_start = board_time_us();
    if ((b >> 1) != emu.addr) {
      emu.state = ST_IGNORE;
      log_nak(b, false);
      return i2c_answer(false, -1);
    }
    emu.st.txns++;
    emu.n = 0;
    emu.reg = emu.ptr;
    if (b & 1) {
      emu.state = ST_READ;
      return i2c_answer(true, regs[emu.ptr++]);
    }
    emu.state = ST_WREG;
    return i2c_answer(true, -1);
  case ST_WREG:
    emu.ptr = b;
    emu.reg = b;
    emu.state = ST_WDATA;
    return i2c_answer(true, -1);
  case ST_WDATA:
    if (!reg_ro(emu.ptr)) {
      regs[emu.ptr] = b;
    }
    if (emu.n < EMU_LOG_MAX) {
      emu.data[emu.n] = b;
    }
    emu.n++;
    emu.ptr++;
    emu.st.writes++;
    return i2c_answer(true, -1);
  case ST_READ:
    /* 'b' is the byte just sent, read back from the bus; queue the next
     * one (the master may NAK and never clock it: see i2c_finish()). */
    emu.n++;
    emu.st.reads++;
    return i2c_answer(false, regs[emu.ptr++]);
  case ST_IDLE:
    log_nak(b, true);
    return i2c_answer(false, -1);
  default:
    return i2c_answer(false, -1);
  }
}

/* End of a transfer (STOP or repeated START): report it. */
static void i2c_finish(void)
{
  if (emu.state == ST_READ) {
    emu.ptr = (uint8_t)(emu.reg + emu.n);  /* undo the prefetch */
    log_transfer(true, false);
  } else if (emu.state == ST_WDATA && emu.n > 0) {
    log_transfer(false, false);
  } else if (emu.state == ST_IGNORE) {
    emu.st.naks++;
  }
}

static void i2c_restart_bytes(void)
{
  PIO pio = emu.pio;
  uint sm = emu.sm_a;

  pio_sm_set_enabled(pio, sm, false);
  pio_sm_clear_fifos(pio, sm);
  pio_sm_restart(pio, sm);
  pio_sm_exec(pio, sm, pio_encode_mov(pio_osr, pio_null));
  pio_sm_exec(pio, sm, pio_encode_out(pio_pindirs, 1));    /* release SDA */
  pio_sm_exec(pio, sm, pio_encode_mov(pio_isr, pio_null));
  pio_sm_exec(pio, sm, pio_encode_jmp(emu.prog_a.offset));
  pio_sm_set_enabled(pio, sm, true);
}

static void i2c_service(void)
{
  PIO pio = emu.pio;

  while (!pio_sm_is_rx_fifo_empty(pio, emu.sm_a)) {
    uint8_t b = (uint8_t)pio_sm_get(pio, emu.sm_a);

    emu.st.bytes++;
    pio_sm_put(pio, emu.sm_a, i2c_on_byte(b));
  }
  /* A STOP can only be pending ahead of a START (the START handshake
   * holds SCL until it's served), so handle it first. */
  if (pio_interrupt_get(pio, FLAG_STOP)) {
    emu.st.stops++;
    i2c_finish();
    emu.state = ST_IDLE;
    i2c_restart_bytes();
    pio_interrupt_clear(pio, FLAG_STOP);
  }
  if (pio_interrupt_get(pio, FLAG_START)) {
    emu.st.starts++;
    i2c_finish();
    emu.state = ST_ADDR;
    i2c_restart_bytes();
    pio_interrupt_clear(pio, FLAG_START);   /* lets 'conditions' release SCL */
  }
}

static void i2c_programs(uint scl, uint sda)
{
  uint16_t *a = emu.prog_a.instr;
  uint16_t *b = emu.prog_b.instr;

  a[0] = pio_encode_set(pio_y, 7);
  a[1] = pio_encode_out(pio_pindirs, 1);
  a[2] = pio_encode_wait_gpio(true, scl);
  a[3] = pio_encode_in(pio_pins, 1);
  a[4] = pio_encode_wait_gpio(false, scl);
  a[5] = pio_encode_jmp_y_dec(1);
  a[6] = pio_encode_set(pio_pindirs, 1);
  a[7] = pio_encode_push(false, true);
  a[8] = pio_encode_pull(false, true);
  a[9] = pio_encode_out(pio_pindirs, 1);
  a[10] = pio_encode_set(pio_pindirs, 0);
  a[11] = pio_encode_wait_gpio(true, scl);
  a[12] = pio_encode_jmp_pin(14);
  a[13] = pio_encode_wait_gpio(false, scl);
  a[14] = pio_encode_jmp(14);
  emu.prog_a.p = (struct pio_program){ .instructions = a, .length = 15, .origin = -1,
                                       .pio_version = 0, .used_gpio_ranges = BIT(0) };

  b[0] = pio_encode_irq_set(false, FLAG_STOP);
  b[1] = pio_encode_wait_gpio(true, sda);
  b[2] = pio_encode_wait_gpio(false, sda);
  b[3] = pio_encode_jmp_pin(6);
  b[4] = pio_encode_wait_gpio(true, sda);
  b[5] = pio_encode_jmp_pin(0);
  b[6] = pio_encode_wait_gpio(false, scl);
  b[7] = pio_encode_set(pio_pindirs, 1);
  b[8] = pio_encode_irq_wait(false, FLAG_START);
  b[9] = pio_encode_set(pio_pindirs, 0);
  b[10] = pio_encode_wait_gpio(true, scl);
  b[11] = pio_encode_wait_gpio(false, scl);
  b[12] = pio_encode_mov(pio_osr, pio_pins);
  b[13] = pio_encode_out(pio_x, 1);
  b[14] = pio_encode_jmp_not_x(4);
  b[15] = pio_encode_jmp(2);
  emu.prog_b.p = (struct pio_program){ .instructions = b, .length = 16, .origin = -1,
                                       .pio_version = 0, .used_gpio_ranges = BIT(0) };
}

/* --- SPI ---------------------------------------------------------------------- */

static void spi_on_byte(uint8_t b)
{
  if (!emu.have_cmd) {
    emu.have_cmd = true;
    emu.t_start = board_time_us();
    emu.spi_read = spi_cmd_is_read(b);
    emu.reg = b & emu.sp.addr_mask;
    emu.n = 0;
    emu.st.txns++;
    return;
  }
  if (emu.spi_read) {
    emu.st.reads++;
  } else {
    uint8_t r = (uint8_t)(emu.reg + emu.n);

    if (!reg_ro(r)) {
      regs[r] = b;
    }
    if (emu.n < EMU_LOG_MAX) {
      emu.data[emu.n] = b;
    }
    emu.st.writes++;
  }
  emu.n++;
}

static void spi_finish(void)
{
  if (!emu.have_cmd) {
    return;
  }
  if (!emu.spi_read && emu.n > 0) {
    spi_table_update(emu.reg, emu.n);
  }
  if (emu.n > 0 || !emu.spi_read) {
    log_transfer(emu.spi_read, false);
  }
  emu.have_cmd = false;
}

/* Back to "waiting for CS": both shifters restarted, the lookup DMA
 * rearmed, the status byte queued for the next command byte. */
static void spi_rearm(void)
{
  PIO pio = emu.pio;
  uint32_t mask = BIT(emu.sm_a) | BIT(emu.sm_b);

  pio_set_sm_mask_enabled(pio, mask, false);
  dma_channel_abort(emu.dma_a);
  dma_channel_abort(emu.dma_b);
  pio_sm_clear_fifos(pio, emu.sm_a);
  pio_sm_clear_fifos(pio, emu.sm_b);
  pio_sm_restart(pio, emu.sm_a);
  pio_sm_restart(pio, emu.sm_b);
  pio_sm_exec(pio, emu.sm_b, pio_encode_set(pio_pindirs, 0));  /* MISO released */
  pio_sm_exec(pio, emu.sm_a, pio_encode_jmp(emu.prog_a.offset));
  pio_sm_exec(pio, emu.sm_b, pio_encode_jmp(emu.prog_b.offset));
  dma_channel_set_read_addr(emu.dma_a, &pio->rxf[emu.sm_b], false);
  dma_channel_set_trans_count(emu.dma_a, 1, true);
  pio_sm_put(pio, emu.sm_b, emu.sp.status * 0x01010101u);
  pio_set_sm_mask_enabled(pio, mask, true);
}

static void spi_service(void)
{
  PIO pio = emu.pio;

  while (!pio_sm_is_rx_fifo_empty(pio, emu.sm_a)) {
    emu.st.bytes++;
    spi_on_byte((uint8_t)pio_sm_get(pio, emu.sm_a));
  }
  if (pio_interrupt_get(pio, FLAG_CS_UP)) {
    emu.st.starts++;
    pio_interrupt_clear(pio, FLAG_CS_UP);
    spi_finish();
    spi_rearm();
  }
}

static void spi_programs(void)
{
  const struct emu_spi_params *p = &emu.sp;
  uint cs = DIO_GPIO(p->cs), sck = DIO_GPIO(p->sck);
  bool idle = (p->mode & 2) != 0;    /* CPOL */
  bool cpha = (p->mode & 1) != 0;
  uint16_t *a = emu.prog_a.instr, *b = emu.prog_b.instr, *c = emu.prog_c.instr;
  int n = 0;

  /* rx: entry waits for CS, then samples MOSI on the right edge forever
   * (wrap 1..3); the CPU restarts it after every transaction. */
  a[0] = pio_encode_wait_gpio(false, cs);
  a[1] = pio_encode_wait_gpio(cpha ? !idle : idle, sck);
  a[2] = pio_encode_wait_gpio(cpha ? idle : !idle, sck);
  a[3] = pio_encode_in(pio_pins, 1);
  emu.prog_a.p = (struct pio_program){ .instructions = a, .length = 4, .origin = -1,
                                       .pio_version = 0, .used_gpio_ranges = BIT(0) };

  /* tx: see the top of the file. CPHA 0 puts a bit out before the
   * leading edge and samples on it; CPHA 1 changes on the leading edge
   * and samples on the trailing one. */
  b[n++] = pio_encode_wait_gpio(false, cs);
  b[n++] = pio_encode_set(pio_pindirs, 1);
  b[n++] = pio_encode_pull(false, true);           /* status byte */
  b[n++] = pio_encode_mov(pio_isr, pio_x);         /* table base >> 13 */
  b[n++] = pio_encode_set(pio_y, 7);
  if (!cpha) {
    b[n++] = pio_encode_out(pio_pins, 1);          /* 5 */
    b[n++] = pio_encode_wait_gpio(!idle, sck);
    b[n++] = pio_encode_in(pio_pins, 1);
    b[n++] = pio_encode_wait_gpio(idle, sck);
  } else {
    b[n++] = pio_encode_wait_gpio(!idle, sck);     /* 5 */
    b[n++] = pio_encode_out(pio_pins, 1);
    b[n++] = pio_encode_wait_gpio(idle, sck);
    b[n++] = pio_encode_in(pio_pins, 1);
  }
  b[n++] = pio_encode_jmp_y_dec(5);
  b[n++] = pio_encode_in(pio_null, 5);             /* row address = base | cmd << 5 */
  b[n++] = pio_encode_push(false, true);
  b[n++] = pio_encode_pull(false, true);           /* 12: wrap bottom, one row byte */
  b[n++] = pio_encode_set(pio_y, 7);
  if (!cpha) {
    b[n++] = pio_encode_out(pio_pins, 1);          /* 14 */
    b[n++] = pio_encode_wait_gpio(!idle, sck);
    b[n++] = pio_encode_wait_gpio(idle, sck);
  } else {
    b[n++] = pio_encode_wait_gpio(!idle, sck);     /* 14 */
    b[n++] = pio_encode_out(pio_pins, 1);
    b[n++] = pio_encode_wait_gpio(idle, sck);
  }
  b[n++] = pio_encode_jmp_y_dec(14);               /* 17: wrap top */
  emu.prog_b.p = (struct pio_program){ .instructions = b, .length = (uint8_t)n, .origin = -1,
                                       .pio_version = 0, .used_gpio_ranges = BIT(0) };

  c[0] = pio_encode_wait_gpio(false, cs);
  c[1] = pio_encode_wait_gpio(true, cs);
  c[2] = pio_encode_irq_set(false, FLAG_CS_UP);
  emu.prog_c.p = (struct pio_program){ .instructions = c, .length = 3, .origin = -1,
                                       .pio_version = 0, .used_gpio_ranges = BIT(0) };
}

/* --- common ------------------------------------------------------------------- */

static void emu_isr(const void *arg)
{
  ARG_UNUSED(arg);
  if (emu.kind == EMU_I2C) {
    i2c_service();
  } else if (emu.kind == EMU_SPI) {
    spi_service();
  }
}

static int prog_load(struct prog *pr)
{
  if (!pio_can_add_program(emu.pio, &pr->p)) {
    return -ENOMEM;
  }
  pr->offset = (uint)pio_add_program(emu.pio, &pr->p);
  pr->loaded = true;
  return 0;
}

static void prog_unload(struct prog *pr)
{
  if (pr->loaded) {
    pio_remove_program(emu.pio, &pr->p, pr->offset);
    pr->loaded = false;
  }
}

static int sm_alloc(uint *sm, int which)
{
  size_t s;
  int ret = pio_rpi_pico_allocate_sm(pio_dev, &s);

  if (ret == 0) {
    *sm = (uint)s;
    emu.sm_claimed |= BIT(which);
  }
  return ret;
}

/* Undoes whatever emu_*_start() got to; safe on a partial start. */
static void teardown(void)
{
  PIO pio = emu.pio;

  irq_disable(EMU_IRQ);
  if (pio != NULL) {
    uint32_t gpios = 0;
    int i;

    pio_set_irq0_source_mask_enabled(pio, 0xFFFFFFFFu, false);
    pio_set_sm_mask_enabled(pio, 0xF, false);
    pio_interrupt_clear(pio, FLAG_START);
    pio_interrupt_clear(pio, FLAG_STOP);
    /* Leave every pin released in PIO1's latch, so the next claim can't
     * glitch it low before the new programs set their directions. */
    for (i = 0; i < DIO_COUNT; i++) {
      if (emu.pins & BIT(i)) {
        gpios |= BIT(DIO_GPIO(i));
      }
    }
    if ((emu.sm_claimed & BIT(0)) && gpios != 0) {
      pio_sm_set_pindirs_with_mask(pio, emu.sm_a, 0, gpios);
    }
    prog_unload(&emu.prog_a);
    prog_unload(&emu.prog_b);
    prog_unload(&emu.prog_c);
    if (emu.sm_claimed & BIT(0)) {
      pio_sm_unclaim(pio, emu.sm_a);
    }
    if (emu.sm_claimed & BIT(1)) {
      pio_sm_unclaim(pio, emu.sm_b);
    }
    if (emu.sm_claimed & BIT(2)) {
      pio_sm_unclaim(pio, emu.sm_c);
    }
    emu.sm_claimed = 0;
  }
  if (emu.dma_a >= 0) {
    dma_channel_abort(emu.dma_a);
    dma_channel_unclaim(emu.dma_a);
    emu.dma_a = -1;
  }
  if (emu.dma_b >= 0) {
    dma_channel_abort(emu.dma_b);
    dma_channel_unclaim(emu.dma_b);
    emu.dma_b = -1;
  }
  if (emu.pins != 0) {
    board_pins_release(emu.pins, OWNER_EMU);
    emu.pins = 0;
  }
  /* Put the pull-ups back - unless someone changed them meanwhile. */
  if (emu.kind == EMU_I2C && board_pullups_changes() == emu.pullups_seen) {
    board_pullups_set(emu.pullups_were_on);
  }
  emu.kind = EMU_OFF;
  k_msgq_purge(&emu_q);
}

static int claim_common(uint8_t pins, enum emu_kind kind)
{
  int ret;

  if (!device_is_ready(pio_dev)) {
    return -ENODEV;
  }
  ret = board_pins_claim(pins, OWNER_EMU, true);
  if (ret != 0) {
    return ret;
  }
  emu.pins = pins;
  emu.kind = kind;
  emu.pio = pio_rpi_pico_get_pio(pio_dev);
  memset(&emu.st, 0, sizeof(emu.st));
  emu.st.kind = kind;
  emu.ptr = 0;
  emu.n = 0;
  return 0;
}

int emu_i2c_start(uint8_t addr, uint8_t scl_dio, uint8_t sda_dio, bool pullups)
{
  uint scl, sda;
  uint32_t pins_mask;
  pio_sm_config c;
  int ret;

  if (addr > 0x7F || scl_dio >= DIO_COUNT || sda_dio >= DIO_COUNT || scl_dio == sda_dio) {
    return -EINVAL;
  }
  k_mutex_lock(&emu_lock, K_FOREVER);
  if (emu.kind != EMU_OFF) {
    teardown();
  }
  ret = claim_common(BIT(scl_dio) | BIT(sda_dio), EMU_I2C);
  if (ret != 0) {
    k_mutex_unlock(&emu_lock);
    return ret;
  }
  scl = DIO_GPIO(scl_dio);
  sda = DIO_GPIO(sda_dio);
  pins_mask = BIT(scl) | BIT(sda);
  emu.addr = addr;
  emu.state = ST_IDLE;
  emu.pullups_were_on = board_pullups_get();
  if (pullups) {
    board_pullups_set(true);
  }
  emu.pullups_seen = board_pullups_changes();

  i2c_programs(scl, sda);
  ret = sm_alloc(&emu.sm_a, 0);
  if (ret == 0) {
    ret = sm_alloc(&emu.sm_b, 1);
  }
  if (ret == 0) {
    ret = prog_load(&emu.prog_a);
  }
  if (ret == 0) {
    ret = prog_load(&emu.prog_b);
  }
  if (ret != 0) {
    teardown();
    k_mutex_unlock(&emu_lock);
    return ret;
  }

  /* Both lines: output latch 0, released (input). */
  pio_sm_set_pins_with_mask(emu.pio, emu.sm_a, 0, pins_mask);
  pio_sm_set_pindirs_with_mask(emu.pio, emu.sm_a, 0, pins_mask);

  c = pio_get_default_sm_config();
  sm_config_set_wrap(&c, emu.prog_a.offset, emu.prog_a.offset + 13);
  sm_config_set_out_pins(&c, sda, 1);
  sm_config_set_set_pins(&c, scl, 1);
  sm_config_set_in_pins(&c, sda);
  sm_config_set_jmp_pin(&c, sda);
  sm_config_set_in_shift(&c, false, false, 32);
  sm_config_set_out_shift(&c, true, false, 32);
  sm_config_set_clkdiv(&c, 1.0f);
  pio_sm_init(emu.pio, emu.sm_a, emu.prog_a.offset, &c);

  c = pio_get_default_sm_config();
  sm_config_set_wrap(&c, emu.prog_b.offset + 2, emu.prog_b.offset + 5);
  sm_config_set_set_pins(&c, scl, 1);
  sm_config_set_in_pins(&c, sda);
  sm_config_set_out_shift(&c, true, false, 32);
  sm_config_set_jmp_pin(&c, scl);
  sm_config_set_clkdiv(&c, 1.0f);
  pio_sm_init(emu.pio, emu.sm_b, emu.prog_b.offset + 1, &c);

  pio_interrupt_clear(emu.pio, FLAG_START);
  pio_interrupt_clear(emu.pio, FLAG_STOP);
  pio_set_irq0_source_enabled(emu.pio, pio_get_rx_fifo_not_empty_interrupt_source(emu.sm_a), true);
  pio_set_irq0_source_enabled(emu.pio, (pio_interrupt_source_t)(pis_interrupt0 + FLAG_START), true);
  pio_set_irq0_source_enabled(emu.pio, (pio_interrupt_source_t)(pis_interrupt0 + FLAG_STOP), true);
  i2c_restart_bytes();
  pio_sm_set_enabled(emu.pio, emu.sm_b, true);
  irq_enable(EMU_IRQ);
  k_mutex_unlock(&emu_lock);
  ui_activity("EMU I2C %02X", addr);
  return 0;
}

int emu_spi_start(const struct emu_spi_params *p)
{
  uint8_t pins;
  pio_sm_config c;
  dma_channel_config dc;
  uint miso;
  int ret;

  if (p->mode > 3 || p->cs >= DIO_COUNT || p->sck >= DIO_COUNT || p->mosi >= DIO_COUNT ||
      p->miso >= DIO_COUNT) {
    return -EINVAL;
  }
  pins = BIT(p->cs) | BIT(p->sck) | BIT(p->mosi) | BIT(p->miso);
  if (__builtin_popcount(pins) != 4) {
    return -EINVAL;
  }
  k_mutex_lock(&emu_lock, K_FOREVER);
  if (emu.kind != EMU_OFF) {
    teardown();
  }
  ret = claim_common(pins, EMU_SPI);
  if (ret != 0) {
    k_mutex_unlock(&emu_lock);
    return ret;
  }
  emu.sp = *p;
  emu.have_cmd = false;
  miso = DIO_GPIO(p->miso);
  spi_programs();
  spi_table_update(0, EMU_REGS);

  ret = sm_alloc(&emu.sm_a, 0);
  if (ret == 0) {
    ret = sm_alloc(&emu.sm_b, 1);
  }
  if (ret == 0) {
    ret = sm_alloc(&emu.sm_c, 2);
  }
  if (ret == 0) {
    ret = prog_load(&emu.prog_a);
  }
  if (ret == 0) {
    ret = prog_load(&emu.prog_b);
  }
  if (ret == 0) {
    ret = prog_load(&emu.prog_c);
  }
  if (ret == 0) {
    emu.dma_a = dma_claim_unused_channel(false);
    emu.dma_b = dma_claim_unused_channel(false);
    if (emu.dma_a < 0 || emu.dma_b < 0) {
      ret = -EBUSY;
    }
  }
  if (ret != 0) {
    teardown();
    k_mutex_unlock(&emu_lock);
    return ret;
  }

  pio_sm_set_pins_with_mask(emu.pio, emu.sm_b, 0, BIT(miso));
  pio_sm_set_pindirs_with_mask(emu.pio, emu.sm_b, 0, BIT(miso));

  /* rx */
  c = pio_get_default_sm_config();
  sm_config_set_wrap(&c, emu.prog_a.offset + 1, emu.prog_a.offset + 3);
  sm_config_set_in_pins(&c, DIO_GPIO(p->mosi));
  sm_config_set_in_shift(&c, false, true, 8);
  sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
  sm_config_set_clkdiv(&c, 1.0f);
  pio_sm_init(emu.pio, emu.sm_a, emu.prog_a.offset, &c);

  /* tx: X = table base >> 13, loaded through the FIFO before it starts */
  c = pio_get_default_sm_config();
  sm_config_set_wrap(&c, emu.prog_b.offset + 12, emu.prog_b.offset + emu.prog_b.p.length - 1);
  sm_config_set_out_pins(&c, miso, 1);
  sm_config_set_set_pins(&c, miso, 1);
  sm_config_set_in_pins(&c, DIO_GPIO(p->mosi));
  sm_config_set_in_shift(&c, false, false, 32);
  sm_config_set_out_shift(&c, false, false, 32);
  sm_config_set_clkdiv(&c, 1.0f);
  pio_sm_init(emu.pio, emu.sm_b, emu.prog_b.offset, &c);
  pio_sm_put(emu.pio, emu.sm_b, (uint32_t)spi_table >> 13);
  pio_sm_exec(emu.pio, emu.sm_b, pio_encode_pull(false, true));
  pio_sm_exec(emu.pio, emu.sm_b, pio_encode_mov(pio_x, pio_osr));

  /* cs */
  c = pio_get_default_sm_config();
  sm_config_set_wrap(&c, emu.prog_c.offset, emu.prog_c.offset + 2);
  sm_config_set_clkdiv(&c, 1.0f);
  pio_sm_init(emu.pio, emu.sm_c, emu.prog_c.offset, &c);

  /* B: one table row into the TX FIFO, started by A writing its read
   * address (READ_ADDR_TRIG alias); A: the row address from tx's RX FIFO. */
  dc = dma_channel_get_default_config(emu.dma_b);
  channel_config_set_transfer_data_size(&dc, DMA_SIZE_8);
  channel_config_set_read_increment(&dc, true);
  channel_config_set_write_increment(&dc, false);
  channel_config_set_dreq(&dc, pio_get_dreq(emu.pio, emu.sm_b, true));
  channel_config_set_high_priority(&dc, true);
  dma_channel_configure(emu.dma_b, &dc, &emu.pio->txf[emu.sm_b], spi_table, EMU_SPI_BURST, false);

  dc = dma_channel_get_default_config(emu.dma_a);
  channel_config_set_transfer_data_size(&dc, DMA_SIZE_32);
  channel_config_set_read_increment(&dc, false);
  channel_config_set_write_increment(&dc, false);
  channel_config_set_dreq(&dc, pio_get_dreq(emu.pio, emu.sm_b, false));
  channel_config_set_high_priority(&dc, true);
  dma_channel_configure(emu.dma_a, &dc, &dma_hw->ch[emu.dma_b].al3_read_addr_trig,
                        &emu.pio->rxf[emu.sm_b], 1, false);

  pio_interrupt_clear(emu.pio, FLAG_CS_UP);
  pio_set_irq0_source_enabled(emu.pio, pio_get_rx_fifo_not_empty_interrupt_source(emu.sm_a), true);
  pio_set_irq0_source_enabled(emu.pio, (pio_interrupt_source_t)(pis_interrupt0 + FLAG_CS_UP), true);
  spi_rearm();
  pio_sm_set_enabled(emu.pio, emu.sm_c, true);
  irq_enable(EMU_IRQ);
  k_mutex_unlock(&emu_lock);
  ui_activity("EMU SPI M%u", p->mode);
  return 0;
}

void emu_stop(void)
{
  k_mutex_lock(&emu_lock, K_FOREVER);
  if (emu.kind != EMU_OFF) {
    teardown();
  }
  k_mutex_unlock(&emu_lock);
}

void emu_get_stats(struct emu_stats *out)
{
  *out = emu.st;
  out->kind = emu.kind;
}

void emu_init(void)
{
  k_mutex_init(&emu_lock);
  emu.dma_a = -1;
  emu.dma_b = -1;
  memset(regs, 0, sizeof(regs));
  IRQ_CONNECT(EMU_IRQ, EMU_IRQ_PRIO, emu_isr, NULL, 0);
  k_thread_create(&emu_thread_data, emu_stack, K_THREAD_STACK_SIZEOF(emu_stack), emu_thread, NULL,
                  NULL, NULL, K_PRIO_PREEMPT(7), 0, K_NO_WAIT);
  k_thread_name_set(&emu_thread_data, "emu");
}
