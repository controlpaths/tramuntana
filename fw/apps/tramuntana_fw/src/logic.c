/*
 * Tramuntana - logic. See logic.h.
 *
 * Sampling: one PIO0 state machine runs 'in pins, 8' as its entire loop
 * (wrap around a single instruction) with autopush at 32 bits, so every
 * PIO clock cycle takes one 8-bit snapshot of GPIO0..7 and every four
 * cycles a word lands in the RX FIFO; the state machine's clock divider
 * sets the sample rate (sysclk / clkdiv, 150MS/s at clkdiv 1). A DMA
 * channel moves the words into the 128KB arena ring.
 *
 * Triggers:
 *   - none: DMA a linear block of n samples straight away.
 *   - rising/falling edge on one pin: the program gains 'wait 0 pin' /
 *     'wait 1 pin' (in the right order) before the sampling loop, so the
 *     hardware itself waits for the edge with zero latency - but no
 *     pre-trigger samples. Then the same linear DMA. With pre-trigger
 *     samples requested, the edge becomes a one-pin pattern instead.
 *   - pattern ((sample & mask) == value): the DMA runs endlessly around
 *     the ring while the CPU scans new words for the first matching byte
 *     (four samples per word, checked with the usual "has-zero-byte" bit
 *     trick), then lets the post-trigger samples land and stops. Gives
 *     pre-trigger like the scope. The trigger is the bus *entering* the
 *     pattern: a sample that does not match has to be seen first, so a
 *     pattern already true when armed waits for the next time it starts. The scan costs ~8 cycles per word in
 *     scan_words() below, so this mode is capped at clkdiv >= 10 (15MS/s)
 *     to be sure the CPU keeps up with the DMA.
 *
 * A finished capture stays in the ring for the host to page through with
 * logic_read(); the ring is marked evictable so a sniffer that needs the
 * memory can take it (the capture then reads as CAP_IDLE).
 *
 * Pin order: the PIO sees GPIO0 in bit 0, but DIO0 is GPIO7 - samples
 * are bit-reversed on the way out so that bit i = DIOi, and the trigger
 * mask/value/pin are converted the other way on the way in.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/misc/pio_rpi_pico/pio_rpi_pico.h>
#include <zephyr/logging/log.h>
#include <hardware/dma.h>
#include <hardware/pio.h>
#include <string.h>

#undef pio0
#undef pio1

#include "logic.h"
#include "board.h"
#include "arena.h"
#include "dmaring.h"
#include "display.h"
#include "proto.h"

LOG_MODULE_REGISTER(logic, LOG_LEVEL_INF);

#define RING_BYTES          ARENA_RING_BYTES
#define RING_WORDS          (RING_BYTES / 4)
#define DEFAULT_TIMEOUT_MS  5000
#define PATTERN_MIN_CLKDIV  10   /* 15MS/s: what the CPU scan keeps up with */
#define PATTERN_MAX_SAMPLES (LOGIC_MAX_SAMPLES - 8192)
#define SCAN_GUARD_BYTES    16

static const struct device *const pio_dev = DEVICE_DT_GET(DT_NODELABEL(pio0));
static const char *const holder = "logic";

static struct {
  struct logic_params p;
  enum capture_state state;
  uint32_t rate_hz;
  uint32_t trig_index;
  uint32_t start;        /* ring byte index of sample 0 of the result */
  uint32_t total;        /* samples in the result */
  PIO pio;
  uint sm;
  uint offset;
  uint16_t instr[3];
  struct pio_program program;
  int dma_chan;          /* linear captures (none / edge trigger) */
  struct dma_ring ring;  /* pattern trigger */
  bool hw_up;
  bool abort_req;
  struct k_sem go;
  struct k_mutex lock;
} la;

static uint8_t rev8[256];

K_THREAD_STACK_DEFINE(logic_stack, 2048);
static struct k_thread logic_thread_data;

static void logic_ui(const char *what)
{
  if (la.rate_hz >= 1000000) {
    ui_set(UI_ROW_LOGIC, "%u.%uMS/s %s", la.rate_hz / 1000000, (la.rate_hz % 1000000) / 100000, what);
  } else {
    ui_set(UI_ROW_LOGIC, "%ukS/s %s", la.rate_hz / 1000, what);
  }
}

