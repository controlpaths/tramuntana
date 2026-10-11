/*
 * Tramuntana - pwm. See pwm.h.
 *
 * DIOn = GPIO(7-n), so the eight header pins land on PWM slices 0..3,
 * two pins per slice. A slice has one counter, one clock divider and one
 * TOP: both pins of a pair always share frequency and phase, each with
 * its own compare level (duty) and polarity. Slice 4 is the AWG's
 * (GPIO8) and is never touched here.
 *
 * Frequency planning: period = clk_sys / f cycles = DIV * (TOP + 1),
 * with TOP + 1 <= 65535. Only the integer part of the divider is used:
 * a fractional divider makes the period dither between two lengths
 * (jitter), and with a 16-bit TOP the integer split already lands within
 * 1/(2*(TOP+1)) of the target - well under 0.01 % below ~2 kHz, and at
 * high frequencies the error is set by TOP being an integer, which a
 * fractional divider can't fix anyway. Range at 150 MHz: ~9 Hz (DIV 255,
 * TOP 65534) to 75 MHz (TOP 1), with duty resolution 1/(TOP+1).
 *
 * Duty: output high while counter < CC, so CC = 0 is 0 % and
 * CC = TOP + 1 is 100 %. TOP and CC are double-buffered by the hardware
 * (latched at wrap), so duty changes never glitch; the divider is not,
 * so a frequency change on a running slice may produce one odd period.
 *
 * Synchronisation: pwm_out_sync() stops the slices, presets each counter
 * to its phase offset and re-enables them with a single write to the
 * EN register's atomic-set alias, so they all start on the same clk_sys
 * edge and, sharing clk_sys, never drift: same or integer-ratio
 * frequencies keep their phase relationship for good.
 *
 * Pins are claimed as OWNER_PWM (soft): any other instrument can take
 * one over. The slice keeps running, the pin just stops showing it, and
 * the channel is then "lost" - it no longer pins the pair's frequency.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <hardware/pwm.h>
#include <hardware/gpio.h>

#include "pwm.h"
#include "board.h"

LOG_MODULE_REGISTER(pwm_out, LOG_LEVEL_INF);

#define PWM_OUT_MAX_DIV  255
/* TOP + 1 <= 65535 keeps CC = TOP + 1 (100 %) inside the 16-bit CC. */
#define PWM_OUT_MAX_TOP1 65535

struct pwm_out_chan {
  bool active;
  bool invert;
  uint16_t duty;
  uint16_t level;
};

struct pwm_out_slice {
  uint32_t freq_hz;    /* requested */
  uint32_t freq_mhz;   /* achieved */
  uint32_t top1;       /* TOP + 1 */
  uint8_t div;
  uint16_t phase_deg;
  struct pwm_out_chan chan[2];   /* PWM_CHAN_A, PWM_CHAN_B */
};

static struct pwm_out_slice slices[PWM_OUT_SLICES];
K_MUTEX_DEFINE(pwm_out_lock);

static inline uint chan_of(int dio)
{
  return pwm_gpio_to_channel(DIO_GPIO(dio));
}

/* Configured and still holding its pin. */
static bool chan_live(int dio)
{
  return slices[PWM_OUT_SLICE(dio)].chan[chan_of(dio)].active &&
         board_pin_owner(dio) == OWNER_PWM;
}

static bool slice_live(uint slice)
{
  int dio_a = 7 - (int)(2 * slice);

  return chan_live(dio_a) || chan_live(PWM_OUT_PARTNER(dio_a));
}

static int plan(uint32_t clk_hz, uint32_t freq_hz, uint8_t *div, uint32_t *top1)
{
  uint32_t cycles, d, t1;

  if (freq_hz < PWM_OUT_MIN_FREQ_HZ || freq_hz > clk_hz / 2) {
    return -EINVAL;
  }
  cycles = (clk_hz + freq_hz / 2) / freq_hz;
  d = (cycles + PWM_OUT_MAX_TOP1 - 1) / PWM_OUT_MAX_TOP1;
  d = MAX(d, 1);
  if (d > PWM_OUT_MAX_DIV) {
    return -EINVAL;
  }
  t1 = (uint32_t)(((uint64_t)clk_hz + (uint64_t)d * freq_hz / 2) / ((uint64_t)d * freq_hz));
  *div = (uint8_t)d;
  *top1 = CLAMP(t1, 2, PWM_OUT_MAX_TOP1);
  return 0;
}

