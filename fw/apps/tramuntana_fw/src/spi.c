/*
 * Tramuntana - spi. See spi.h.
 *
 * Master: Zephyr's &spi0 (PL022) for the shifting, with chip select on
 * DIO2 driven by hand as a GPIO. The PL022's own hardware CS pulses once
 * per *word*, which breaks every multi-byte transaction, and Zephyr's
 * software-CS path is a no-op when the pin is muxed to the peripheral -
 * so the pin is simply never handed to SPI0. That is also what makes the
 * SPIDriver-style sel/write/read/unsel sequences possible: CS stays
 * wherever the host left it between calls.
 *
 * One quirk of spi_pl022.c worth knowing: it skips reprogramming the
 * hardware whenever it's handed the same 'struct spi_config' pointer as
 * the previous call (pointer equality, not value), so the config lives in
 * two slots and 'conf' alternates between them - every transfer after a
 * 'conf' gets a new pointer (and a reprogram), every transfer after that
 * reuses it (and skips the ~50us of reconfiguration).
 *
 * Sniffer: a PIO0 state machine (see spi_sniff_program()) waits for CS
 * low, samples MOSI and MISO together on the active SCK edge, autopushes
 * a word every 8 clocks and pushes a 0xFFFFFFFF marker when CS rises
 * (so both directions of the bus are captured, not just the master's
 * side); a DMA channel
 * streams the FIFO into the 128KB arena ring, and a thread decodes the
 * ring into EVT_SPI_MSG events. Polling SCK through 'mov osr, pins'
 * rather than 'wait' lets CS be checked between every bit, so the machine
 * frames messages on its own with no CPU restart between them. Roughly
 * 15MHz SCK is the ceiling (4 instructions per poll at 150MHz).
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/misc/pio_rpi_pico/pio_rpi_pico.h>
#include <zephyr/logging/log.h>
#include <hardware/dma.h>
#include <hardware/gpio.h>
#include <hardware/pio.h>
#include <string.h>

#undef pio0
#undef pio1

#include "spi.h"
#include "board.h"
#include "arena.h"
#include "dmaring.h"
#include "display.h"
#include "proto.h"
#include "stamp.h"

LOG_MODULE_REGISTER(spi_drv, LOG_LEVEL_INF);

static const struct device *const spi_bus = DEVICE_DT_GET(DT_NODELABEL(spi0));
static const struct device *const pio_dev = DEVICE_DT_GET(DT_NODELABEL(pio0));

static struct {
  struct spi_config cfg[2];
  int cur;
  uint32_t freq_hz;
  uint8_t mode;
  bool lsb_first;
  bool selected;
  bool claimed;
  bool unsettled;   /* 'conf' changed the mode: SCK may not idle at the new CPOL yet */
  struct k_mutex lock;
} spi;

static uint32_t spi_max_freq_hz(void)
{
  const struct device *clk_dev = DEVICE_DT_GET(DT_CLOCKS_CTLR(DT_NODELABEL(spi0)));
  clock_control_subsys_t clk_id = (clock_control_subsys_t)DT_CLOCKS_CELL(DT_NODELABEL(spi0), clk_id);
  uint32_t pclk = 0;

  if (!device_is_ready(clk_dev) || clock_control_get_rate(clk_dev, clk_id, &pclk) < 0 || pclk == 0) {
    return 1000000;
  }
  return pclk / 2;
}

static void spi_ui_update(void)
{
  if (spi.freq_hz >= 1000000) {
    ui_set(UI_ROW_SPI, "%u.%uMHz M%u%s", spi.freq_hz / 1000000, (spi.freq_hz % 1000000) / 100000,
           spi.mode, spi.lsb_first ? " LSB" : "");
  } else {
    ui_set(UI_ROW_SPI, "%ukHz M%u%s", spi.freq_hz / 1000, spi.mode, spi.lsb_first ? " LSB" : "");
  }
}

