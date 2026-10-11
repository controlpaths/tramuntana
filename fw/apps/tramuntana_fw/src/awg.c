/*
 * Tramuntana - awg. See awg.h.
 *
 * AOUT0 (J1.7) is GPIO8 behind R3 (2k7) and C3 (1nF): a first-order RC
 * low-pass with its corner at 59kHz. There's no DAC - the "analog"
 * output is the average of a PWM on GPIO8 (slice 4, channel A), so the
 * PWM runs well above that corner. TOP = 255 (8 bits, 586kHz at
 * 150MHz) is the sweet spot for this filter; awg_plan() stretches TOP a
 * little (up to 511) when that lands closer to the requested frequency.
 *
 * One period of the waveform is pre-computed into awg_table[] as PWM
 * compare values and a DMA channel feeds it into the slice's CC register
 * paced by the slice's wrap DREQ - one sample per PWM period. A second,
 * one-word DMA channel restarts the first when the table's done (the
 * pico-sdk "control channel" idiom), so the output keeps going with no
 * CPU involvement until awg_off().
 *
 * Frequency planning: period = clk_sys / f cycles, split between the
 * PWM clock divider (integer, only > 1 at low frequencies), the table
 * length N (as many samples per period as the PWM rate allows, up to
 * AWG_TABLE_MAX) and TOP+1, nudged so the product hits the requested
 * frequency as closely as possible (usually within 0.1%).
 *
 * ARB: the host uploads up to AWG_ARB_MAX 8-bit samples (0..255 =
 * 0..3.3V) with awg_arb_load(); awg_set(AWG_ARB, f, ...) then plays the
 * whole table f times a second, resampled to whatever N the planner
 * picks, scaled by amp/offset (amp = 1650mV, off = 1650mV reproduces
 * the table full-scale).
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <hardware/pwm.h>
#include <hardware/dma.h>
#include <math.h>
#include <string.h>

#include "awg.h"
#include "board.h"
#include "display.h"

LOG_MODULE_REGISTER(awg, LOG_LEVEL_INF);

#define AWG_GPIO          GPIO_AOUT0
#define AWG_PWM_MIN_TOP   255
#define AWG_PWM_MAX_TOP   511
#define AWG_TABLE_MAX     4096
#define AWG_MAX_DIV       255
#define AWG_TWO_PI        6.28318530718f

static uint32_t awg_table[AWG_TABLE_MAX];
static const uint32_t *const awg_table_start = awg_table;
static uint8_t arb[AWG_ARB_MAX];
static uint16_t arb_len;

static struct {
  bool running;
  int dma_data;
  int dma_ctrl;
  enum awg_shape shape;
  uint32_t freq_mhz;
  int32_t amp_mv, off_mv;
  uint32_t param;
  uint32_t n, top, div, pwm_hz;
  struct k_mutex lock;
} awg = { .dma_data = -1, .dma_ctrl = -1, .shape = AWG_OFF };

static const char *const shape_names[] = {
  [AWG_SINE] = "SINE", [AWG_TRI] = "TRI", [AWG_SAW] = "SAW", [AWG_SQUARE] = "SQUARE",
  [AWG_STAIRS] = "STAIRS", [AWG_ARB] = "ARB", [AWG_DC] = "DC", [AWG_STEP] = "STEP",
  [AWG_OFF] = "OFF",
};

const char *awg_shape_name(enum awg_shape s)
{
  return (s <= AWG_OFF) ? shape_names[s] : "?";
}

static void awg_ui(void)
{
  uint32_t mhz = awg.freq_mhz;

  if (awg.shape == AWG_OFF) {
    ui_set(UI_ROW_AWG, "off");
  } else if (awg.shape == AWG_DC || awg.shape == AWG_STEP) {
    ui_set(UI_ROW_AWG, "%s %d.%02dV", shape_names[awg.shape], awg.off_mv / 1000,
           (awg.off_mv % 1000) / 10);
  } else if (mhz >= 1000000) {
    ui_set(UI_ROW_AWG, "%s %u.%03ukHz %d.%02dVp", shape_names[awg.shape], mhz / 1000000,
           (mhz / 1000) % 1000, awg.amp_mv / 1000, (awg.amp_mv % 1000) / 10);
  } else {
    ui_set(UI_ROW_AWG, "%s %u.%uHz %d.%02dVp", shape_names[awg.shape], mhz / 1000,
           (mhz / 100) % 10, awg.amp_mv / 1000, (awg.amp_mv % 1000) / 10);
  }
}

static uint32_t mv_to_level(int32_t mv)
{
  mv = CLAMP(mv, 0, AWG_VREF_MV);
  return ((uint32_t)mv * awg.top + AWG_VREF_MV / 2) / AWG_VREF_MV;
}

static void awg_plan(uint32_t clk_hz, uint32_t freq_hz)
{
  double cycles = (double)clk_hz / (double)freq_hz;
  uint32_t div = 1;
  double per;
  uint32_t n, top1 = 0;
  double best_err = -1.0;
  uint32_t cand;

  while (cycles / div > (double)AWG_TABLE_MAX * (AWG_PWM_MAX_TOP + 1) && div < AWG_MAX_DIV) {
    div++;
  }
  per = cycles / div;
  n = (uint32_t)(per / (AWG_PWM_MIN_TOP + 1));
  n = CLAMP(n, 2, AWG_TABLE_MAX);
  for (cand = (n > 4) ? n - 3 : 2; cand <= n + 3 && cand <= AWG_TABLE_MAX; cand++) {
    uint32_t t1 = (uint32_t)(per / cand + 0.5);
    double err;

    t1 = CLAMP(t1, AWG_PWM_MIN_TOP + 1, AWG_PWM_MAX_TOP + 1);
    err = fabs((double)cand * t1 - per);
    if (best_err < 0.0 || err < best_err) {
      best_err = err;
      n = cand;
      top1 = t1;
    }
  }
  awg.div = div;
  awg.n = n;
  awg.top = top1 - 1;
  awg.pwm_hz = clk_hz / (div * top1);
  awg.freq_mhz = (uint32_t)(((uint64_t)clk_hz * 1000ULL) / ((uint64_t)div * n * top1));
}

static void awg_fill_table(void)
{
  uint32_t i;

  for (i = 0; i < awg.n; i++) {
    float phase = (float)i / (float)awg.n;
    float unit;

    switch (awg.shape) {
    case AWG_SINE:
      unit = sinf(AWG_TWO_PI * phase);
      break;
    case AWG_TRI:
      unit = (phase < 0.5f) ? (4.0f * phase - 1.0f) : (3.0f - 4.0f * phase);
      break;
    case AWG_SAW:
      unit = 2.0f * phase - 1.0f;
      break;
    case AWG_SQUARE:
      unit = (phase * 100.0f < (float)awg.param) ? 1.0f : -1.0f;
      break;
    case AWG_STAIRS: {
      uint32_t k = (uint32_t)(phase * (float)awg.param);

      unit = -1.0f + 2.0f * (float)k / (float)(awg.param - 1);
      break;
    }
    case AWG_ARB: {
      /* Nearest-sample resampling of the uploaded table. */
      uint32_t k = (uint32_t)(phase * (float)arb_len);

      if (k >= arb_len) {
        k = arb_len - 1;
      }
      unit = ((float)arb[k] - 127.5f) / 127.5f;
      break;
    }
    default:
      unit = 0.0f;
      break;
    }
    awg_table[i] = mv_to_level(awg.off_mv + (int32_t)((float)awg.amp_mv * unit));
  }
}