static void logic_evicted(void)
{
  k_mutex_lock(&la.lock, K_FOREVER);
  if (la.state == CAP_DONE) {
    la.state = CAP_IDLE;
    la.total = 0;
  }
  k_mutex_unlock(&la.lock);
}

static int hw_start(void)
{
  size_t sm;
  int ret;
  pio_sm_config c;
  dma_channel_config dc;
  uint8_t gpio_pin = 7 - (la.p.trig_pin & 7);
  int len = 0;

  if (!device_is_ready(pio_dev)) {
    return -ENODEV;
  }
  la.pio = pio_rpi_pico_get_pio(pio_dev);

  if (la.p.trig_mode == LOGIC_TRIG_RISING) {
    la.instr[len++] = pio_encode_wait_pin(false, gpio_pin);
    la.instr[len++] = pio_encode_wait_pin(true, gpio_pin);
  } else if (la.p.trig_mode == LOGIC_TRIG_FALLING) {
    la.instr[len++] = pio_encode_wait_pin(true, gpio_pin);
    la.instr[len++] = pio_encode_wait_pin(false, gpio_pin);
  }
  la.instr[len++] = pio_encode_in(pio_pins, 8);
  la.program = (struct pio_program){
    .instructions = la.instr,
    .length = len,
    .origin = -1,
    .pio_version = 0,
    .used_gpio_ranges = BIT(0),
  };
  if (!pio_can_add_program(la.pio, &la.program)) {
    return -ENOMEM;
  }
  la.dma_chan = -1;
  ret = pio_rpi_pico_allocate_sm(pio_dev, &sm);
  if (ret < 0) {
    return ret;
  }
  la.sm = (uint)sm;
  la.offset = (uint)pio_add_program(la.pio, &la.program);
  pio_sm_set_consecutive_pindirs(la.pio, la.sm, 0, 8, false);

  c = pio_get_default_sm_config();
  /* Loop only over the 'in' instruction; the wait prefix runs once. */
  sm_config_set_wrap(&c, la.offset + len - 1, la.offset + len - 1);
  sm_config_set_in_pins(&c, 0);
  /* Shift right so the first of the four samples in a word lands in
   * bits 7:0 - i.e. in memory byte 0 once the DMA writes the word
   * little-endian. Shifting left would store each group of four samples
   * in reverse order and turn every transition into a fake glitch. */
  sm_config_set_in_shift(&c, true, true, 32);
  sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
  sm_config_set_clkdiv_int_frac(&c, la.p.clkdiv, 0);
  pio_sm_init(la.pio, la.sm, la.offset, &c);
  pio_sm_clear_fifos(la.pio, la.sm);
  la.pio->fdebug = 1u << (PIO_FDEBUG_RXSTALL_LSB + la.sm);

  if (la.p.trig_mode == LOGIC_TRIG_PATTERN) {
    ret = dma_ring_start(&la.ring, arena_ring(), RING_BYTES, 4,
                         pio_get_dreq(la.pio, la.sm, false), &la.pio->rxf[la.sm]);
  } else {
    la.dma_chan = dma_claim_unused_channel(false);
    ret = (la.dma_chan < 0) ? -EBUSY : 0;
    if (ret == 0) {
      dc = dma_channel_get_default_config(la.dma_chan);
      channel_config_set_transfer_data_size(&dc, DMA_SIZE_32);
      channel_config_set_read_increment(&dc, false);
      channel_config_set_write_increment(&dc, true);
      channel_config_set_dreq(&dc, pio_get_dreq(la.pio, la.sm, false));
      dma_channel_configure(la.dma_chan, &dc, arena_ring(), &la.pio->rxf[la.sm],
                            (la.p.n + 3) / 4, true);
    }
  }
  if (ret != 0) {
    pio_remove_program(la.pio, &la.program, la.offset);
    pio_sm_unclaim(la.pio, la.sm);
    return ret;
  }
  pio_sm_set_enabled(la.pio, la.sm, true);
  la.hw_up = true;
  return 0;
}

static void hw_stop(void)
{
  if (!la.hw_up) {
    return;
  }
  pio_sm_set_enabled(la.pio, la.sm, false);
  if (la.p.trig_mode == LOGIC_TRIG_PATTERN) {
    dma_ring_stop(&la.ring);
  } else {
    dma_channel_abort(la.dma_chan);
    dma_channel_cleanup(la.dma_chan);
    dma_channel_unclaim(la.dma_chan);
    la.dma_chan = -1;
  }
  pio_remove_program(la.pio, &la.program, la.offset);
  pio_sm_unclaim(la.pio, la.sm);
  la.hw_up = false;
}