/* SCK/MOSI/MISO to the peripheral, CS as a GPIO output (high). */
static int spi_claim_pins(void)
{
  int ret;

  if (spi.claimed && board_pin_owner(2) == OWNER_SPI && board_pin_owner(3) == OWNER_SPI &&
      board_pin_owner(4) == OWNER_SPI && board_pin_owner(5) == OWNER_SPI) {
    return 0;
  }
  ret = board_pins_claim(DIO_MASK_SPI, OWNER_SPI, false);
  if (ret != 0) {
    return ret;
  }
  /* board.c muxed SCK/MOSI/MISO to SPI0 and left CS as a SIO output,
   * high. */
  spi.selected = false;
  spi.claimed = true;
  return 0;
}

static struct spi_config *spi_next_cfg(void)
{
  struct spi_config *next = &spi.cfg[spi.cur ^ 1];

  *next = spi.cfg[spi.cur];
  spi.cur ^= 1;
  return next;
}

int spi_drv_conf(uint32_t freq_hz, uint8_t mode, bool lsb_first, uint32_t *actual_hz)
{
  uint32_t max = spi_max_freq_hz();
  struct spi_config *cfg;

  if (mode > 3 || freq_hz < SPI_MIN_FREQ_HZ) {
    return -EINVAL;
  }
  if (freq_hz > max) {
    freq_hz = max;
  }
  k_mutex_lock(&spi.lock, K_FOREVER);
  cfg = spi_next_cfg();
  cfg->frequency = freq_hz;
  cfg->operation = SPI_WORD_SET(8) | SPI_OP_MODE_CONTROLLER |
                   ((mode & 2) ? SPI_MODE_CPOL : 0) | ((mode & 1) ? SPI_MODE_CPHA : 0) |
                   (lsb_first ? SPI_TRANSFER_LSB : 0);
  spi.freq_hz = freq_hz;
  spi.mode = mode;
  spi.lsb_first = lsb_first;
  spi.unsettled = true;
  k_mutex_unlock(&spi.lock);
  if (actual_hz != NULL) {
    *actual_hz = freq_hz;
  }
  spi_ui_update();
  return 0;
}

/* The PL022 is only reprogrammed by the next transfer, so after a mode
 * change SCK still idles at the old CPOL - and would move to the new one
 * after CS has gone low, a clock edge the target counts. One dummy byte
 * with CS still high (ignored by every target) settles it first. */
static void spi_settle(void)
{
  static uint8_t dummy;
  struct spi_buf b = { .buf = &dummy, .len = 1 };
  struct spi_buf_set set = { .buffers = &b, .count = 1 };

  (void)spi_transceive(spi_bus, &spi.cfg[spi.cur], &set, NULL);
  spi.unsettled = false;
}

int spi_drv_sel(void)
{
  int ret;

  k_mutex_lock(&spi.lock, K_FOREVER);
  ret = spi_claim_pins();
  if (ret == 0) {
    if (spi.unsettled && !spi.selected) {
      spi_settle();
    }
    gpio_put(GPIO_SPI_CS, 0);
    spi.selected = true;
  }
  k_mutex_unlock(&spi.lock);
  return ret;
}

int spi_drv_unsel(void)
{
  int ret;

  k_mutex_lock(&spi.lock, K_FOREVER);
  ret = spi_claim_pins();
  if (ret == 0) {
    gpio_put(GPIO_SPI_CS, 1);
    spi.selected = false;
  }
  k_mutex_unlock(&spi.lock);
  return ret;
}

static uint8_t spi_fill_buf[SPI_MAX_XFER];

int spi_drv_xfer(const uint8_t *tx, uint8_t *rx, size_t n, uint8_t fill)
{
  struct spi_buf tx_buf, rx_buf;
  struct spi_buf_set tx_set = { .buffers = &tx_buf, .count = 1 };
  struct spi_buf_set rx_set = { .buffers = &rx_buf, .count = 1 };
  int ret;

  if (n == 0 || n > SPI_MAX_XFER) {
    return -EINVAL;
  }
  k_mutex_lock(&spi.lock, K_FOREVER);
  ret = spi_claim_pins();
  if (ret != 0) {
    k_mutex_unlock(&spi.lock);
    return ret;
  }
  if (tx == NULL) {
    memset(spi_fill_buf, fill, n);
    tx = spi_fill_buf;
  }
  tx_buf.buf = (void *)tx;
  tx_buf.len = n;
  rx_buf.buf = rx;
  rx_buf.len = n;
  led_set(LED_BLUE);
  /* Same config pointer as last time unless 'conf' changed it: the driver
   * only reprograms the SSP when the pointer differs (see spi_next_cfg). */
  ret = spi_transceive(spi_bus, &spi.cfg[spi.cur], &tx_set, rx != NULL ? &rx_set : NULL);
  spi.unsettled = false;
  led_flash(ret == 0 ? LED_GREEN : LED_RED, 60);
  k_mutex_unlock(&spi.lock);
  ui_activity("SPI %uB", (unsigned)n);
  return ret;
}