static void awg_stop_dma(uint32_t level)
{
  uint slice = pwm_gpio_to_slice_num(AWG_GPIO);

  if (awg.dma_data >= 0) {
    dma_hw->abort = BIT(awg.dma_data) | BIT(awg.dma_ctrl);
    while (dma_hw->abort) {
      /* wait */
    }
    dma_channel_cleanup(awg.dma_data);
    dma_channel_cleanup(awg.dma_ctrl);
    dma_channel_unclaim(awg.dma_data);
    dma_channel_unclaim(awg.dma_ctrl);
    awg.dma_data = awg.dma_ctrl = -1;
  }
  awg.running = false;
  pwm_set_chan_level(slice, PWM_CHAN_A, (uint16_t)level);
}

static void awg_pwm_setup(void)
{
  uint slice = pwm_gpio_to_slice_num(AWG_GPIO);

  pwm_set_clkdiv_int_frac(slice, (uint8_t)awg.div, 0);
  pwm_set_wrap(slice, (uint16_t)awg.top);
  pwm_set_enabled(slice, true);
}

static int awg_start_loop(void)
{
  uint slice = pwm_gpio_to_slice_num(AWG_GPIO);
  dma_channel_config cfg;

  awg.dma_data = dma_claim_unused_channel(false);
  awg.dma_ctrl = dma_claim_unused_channel(false);
  if (awg.dma_data < 0 || awg.dma_ctrl < 0) {
    if (awg.dma_data >= 0) {
      dma_channel_unclaim(awg.dma_data);
    }
    if (awg.dma_ctrl >= 0) {
      dma_channel_unclaim(awg.dma_ctrl);
    }
    awg.dma_data = awg.dma_ctrl = -1;
    return -EBUSY;
  }

  cfg = dma_channel_get_default_config(awg.dma_data);
  channel_config_set_transfer_data_size(&cfg, DMA_SIZE_32);
  channel_config_set_read_increment(&cfg, true);
  channel_config_set_write_increment(&cfg, false);
  channel_config_set_dreq(&cfg, pwm_get_dreq(slice));
  channel_config_set_chain_to(&cfg, awg.dma_ctrl);
  dma_channel_configure(awg.dma_data, &cfg, &pwm_hw->slice[slice].cc, awg_table, awg.n, false);

  cfg = dma_channel_get_default_config(awg.dma_ctrl);
  channel_config_set_transfer_data_size(&cfg, DMA_SIZE_32);
  channel_config_set_read_increment(&cfg, false);
  channel_config_set_write_increment(&cfg, false);
  dma_channel_configure(awg.dma_ctrl, &cfg, &dma_hw->ch[awg.dma_data].al3_read_addr_trig,
                        &awg_table_start, 1, false);

  pwm_set_chan_level(slice, PWM_CHAN_A, (uint16_t)awg_table[0]);
  awg_pwm_setup();
  dma_channel_start(awg.dma_data);
  awg.running = true;
  led_background(LED_MAGENTA);
  return 0;
}