static uint32_t ring_write_bytes(void)
{
  if (la.p.trig_mode == LOGIC_TRIG_PATTERN) {
    return dma_ring_write_offset(&la.ring);
  }
  return dma_channel_hw_addr(la.dma_chan)->write_addr - (uint32_t)arena_ring();
}

/* Scans words[0..n) for the first byte with (byte & mask) == value
 * (mask4/val4 are the byte pattern replicated in all four lanes) using
 * the classic "has a zero byte" trick, four samples per iteration.
 * Returns the byte index of the match or -1. Hot loop: kept free of
 * modulo and function calls, and compiled -O2 whatever the build uses. */
__ramfunc __attribute__((optimize("O2")))
static int scan_words(const uint32_t *words, uint32_t n, uint32_t mask4, uint32_t val4)
{
  uint32_t i;

  for (i = 0; i < n; i++) {
    uint32_t v = (words[i] & mask4) ^ val4;

    if (((v - 0x01010101u) & ~v & 0x80808080u) != 0) {
      int b;

      for (b = 0; b < 4; b++) {
        if (((v >> (8 * b)) & 0xFF) == 0) {
          return (int)(i * 4 + (uint32_t)b);
        }
      }
    }
  }
  return -1;
}

/* Index of the first sample that does NOT match, or -1. */
static int scan_words_miss(const uint32_t *words, uint32_t n, uint32_t mask4, uint32_t val4)
{
  uint32_t i;

  for (i = 0; i < n; i++) {
    uint32_t v = (words[i] & mask4) ^ val4;

    if (v != 0) {
      int b;

      for (b = 0; b < 4; b++) {
        if (((v >> (8 * b)) & 0xFF) != 0) {
          return (int)(i * 4 + (uint32_t)b);
        }
      }
    }
  }
  return -1;
}