int spi_drv_txn(const uint8_t *tx, size_t tn, uint8_t *rx, size_t rn)
{
  int ret = spi_drv_sel();

  if (ret == 0 && tn > 0) {
    ret = spi_drv_xfer(tx, NULL, tn, 0);
  }
  if (ret == 0 && rn > 0) {
    ret = spi_drv_xfer(NULL, rx, rn, 0);
  }
  spi_drv_unsel();
  return ret;
}

uint32_t spi_drv_freq(void)
{
  return spi.freq_hz;
}

uint8_t spi_drv_mode(void)
{
  return spi.mode;
}

bool spi_drv_lsb_first(void)
{
  return spi.lsb_first;
}

bool spi_drv_selected(void)
{
  return spi.selected;
}

/* --- sniffer -------------------------------------------------------------- */

#define SNIFF_MARKER      0xFFFFFFFFu
#define SNIFF_RING_WORDS  (ARENA_RING_BYTES / 4)
#define SNIFF_KEEP_BYTES  256
#define SNIFF_MSG_TIMEOUT_MS 2000
#define SNIFF_PIN_SCK  GPIO_SPI_SCK   /* 2 */
#define SNIFF_PIN_MOSI GPIO_SPI_TX    /* 3 */
#define SNIFF_PIN_MISO GPIO_SPI_RX    /* 4 */
#define SNIFF_PIN_CS   GPIO_SPI_CS    /* 5 */

static const char *const sniff_holder = "spi-sniff";

static struct {
  bool active;
  bool stop_req;
  PIO pio;
  uint sm;
  uint offset;
  uint16_t instr[14];
  struct pio_program program;
  struct dma_ring ring;
  uint32_t rd;
  uint32_t count;
  uint8_t mode;
  struct k_thread thread;
  struct k_sem done;
} sniff;

K_THREAD_STACK_DEFINE(sniff_stack, 2048);

/*
 *    0  mov isr, ~null      ; \ frame end (and program start): push the
 *    1  push noblock        ; / 0xFFFFFFFF marker, which also clears ISR
 *    2  wait 0 gpio CS      ; frame start
 *    3  irq set 0           ; stamp it (stamp.c)
 *    4  jmp pin, 0          ; \
 *    5  mov osr, pins       ;  | poll until SCK sits at its inactive level
 *    6  out x, 1            ;  | (x = SCK), bailing out to 0 the moment CS
 *    7  jmp x--, 4          ; /  goes high
 *    8  jmp pin, 0          ; \
 *    9  mov osr, pins       ;  | poll until SCK reaches its active level:
 *   10  out x, 1            ;  | that's the sampling edge, and the same
 *   11  jmp !x, 8           ; /  snapshot already holds MOSI/MISO then
 *   12  in osr, 2           ; MOSI, MISO -> ISR; autopush every 16 bits
 *   13  jmp 4               ; next bit
 *
 * (7 and 11 swap their conditions for falling-edge sampling: modes 1/2.)
 *
 * in_base = GPIO2, so a pin snapshot has SCK in bit 0, MOSI (GPIO3) in
 * bit 1 and MISO (GPIO4) in bit 2. 'out x, 1' peels SCK off, then 'in
 * osr, 2' takes the next two: after 8 clocks the ISR holds
 * miso7 mosi7 miso6 mosi6 ... miso0 mosi0 - both directions of the bus
 * from one state machine, deinterleaved by sniff_split() below. The
 * 0xFFFFFFFF marker can never collide with a 16-bit push.
 */