int awg_set(enum awg_shape shape, uint32_t freq_hz, uint16_t amp_mv, uint16_t off_mv,
            uint16_t param, struct awg_status *out)
{
  int ret;

  if (shape > AWG_ARB) {
    return -EINVAL;
  }
  if (freq_hz < AWG_MIN_FREQ_HZ || freq_hz > AWG_MAX_FREQ_HZ) {
    return -EINVAL;
  }
  if (amp_mv > AWG_VREF_MV || off_mv > AWG_VREF_MV) {
    return -EINVAL;
  }
  if (shape == AWG_SQUARE) {
    param = (param == 0 || param > 100) ? 50 : param;
  } else if (shape == AWG_STAIRS) {
    param = (param < 2) ? 8 : MIN(param, 256);
  } else if (shape == AWG_ARB && arb_len == 0) {
    return -EINVAL;
  }

  k_mutex_lock(&awg.lock, K_FOREVER);
  awg_stop_dma(0);
  awg.shape = shape;
  awg.amp_mv = amp_mv;
  awg.off_mv = off_mv;
  awg.param = param;
  awg_plan(board_sysclk_hz(), freq_hz);
  awg_fill_table();
  ret = awg_start_loop();
  if (ret != 0) {
    awg.shape = AWG_OFF;
  }
  k_mutex_unlock(&awg.lock);
  awg_ui();
  if (out != NULL) {
    awg_get_status(out);
  }
  return ret;
}

int awg_dc(uint16_t mv)
{
  if (mv > AWG_VREF_MV) {
    return -EINVAL;
  }
  k_mutex_lock(&awg.lock, K_FOREVER);
  awg.shape = AWG_DC;
  awg.off_mv = mv;
  awg.amp_mv = 0;
  awg.freq_mhz = 0;
  awg.div = 1;
  awg.top = AWG_PWM_MIN_TOP;
  awg_stop_dma(0);
  awg_pwm_setup();
  pwm_set_chan_level(pwm_gpio_to_slice_num(AWG_GPIO), PWM_CHAN_A, (uint16_t)mv_to_level(mv));
  led_background(LED_MAGENTA);
  k_mutex_unlock(&awg.lock);
  awg_ui();
  return 0;
}

int awg_step(uint16_t from_mv, uint16_t to_mv, uint16_t delay_ms)
{
  int ret = awg_dc(from_mv);

  if (ret != 0) {
    return ret;
  }
  k_sleep(K_MSEC(delay_ms));
  ret = awg_dc(to_mv);
  awg.shape = AWG_STEP;
  awg_ui();
  return ret;
}

void awg_off(void)
{
  k_mutex_lock(&awg.lock, K_FOREVER);
  awg_stop_dma(0);
  awg.shape = AWG_OFF;
  awg.freq_mhz = 0;
  led_background(LED_OFF);
  k_mutex_unlock(&awg.lock);
  awg_ui();
}

int awg_arb_load(uint16_t offset, const uint8_t *samples, size_t n)
{
  if (n == 0 || (size_t)offset + n > AWG_ARB_MAX) {
    return -EINVAL;
  }
  k_mutex_lock(&awg.lock, K_FOREVER);
  memcpy(&arb[offset], samples, n);
  /* Loading from 0 restarts the table; appending extends it. */
  arb_len = (offset == 0) ? (uint16_t)n : (uint16_t)MAX(arb_len, offset + n);
  k_mutex_unlock(&awg.lock);
  return 0;
}

uint16_t awg_arb_len(void)
{
  return arb_len;
}

void awg_get_status(struct awg_status *out)
{
  out->running = awg.running || awg.shape == AWG_DC || awg.shape == AWG_STEP;
  out->shape = awg.shape;
  out->freq_mhz = awg.freq_mhz;
  out->amp_mv = (uint16_t)awg.amp_mv;
  out->off_mv = (uint16_t)awg.off_mv;
  out->n = (uint16_t)awg.n;
  out->top = (uint16_t)awg.top;
}

void awg_init(void)
{
  k_mutex_init(&awg.lock);
  awg.shape = AWG_OFF;
  awg.top = AWG_PWM_MIN_TOP;
  awg.div = 1;
  awg_ui();
}