static uint16_t duty_to_level(uint16_t duty, uint32_t top1)
{
  return (uint16_t)(((uint64_t)duty * top1 + PWM_OUT_DUTY_FULL / 2) / PWM_OUT_DUTY_FULL);
}

/* Full slice setup: divider, wrap, both levels, polarity. */
static void slice_apply(uint slice)
{
  struct pwm_out_slice *s = &slices[slice];

  pwm_set_clkdiv_int_frac(slice, s->div, 0);
  pwm_set_wrap(slice, (uint16_t)(s->top1 - 1));
  pwm_set_both_levels(slice, s->chan[PWM_CHAN_A].level, s->chan[PWM_CHAN_B].level);
  pwm_set_output_polarity(slice, s->chan[PWM_CHAN_A].invert, s->chan[PWM_CHAN_B].invert);
}

static void fill_status(int dio, struct pwm_out_status *out)
{
  const struct pwm_out_slice *s = &slices[PWM_OUT_SLICE(dio)];
  const struct pwm_out_chan *c = &s->chan[chan_of(dio)];

  out->active = c->active;
  out->lost = c->active && board_pin_owner(dio) != OWNER_PWM;
  out->invert = c->invert;
  out->duty = c->duty;
  out->freq_hz = s->freq_hz;
  out->freq_mhz = s->freq_mhz;
  out->level = c->level;
  out->top = (uint16_t)(s->top1 - 1);
  out->div = s->div;
  out->phase_deg = s->phase_deg;
}

int pwm_out_set(int dio, uint32_t freq_hz, uint16_t duty, bool invert, struct pwm_out_status *out)
{
  uint slice, ch;
  struct pwm_out_slice *s;
  struct pwm_out_slice saved;
  bool partner_live, was_enabled;
  uint32_t clk_hz = board_sysclk_hz();
  uint8_t div;
  uint32_t top1;
  int ret;

  if (dio < 0 || dio >= DIO_COUNT || duty > PWM_OUT_DUTY_FULL) {
    return -EINVAL;
  }
  ret = plan(clk_hz, freq_hz, &div, &top1);
  if (ret != 0) {
    return ret;
  }
  slice = PWM_OUT_SLICE(dio);
  ch = chan_of(dio);
  s = &slices[slice];

  k_mutex_lock(&pwm_out_lock, K_FOREVER);
  partner_live = chan_live(PWM_OUT_PARTNER(dio));
  if (partner_live && s->freq_hz != freq_hz) {
    k_mutex_unlock(&pwm_out_lock);
    LOG_DBG("DIO%d: partner DIO%d runs at %u Hz", dio, PWM_OUT_PARTNER(dio), s->freq_hz);
    return -EBUSY;
  }
  if (!partner_live) {
    /* A partner that lost its pin doesn't count any more. */
    s->chan[ch ^ 1].active = false;
    s->chan[ch ^ 1].level = 0;
  }
  saved = *s;
  was_enabled = pwm_hw->slice[slice].csr & PWM_CH0_CSR_EN_BITS;

  s->freq_hz = freq_hz;
  s->div = div;
  s->top1 = top1;
  s->freq_mhz = (uint32_t)(((uint64_t)clk_hz * 1000ULL) / ((uint64_t)div * top1));
  s->chan[ch].active = true;
  s->chan[ch].invert = invert;
  s->chan[ch].duty = duty;
  s->chan[ch].level = duty_to_level(duty, top1);

  if (partner_live) {
    /* Same frequency: only this channel's level/polarity changes, both
     * latched glitch-free (CC at wrap, polarity is a plain inverter). */
    pwm_set_chan_level(slice, ch, s->chan[ch].level);
    pwm_set_output_polarity(slice, s->chan[PWM_CHAN_A].invert, s->chan[PWM_CHAN_B].invert);
  } else {
    slice_apply(slice);
    pwm_set_enabled(slice, true);
  }

  /* Pad mux last, so the pin goes straight from input to the configured
   * waveform. */
  ret = board_pins_claim(BIT(dio), OWNER_PWM, false);
  if (ret != 0) {
    *s = saved;
    slice_apply(slice);
    if (!was_enabled) {
      pwm_set_enabled(slice, false);
    }
    k_mutex_unlock(&pwm_out_lock);
    return ret;
  }
  if (out != NULL) {
    fill_status(dio, out);
  }
  k_mutex_unlock(&pwm_out_lock);
  return 0;
}