static void spi_sniff_program(uint8_t mode)
{
  bool sample_rising = ((mode >> 1) & 1) == (mode & 1); /* CPOL == CPHA */
  uint16_t *i = sniff.instr;

  i[0] = pio_encode_mov_not(pio_isr, pio_null);
  i[1] = pio_encode_push(false, false);
  i[2] = pio_encode_wait_gpio(false, SNIFF_PIN_CS);
  i[3] = pio_encode_irq_set(false, STAMP_PIO_FLAG);
  i[4] = pio_encode_jmp_pin(0);
  i[5] = pio_encode_mov(pio_osr, pio_pins);
  i[6] = pio_encode_out(pio_x, 1);
  i[7] = sample_rising ? pio_encode_jmp_x_dec(4) : pio_encode_jmp_not_x(4);
  i[8] = pio_encode_jmp_pin(0);
  i[9] = pio_encode_mov(pio_osr, pio_pins);
  i[10] = pio_encode_out(pio_x, 1);
  i[11] = sample_rising ? pio_encode_jmp_not_x(8) : pio_encode_jmp_x_dec(8);
  i[12] = pio_encode_in(pio_osr, 2);
  i[13] = pio_encode_jmp(4);

  sniff.program = (struct pio_program){
    .instructions = sniff.instr,
    .length = 14,
    .origin = -1,
    .pio_version = 0,
    .used_gpio_ranges = BIT(0),
  };
}

/* Splits one 16-bit interleaved word into its MOSI and MISO bytes. */
static void sniff_split(uint32_t word, uint8_t *mosi, uint8_t *miso)
{
  uint8_t mo = 0, mi = 0;
  int b;

  for (b = 0; b < 8; b++) {
    mo |= (uint8_t)(((word >> (2 * b)) & 1) << b);
    mi |= (uint8_t)(((word >> (2 * b + 1)) & 1) << b);
  }
  *mosi = mo;
  *miso = mi;
}

static uint32_t sniff_ring_pending(void)
{
  uint32_t wr = dma_ring_write_offset(&sniff.ring) / 4;

  return (wr - sniff.rd) % SNIFF_RING_WORDS;
}

/* Ring index of the word sniff_ring_get() returned last. */
static uint32_t sniff_last_index;

