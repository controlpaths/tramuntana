/*
 * Tramuntana - scope. See scope.h.
 *
 * The ADC free-runs (one channel, or AIN0/AIN1 round-robin) into its
 * FIFO and a DMA channel streams the FIFO endlessly into the 32KB scope
 * ring in the capture arena, address-wrapped by the DMA itself. A
 * capture thread then watches the ring like a real oscilloscope does:
 * it keeps track of how many samples have been written in total, scans
 * every new AIN0 sample for the trigger condition once at least 'pre'
 * samples are in the ring, and once triggered waits for the post-trigger
 * samples to land before stopping the ADC and copying the window
 * [trigger - pre, trigger + post) out of the ring into a linear result
 * buffer. So: pre-trigger, no missed edges between "wait for trigger"
 * and "start capturing", and the trigger index is exact.
 *
 * Zephyr's &adc device still owns the peripheral (clock, pinctrl, IRQ);
 * a capture borrows it under board_adc_mutex: parks the driver's IRQ,
 * reprograms FIFO/clock divider for free-running + DMA, and puts both
 * back afterwards, so board_adc_read_mv() keeps working in between.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <hardware/adc.h>
#include <hardware/dma.h>
#include <string.h>

#include "scope.h"
#include "board.h"
#include "arena.h"
#include "display.h"
#include "proto.h"

LOG_MODULE_REGISTER(scope, LOG_LEVEL_INF);

#define RING_SAMPLES        (ARENA_SCOPE_BYTES / 2)
#define DEFAULT_TIMEOUT_MS  5000
#define POLL_MS             1
#define SCAN_GUARD_SAMPLES  8

extern struct k_mutex board_adc_mutex;

static uint16_t result[SCOPE_MAX_SAMPLES];

static struct {
  struct scope_params p;
  enum capture_state state;
  uint32_t rate_hz;       /* samples per second, all channels together */
  uint16_t trig_index;    /* sample index (per channel) of the trigger in result[] */
  uint32_t total;         /* total samples (both channels) in result[] */
  int dma_chan;
  bool abort_req;
  struct k_sem start;
  struct k_mutex lock;
} scope;

K_THREAD_STACK_DEFINE(scope_stack, 2048);
static struct k_thread scope_thread_data;

static void scope_ui(const char *what)
{
  uint32_t per_ch = scope.rate_hz / scope.p.channels;

  if (per_ch >= 1000) {
    ui_set(UI_ROW_SCOPE, "%uch %u.%ukS/s %s", scope.p.channels, per_ch / 1000,
           (per_ch % 1000) / 100, what);
  } else {
    ui_set(UI_ROW_SCOPE, "%uch %uS/s %s", scope.p.channels, per_ch, what);
  }
}

static uint32_t ring_write_index(void)
{
  return (dma_channel_hw_addr(scope.dma_chan)->write_addr - (uint32_t)arena_scope()) / 2;
}

static void adc_start_capture(void)
{
  dma_channel_config dc;

  adc_irq_set_enabled(false);
  adc_run(false);
  adc_set_round_robin(scope.p.channels == 2 ? 0x3 : 0);
  adc_select_input(0);
  /* SDK: period = div + 1. At the minimum, a 96-cycle period lands each
   * start on the end of the previous conversion and the ADC drops every
   * other one (250 kS/s instead of 500); div 0 runs conversions back to
   * back, which is the same 96 cycles without the race. */
  adc_set_clkdiv(scope.p.clkdiv <= SCOPE_MIN_CLKDIV ? 0.f : (float)(scope.p.clkdiv - 1));
  adc_fifo_setup(true, true, 1, false, false);
  adc_fifo_drain();

  dc = dma_channel_get_default_config(scope.dma_chan);
  channel_config_set_transfer_data_size(&dc, DMA_SIZE_16);
  channel_config_set_read_increment(&dc, false);
  channel_config_set_write_increment(&dc, true);
  channel_config_set_ring(&dc, true, ARENA_SCOPE_BITS);
  channel_config_set_dreq(&dc, DREQ_ADC);
  dma_channel_configure(scope.dma_chan, &dc, arena_scope(), &adc_hw->fifo,
                        dma_encode_endless_transfer_count(), true);
  adc_run(true);
}

static void adc_stop_capture(void)
{
  adc_run(false);
  dma_channel_abort(scope.dma_chan);
  dma_channel_cleanup(scope.dma_chan);
  adc_set_round_robin(0);
  adc_fifo_setup(true, false, 1, true, true);
  adc_fifo_drain();
  adc_set_clkdiv(0.f);
  adc_irq_set_enabled(true);
}

/* Copies 'count' samples ending just before absolute index 'end' out of
 * the ring into result[] (handles the wrap). */