static void logic_thread(void *a, void *b, void *c)
{
  ARG_UNUSED(a);
  ARG_UNUSED(b);
  ARG_UNUSED(c);

  for (;;) {
    bool timed_out = false, ok = false;
    int64_t deadline, started;
    uint8_t evt[2];

    k_sem_take(&la.go, K_FOREVER);
    started = k_uptime_get();
    deadline = started + (la.p.timeout_ms ? la.p.timeout_ms : DEFAULT_TIMEOUT_MS);
    led_set(la.p.trig_mode == LOGIC_TRIG_NONE ? LED_GREEN : LED_RED);
    logic_ui(la.p.trig_mode == LOGIC_TRIG_NONE ? "capturing" : "armed");

    if (la.p.trig_mode == LOGIC_TRIG_PATTERN) {
      const uint32_t *ring = (const uint32_t *)arena_ring();
      uint8_t gmask = rev8[la.p.trig_mask];
      uint8_t gval = rev8[la.p.trig_value & la.p.trig_mask];
      uint32_t mask4 = gmask * 0x01010101u;
      uint32_t val4 = gval * 0x01010101u;
      uint32_t written = 0, last_wr = 0, scanned = 0; /* bytes */
      uint32_t trig_abs = 0;
      bool triggered = false;
      bool primed = false; /* a non-matching sample has been seen */

      while (!la.abort_req) {
        uint32_t wr = ring_write_bytes() & ~3u;
        uint32_t delta = (wr - last_wr) % RING_BYTES;

        last_wr = wr;
        written += delta;

        if (!triggered) {
          /* Scan everything written since last time, in contiguous runs
           * (no wrap inside a run), staying a few words behind the DMA:
           * its write pointer moves a few cycles before the data lands
           * and the ring still holds the previous capture underneath. */
          while (!triggered && written >= scanned + 4 + SCAN_GUARD_BYTES) {
            uint32_t avail = written - SCAN_GUARD_BYTES - scanned;
            uint32_t idx = scanned % RING_BYTES;
            uint32_t run = MIN(avail, RING_BYTES - idx) & ~3u;
            int m;

            if (scanned < la.p.pre) {
              uint32_t skip = MIN(run, ((la.p.pre - scanned) + 3) & ~3u);

              scanned += skip;
              continue;
            }
            if (!primed) {
              const uint8_t *ring8 = (const uint8_t *)ring;
              uint32_t base;
              int k;

              m = scan_words_miss((const uint32_t *)(ring + idx / 4), run / 4, mask4, val4);
              if (m < 0) {
                scanned += run;
                continue;
              }
              primed = true;
              /* Rest of the word holding the miss, sample by sample: the
               * trigger is the first match after it. */
              base = scanned + ((uint32_t)m & ~3u);
              for (k = (m & 3) + 1; k < 4; k++) {
                if ((ring8[(base + (uint32_t)k) % RING_BYTES] & gmask) == gval) {
                  triggered = true;
                  trig_abs = base + (uint32_t)k;
                  break;
                }
              }
              scanned = base + 4;
              continue;
            }
            m = scan_words((const uint32_t *)(ring + idx / 4), run / 4, mask4, val4);
            if (m >= 0) {
              triggered = true;
              trig_abs = scanned + (uint32_t)m;
              break;
            }
            scanned += run;
          }
          if (!triggered && written - scanned > RING_BYTES - 4096) {
            scanned = written - 4096; /* fell behind: skip ahead */
            scanned &= ~3u;
          }
          if (triggered) {
            la.state = CAP_RUNNING;
            led_set(LED_GREEN);
            logic_ui("capturing");
          } else if (k_uptime_get() > deadline) {
            timed_out = true;
            break;
          }
        }
        if (triggered && written >= trig_abs + (la.p.n - la.p.pre)) {
          break;
        }
        k_sleep(K_USEC(250));
      }
      hw_stop();
      if (triggered && !la.abort_req) {
        la.start = (trig_abs - la.p.pre) % RING_BYTES;
        la.total = la.p.n;
        la.trig_index = la.p.pre;
        ok = true;
      }
    } else {
      bool armed_shown = false;

      while (!la.abort_req) {
        if (!dma_channel_is_busy(la.dma_chan)) {
          ok = true;
          break;
        }
        if (!armed_shown && la.p.trig_mode != LOGIC_TRIG_NONE && ring_write_bytes() > 0) {
          la.state = CAP_RUNNING;
          led_set(LED_GREEN);
          logic_ui("capturing");
          armed_shown = true;
        }
        if (k_uptime_get() > deadline) {
          timed_out = true;
          break;
        }
        k_sleep(K_MSEC(1));
      }
      hw_stop();
      if (ok) {
        la.start = 0;
        la.total = la.p.n;
        la.trig_index = 0;
      }
    }

    k_mutex_lock(&la.lock, K_FOREVER);
    if (la.abort_req) {
      la.state = CAP_ABORTED;
      la.total = 0;
      arena_ring_release(holder);
    } else if (timed_out) {
      la.state = CAP_TIMEOUT;
      la.total = 0;
      arena_ring_release(holder);
    } else {
      la.state = CAP_DONE;
      arena_ring_set_evictable(holder, logic_evicted);
    }
    k_mutex_unlock(&la.lock);

    LOG_INF("capture %s: %u samples @ %u S/s in %lld ms", la.state == CAP_DONE ? "done" : "failed",
            la.total, la.rate_hz, (long long)(k_uptime_get() - started));
    if (la.state == CAP_DONE) {
      /* Dashboard preview from the first samples (reversed to DIO order). */
      static uint8_t preview[1024];
      uint32_t n = MIN(la.total, sizeof(preview));
      const uint8_t *ring = arena_ring();
      uint32_t i;

      for (i = 0; i < n; i++) {
        preview[i] = rev8[ring[(la.start + i) % RING_BYTES]];
      }
      ui_plot_digital(preview, n);
      logic_ui("done");
      led_flash(LED_GREEN, 100);
    } else {
      logic_ui(la.state == CAP_TIMEOUT ? "no trigger" : "aborted");
      led_flash(LED_RED, 200);
    }
    evt[0] = 1;
    evt[1] = (uint8_t)la.state;
    proto_send_event(EVT_CAPTURE_DONE, evt, 2);
  }
}