int pwm_out_phase(int dio, uint16_t phase_deg)
{
  if (dio < 0 || dio >= DIO_COUNT || phase_deg >= 360) {
    return -EINVAL;
  }
  k_mutex_lock(&pwm_out_lock, K_FOREVER);
  slices[PWM_OUT_SLICE(dio)].phase_deg = phase_deg;
  k_mutex_unlock(&pwm_out_lock);
  return 0;
}

int pwm_out_sync(uint8_t dio_mask)
{
  uint32_t en_mask = 0;
  uint slice;
  int i;

  k_mutex_lock(&pwm_out_lock, K_FOREVER);
  for (i = 0; i < DIO_COUNT; i++) {
    if ((dio_mask == 0 || (dio_mask & BIT(i))) && chan_live(i)) {
      en_mask |= BIT(PWM_OUT_SLICE(i));
    }
  }
  if (en_mask == 0) {
    k_mutex_unlock(&pwm_out_lock);
    return -EPERM;
  }
  hw_clear_bits(&pwm_hw->en, en_mask);
  for (slice = 0; slice < PWM_OUT_SLICES; slice++) {
    if (en_mask & BIT(slice)) {
      const struct pwm_out_slice *s = &slices[slice];
      /* Starting at TOP+1 - phase*(TOP+1)/360, the counter wraps (rising
       * edge) phase/360 of a period after a slice preset to 0. */
      uint32_t delay = (s->top1 * s->phase_deg + 180) / 360;

      pwm_set_counter(slice, (uint16_t)((s->top1 - delay) % s->top1));
    }
  }
  /* One write through the atomic-set alias: every slice in the mask
   * starts on the same clk_sys edge, the AWG's slice is left alone. */
  hw_set_bits(&pwm_hw->en, en_mask);
  k_mutex_unlock(&pwm_out_lock);
  return 0;
}

void pwm_out_stop(uint8_t dio_mask)
{
  uint32_t touched = 0;
  uint slice;
  int i;

  k_mutex_lock(&pwm_out_lock, K_FOREVER);
  board_pins_release(dio_mask, OWNER_PWM);
  for (i = 0; i < DIO_COUNT; i++) {
    if (dio_mask & BIT(i)) {
      struct pwm_out_chan *c = &slices[PWM_OUT_SLICE(i)].chan[chan_of(i)];

      c->active = false;
      c->level = 0;
      pwm_set_chan_level(PWM_OUT_SLICE(i), chan_of(i), 0);
      touched |= BIT(PWM_OUT_SLICE(i));
    }
  }
  for (slice = 0; slice < PWM_OUT_SLICES; slice++) {
    if ((touched & BIT(slice)) && !slice_live(slice)) {
      pwm_set_enabled(slice, false);
      slices[slice].chan[PWM_CHAN_A].active = false;
      slices[slice].chan[PWM_CHAN_B].active = false;
    }
  }
  k_mutex_unlock(&pwm_out_lock);
}

void pwm_out_get_status(int dio, struct pwm_out_status *out)
{
  k_mutex_lock(&pwm_out_lock, K_FOREVER);
  fill_status(dio, out);
  k_mutex_unlock(&pwm_out_lock);
}