static void ring_copy(uint32_t end, uint32_t count)
{
  const uint16_t *ring = (const uint16_t *)arena_scope();
  uint32_t start = end - count;
  uint32_t i;

  for (i = 0; i < count; i++) {
    result[i] = ring[(start + i) % RING_SAMPLES];
  }
}

static void scope_thread(void *a, void *b, void *c)
{
  ARG_UNUSED(a);
  ARG_UNUSED(b);
  ARG_UNUSED(c);

  for (;;) {
    uint32_t ch, pre_total, post_total, need_total;
    uint32_t written = 0, last_wr = 0, scanned;
    uint32_t trig_abs = 0;
    bool triggered = false, timed_out = false;
    int64_t deadline;
    uint16_t prev = 0;
    bool have_prev = false;
    const uint16_t *ring = (const uint16_t *)arena_scope();
    uint8_t evt[2];

    k_sem_take(&scope.start, K_FOREVER);

    ch = scope.p.channels;
    pre_total = (uint32_t)scope.p.pre * ch;
    post_total = ((uint32_t)scope.p.n - scope.p.pre) * ch;
    need_total = pre_total + post_total;
    deadline = k_uptime_get() + (scope.p.timeout_ms ? scope.p.timeout_ms : DEFAULT_TIMEOUT_MS);

    k_mutex_lock(&board_adc_mutex, K_FOREVER);
    adc_start_capture();
    led_set(scope.p.trig_mode == SCOPE_TRIG_NONE ? LED_GREEN : LED_RED);
    scope_ui(scope.p.trig_mode == SCOPE_TRIG_NONE ? "capturing" : "armed");
    scanned = 0;

    while (!scope.abort_req) {
      uint32_t wr = ring_write_index();
      uint32_t delta = (wr - last_wr) % RING_SAMPLES;

      last_wr = wr;
      written += delta;

      if (!triggered) {
        if (scope.p.trig_mode == SCOPE_TRIG_NONE) {
          if (written >= pre_total) {
            triggered = true;
            trig_abs = pre_total;
          }
        } else {
          /* Scan new AIN0 samples (even indices when 2ch), staying a few
           * samples behind the DMA so a word still in flight is never read. */
          while (written >= scanned + ch + SCAN_GUARD_SAMPLES) {
            uint16_t s = ring[scanned % RING_SAMPLES];

            if (have_prev && scanned >= pre_total) {
              bool rising = prev < scope.p.trig_level && s >= scope.p.trig_level;
              bool falling = prev > scope.p.trig_level && s <= scope.p.trig_level;

              if ((scope.p.trig_mode == SCOPE_TRIG_RISING && rising) ||
                  (scope.p.trig_mode == SCOPE_TRIG_FALLING && falling)) {
                triggered = true;
                trig_abs = scanned;
                break;
              }
            }
            prev = s;
            have_prev = true;
            scanned += ch;
          }
          /* Lagging more than a ring behind means samples were lost:
           * only matters before the trigger, so just keep going. */
          if (written - scanned > RING_SAMPLES - 64) {
            scanned = written - (written % ch);
            have_prev = false;
          }
        }
        if (triggered) {
          scope.state = CAP_RUNNING;
          led_set(LED_GREEN);
          scope_ui("capturing");
        } else if (k_uptime_get() > deadline) {
          timed_out = true;
          break;
        }
      }
      if (triggered && written >= trig_abs + post_total) {
        /* Don't let the DMA overwrite the window before it's copied. */
        if (written - (trig_abs - pre_total) > RING_SAMPLES - 64) {
          /* Too slow: the pre-trigger part was overwritten. Shouldn't
           * happen at these rates, but degrade gracefully. */
          LOG_WRN("ring overrun while finishing capture");
        }
        break;
      }
      k_sleep(K_MSEC(POLL_MS));
    }

    adc_stop_capture();
    if (triggered && !scope.abort_req) {
      ring_copy(trig_abs + post_total, need_total);
      scope.total = need_total;
      scope.trig_index = scope.p.pre;
    }
    k_mutex_unlock(&board_adc_mutex);

    k_mutex_lock(&scope.lock, K_FOREVER);
    if (scope.abort_req) {
      scope.state = CAP_ABORTED;
      scope.total = 0;
    } else if (timed_out) {
      scope.state = CAP_TIMEOUT;
      scope.total = 0;
    } else {
      scope.state = CAP_DONE;
    }
    dma_channel_unclaim(scope.dma_chan);
    scope.dma_chan = -1;
    k_mutex_unlock(&scope.lock);

    if (scope.state == CAP_DONE) {
      ui_plot_analog(0, result, ch, 0, scope.p.n);
      if (ch == 2) {
        ui_plot_analog(1, result, ch, 1, scope.p.n);
      } else {
        ui_plot_clear();
        ui_plot_analog(0, result, 1, 0, scope.p.n);
      }
      scope_ui("done");
      led_flash(LED_GREEN, 100);
    } else {
      scope_ui(scope.state == CAP_TIMEOUT ? "no trigger" : "aborted");
      led_flash(LED_RED, 200);
    }
    evt[0] = 0;
    evt[1] = (uint8_t)scope.state;
    proto_send_event(EVT_CAPTURE_DONE, evt, 2);
  }
}