int logic_arm(const struct logic_params *p, uint32_t *rate_hz)
{
  struct logic_params lp = *p;
  int ret;

  if (lp.n == 0 || lp.n > LOGIC_MAX_SAMPLES || lp.clkdiv == 0) {
    return -EINVAL;
  }
  if (lp.trig_mode > LOGIC_TRIG_PATTERN || lp.trig_pin > 7) {
    return -EINVAL;
  }
  /* An edge with pre-trigger is the pin entering its new level: a
   * one-pin pattern, scanned by the CPU (so 15MS/s at most). */
  if ((lp.trig_mode == LOGIC_TRIG_RISING || lp.trig_mode == LOGIC_TRIG_FALLING) && lp.pre > 0) {
    lp.trig_mask = BIT(lp.trig_pin);
    lp.trig_value = lp.trig_mode == LOGIC_TRIG_RISING ? lp.trig_mask : 0;
    lp.trig_mode = LOGIC_TRIG_PATTERN;
  }
  if (lp.trig_mode == LOGIC_TRIG_PATTERN) {
    if (lp.clkdiv < PATTERN_MIN_CLKDIV || lp.n > PATTERN_MAX_SAMPLES || lp.pre > lp.n ||
        lp.trig_mask == 0) {
      return -EINVAL;
    }
  } else {
    lp.pre = 0;
  }
  /* Whole words: the PIO pushes 4 samples at a time. */
  lp.n = (lp.n + 3) & ~3u;

  k_mutex_lock(&la.lock, K_FOREVER);
  if (la.state == CAP_ARMED || la.state == CAP_RUNNING) {
    k_mutex_unlock(&la.lock);
    return -EBUSY;
  }
  ret = arena_ring_claim(holder);
  if (ret != 0) {
    k_mutex_unlock(&la.lock);
    return ret;
  }
  /* No pin claim: the PIO samples every pad's input regardless of what
   * drives it, so the analyzer can capture the board's own SPI/I2C/UART
   * traffic as well as a target's. */
  la.p = lp;
  la.rate_hz = board_sysclk_hz() / lp.clkdiv;
  la.total = 0;
  la.trig_index = 0;
  la.abort_req = false;
  ret = hw_start();
  if (ret != 0) {
    arena_ring_release(holder);
    k_mutex_unlock(&la.lock);
    return ret;
  }
  la.state = CAP_ARMED;
  k_mutex_unlock(&la.lock);
  if (rate_hz != NULL) {
    *rate_hz = la.rate_hz;
  }
  k_sem_give(&la.go);
  return 0;
}

int logic_abort(void)
{
  if (la.state == CAP_ARMED || la.state == CAP_RUNNING) {
    la.abort_req = true;
  }
  return 0;
}

enum capture_state logic_state(void)
{
  return la.state;
}

void logic_status(uint32_t *n, uint32_t *rate_hz, uint32_t *trig_index)
{
  *n = la.total;
  *rate_hz = la.rate_hz;
  *trig_index = la.trig_index;
}

int logic_read(uint32_t offset, uint16_t count, uint8_t *out)
{
  const uint8_t *ring = arena_ring();
  uint32_t i;

  if (la.state != CAP_DONE) {
    return -EPERM;
  }
  if (offset >= la.total) {
    return -EINVAL;
  }
  if (offset + count > la.total) {
    count = (uint16_t)(la.total - offset);
  }
  for (i = 0; i < count; i++) {
    out[i] = rev8[ring[(la.start + offset + i) % RING_BYTES]];
  }
  return count;
}

int logic_capture_blocking(const struct logic_params *p, uint32_t *rate_hz)
{
  int ret = logic_arm(p, rate_hz);

  if (ret != 0) {
    return ret;
  }
  while (la.state == CAP_ARMED || la.state == CAP_RUNNING) {
    k_sleep(K_MSEC(5));
  }
  switch (la.state) {
  case CAP_DONE:
    return 0;
  case CAP_TIMEOUT:
    return -ETIMEDOUT;
  default:
    return -ECANCELED;
  }
}

void logic_init(void)
{
  int i;

  for (i = 0; i < 256; i++) {
    uint8_t v = 0;
    int b;

    for (b = 0; b < 8; b++) {
      if (i & BIT(b)) {
        v |= BIT(7 - b);
      }
    }
    rev8[i] = v;
  }
  k_mutex_init(&la.lock);
  k_sem_init(&la.go, 0, 1);
  la.state = CAP_IDLE;
  la.rate_hz = board_sysclk_hz();
  k_thread_create(&logic_thread_data, logic_stack, K_THREAD_STACK_SIZEOF(logic_stack),
                  logic_thread, NULL, NULL, NULL, K_PRIO_PREEMPT(4), 0, K_NO_WAIT);
  k_thread_name_set(&logic_thread_data, "logic");
  ui_set(UI_ROW_LOGIC, "idle");
}