static bool sniff_ring_get(uint32_t *word, bool *overflow)
{
  uint32_t pending = sniff_ring_pending();
  const uint32_t *ring = (const uint32_t *)arena_ring();

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

static int sniff_hw_start(uint8_t mode)
{
  size_t sm;
  int ret;
  pio_sm_config c;

  if (!device_is_ready(pio_dev)) {
    return -ENODEV;
  }
  sniff.pio = pio_rpi_pico_get_pio(pio_dev);
  spi_sniff_program(mode);
  if (!pio_can_add_program(sniff.pio, &sniff.program)) {
    return -ENOMEM;
  }
  ret = pio_rpi_pico_allocate_sm(pio_dev, &sm);
  if (ret < 0) {
    return ret;
  }
  sniff.sm = (uint)sm;
  sniff.offset = (uint)pio_add_program(sniff.pio, &sniff.program);

  pio_sm_set_consecutive_pindirs(sniff.pio, sniff.sm, SNIFF_PIN_SCK, 4, false);

  c = pio_get_default_sm_config();
  sm_config_set_wrap(&c, sniff.offset, sniff.offset + sniff.program.length - 1);
  sm_config_set_in_pins(&c, SNIFF_PIN_SCK);
  sm_config_set_jmp_pin(&c, SNIFF_PIN_CS);
  sm_config_set_in_shift(&c, false, true, 16);
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

static void sniff_thread(void *a, void *b, void *c)
{
  /* evt[0..6]: flags, [t_us], total; the unstamped header starts at evt + 4 */
  static uint8_t evt[7 + 2 * SNIFF_KEEP_BYTES];
  uint8_t *mosi = evt + 7;
  uint8_t *miso = evt + 7 + SNIFF_KEEP_BYTES;
  uint32_t t_first = 0;
  size_t stored = 0, total = 0;
  bool overflow = false;
  int64_t last_byte_at = 0;

  ARG_UNUSED(a);
  ARG_UNUSED(b);
  ARG_UNUSED(c);

  while (!sniff.stop_req) {
    uint32_t word;
    bool got = sniff_ring_get(&word, &overflow);

    if (!got) {
      if (total > 0 && k_uptime_get() - last_byte_at > SNIFF_MSG_TIMEOUT_MS) {
        /* CS parked low: flush what's there. */
        word = SNIFF_MARKER;
      } else {
        k_sleep(K_MSEC(1));
        continue;
      }
    }
    if (word == SNIFF_MARKER) {
      if (total > 0) {
        bool rxstall = (sniff.pio->fdebug & (1u << (PIO_FDEBUG_RXSTALL_LSB + sniff.sm))) != 0;

        sniff.pio->fdebug = 1u << (PIO_FDEBUG_RXSTALL_LSB + sniff.sm);
        uint8_t flags = (overflow || rxstall) ? 1 : 0;

        /* Pack: flags, [t_us], total, mosi[stored], miso[stored]. */
        memmove(evt + 7 + stored, miso, stored);
        if (proto_event_stamps) {
          evt[0] = flags | EVT_FLAG_STAMPED;
          wr32(evt + 1, t_first);
          wr16(evt + 5, (uint16_t)MIN(total, 65535));
          proto_send_event(EVT_SPI_MSG, evt, 7 + 2 * stored);
        } else {
          evt[4] = flags;
          wr16(evt + 5, (uint16_t)MIN(total, 65535));
          proto_send_event(EVT_SPI_MSG, evt + 4, 3 + 2 * stored);
        }
        sniff.count++;
        led_flash(flags ? LED_RED : LED_GREEN, 40);
        ui_activity("SPI msg %uB", (unsigned)total);
        if (sniff.count % 10 == 1) {
          ui_set(UI_ROW_SPI, "SNIFF M%u %u msgs", sniff.mode, sniff.count);
        }
        stored = total = 0;
        overflow = false;
      }
      continue;
    }
    if (total == 0 && !stamp_for(sniff_last_index, &t_first)) {
      t_first = board_time_us();
    }
    if (stored < SNIFF_KEEP_BYTES) {
      sniff_split(word, &mosi[stored], &miso[stored]);
      stored++;
    }
    total++;
    last_byte_at = k_uptime_get();
  }
  k_sem_give(&sniff.done);
}

int spi_sniff_start(uint8_t mode)
{
  int ret;

  if (sniff.active) {
    return -EBUSY;
  }
  if (mode > 3) {
    return -EINVAL;
  }
  ret = arena_ring_claim(sniff_holder);
  if (ret != 0) {
    return ret;
  }
  /* No pin claim: PIO reads every pad's input whatever its function
   * select, so the sniffer can watch the bus while the master (ours or
   * a target's) drives it - including this board's own transactions. */
  ret = sniff_hw_start(mode);
  if (ret != 0) {
    arena_ring_release(sniff_holder);
    return ret;
  }
  sniff.mode = mode;
  sniff.count = 0;
  sniff.stop_req = false;
  sniff.active = true;
  k_sem_init(&sniff.done, 0, 1);
  k_thread_create(&sniff.thread, sniff_stack, K_THREAD_STACK_SIZEOF(sniff_stack), sniff_thread,
                  NULL, NULL, NULL, K_PRIO_PREEMPT(6), 0, K_NO_WAIT);
  k_thread_name_set(&sniff.thread, "spi-sniff");
  led_background(LED_BLUE);
  ui_set(UI_ROW_SPI, "SNIFF M%u listening", mode);
  return 0;
}

int spi_sniff_stop(void)
{
  if (!sniff.active) {
    return 0;
  }
  sniff.stop_req = true;
  k_sem_take(&sniff.done, K_MSEC(500));
  sniff_hw_stop();
  arena_ring_release(sniff_holder);
  sniff.active = false;
  led_background(LED_OFF);
  spi_ui_update();
  return 0;
}

bool spi_sniff_active(void)
{
  return sniff.active;
}

uint32_t spi_sniff_count(void)
{
  return sniff.count;
}

void spi_drv_init(void)
{
  k_mutex_init(&spi.lock);
  spi.cfg[0].frequency = 1000000;
  spi.cfg[0].operation = SPI_WORD_SET(8) | SPI_OP_MODE_CONTROLLER;
  spi.cfg[1] = spi.cfg[0];
  spi.freq_hz = 1000000;
  spi.mode = 0;
  if (!device_is_ready(spi_bus)) {
    LOG_ERR("spi0 not ready");
  }
  spi_ui_update();
}