int scope_arm(const struct scope_params *p, uint32_t *rate_hz)
{
  struct scope_params sp = *p;

  if (sp.channels != 1 && sp.channels != 2) {
    return -EINVAL;
  }
  if (sp.n == 0 || (uint32_t)sp.n * sp.channels > SCOPE_MAX_SAMPLES || sp.pre > sp.n) {
    return -EINVAL;
  }
  if (sp.trig_mode > SCOPE_TRIG_FALLING || sp.trig_level > 4095) {
    return -EINVAL;
  }
  if (sp.clkdiv < SCOPE_MIN_CLKDIV) {
    sp.clkdiv = SCOPE_MIN_CLKDIV;
  }
  /* A trigger needs one previous sample; with pre == n there's no room
   * for anything after it. Both are legal but pointless - allow. */

  k_mutex_lock(&scope.lock, K_FOREVER);
  if (scope.state == CAP_ARMED || scope.state == CAP_RUNNING) {
    k_mutex_unlock(&scope.lock);
    return -EBUSY;
  }
  if (k_mutex_lock(&board_adc_mutex, K_NO_WAIT) != 0) {
    k_mutex_unlock(&scope.lock);
    return -EBUSY;
  }
  k_mutex_unlock(&board_adc_mutex);
  scope.dma_chan = dma_claim_unused_channel(false);
  if (scope.dma_chan < 0) {
    k_mutex_unlock(&scope.lock);
    return -EBUSY;
  }
  scope.p = sp;
  scope.rate_hz = SCOPE_ADC_CLOCK_HZ / sp.clkdiv;
  scope.total = 0;
  scope.trig_index = 0;
  scope.abort_req = false;
  scope.state = CAP_ARMED;
  k_mutex_unlock(&scope.lock);
  if (rate_hz != NULL) {
    *rate_hz = scope.rate_hz;
  }
  k_sem_give(&scope.start);
  return 0;
}

int scope_abort(void)
{
  if (scope.state == CAP_ARMED || scope.state == CAP_RUNNING) {
    scope.abort_req = true;
  }
  return 0;
}

enum capture_state scope_state(void)
{
  return scope.state;
}

void scope_status(uint8_t *channels, uint16_t *n, uint32_t *rate_hz, uint16_t *trig_index)
{
  *channels = scope.p.channels ? scope.p.channels : 1;
  *n = scope.p.n;
  *rate_hz = scope.rate_hz;
  *trig_index = scope.trig_index;
}

int scope_read(uint16_t offset, uint16_t count, uint8_t *out)
{
  if (scope.state != CAP_DONE) {
    return -EPERM;
  }
  if (offset >= scope.total) {
    return -EINVAL;
  }
  if ((uint32_t)offset + count > scope.total) {
    count = (uint16_t)(scope.total - offset);
  }
  memcpy(out, &result[offset], (size_t)count * 2);
  return count;
}

const uint16_t *scope_samples(void)
{
  return result;
}

int scope_capture_blocking(const struct scope_params *p, uint32_t *rate_hz)
{
  int ret = scope_arm(p, rate_hz);

  if (ret != 0) {
    return ret;
  }
  while (scope.state == CAP_ARMED || scope.state == CAP_RUNNING) {
    k_sleep(K_MSEC(5));
  }
  switch (scope.state) {
  case CAP_DONE:
    return 0;
  case CAP_TIMEOUT:
    return -ETIMEDOUT;
  default:
    return -ECANCELED;
  }
}

void scope_init(void)
{
  k_mutex_init(&scope.lock);
  k_sem_init(&scope.start, 0, 1);
  scope.state = CAP_IDLE;
  scope.dma_chan = -1;
  scope.p.channels = 1;
  scope.p.clkdiv = SCOPE_MIN_CLKDIV;
  scope.rate_hz = SCOPE_ADC_CLOCK_HZ / SCOPE_MIN_CLKDIV;
  k_thread_create(&scope_thread_data, scope_stack, K_THREAD_STACK_SIZEOF(scope_stack),
                  scope_thread, NULL, NULL, NULL, K_PRIO_PREEMPT(4), 0, K_NO_WAIT);
  k_thread_name_set(&scope_thread_data, "scope");
  ui_set(UI_ROW_SCOPE, "idle");
}
