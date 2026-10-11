/*
 * Tramuntana - pwm: hardware PWM outputs on the DIO header. Any DIO can
 * be a PWM output with its own duty and polarity, but the pins come in
 * pairs that share one RP2350 PWM slice - and so one frequency:
 *
 *   slice 0: DIO7 (A) / DIO6 (B)    slice 2: DIO3 (A) / DIO2 (B)
 *   slice 1: DIO5 (A) / DIO4 (B)    slice 3: DIO1 (A) / DIO0 (B)
 *
 * See pwm.c.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#define PWM_OUT_MIN_FREQ_HZ  10
#define PWM_OUT_DUTY_FULL    10000   /* duty unit: 0.01 % */
#define PWM_OUT_SLICES       4

/* Slice / pair partner of a DIO. */
#define PWM_OUT_SLICE(dio)   ((7 - (dio)) >> 1)
#define PWM_OUT_PARTNER(dio) ((dio) ^ 1)

struct pwm_out_status {
  bool active;         /* configured as PWM by pwm_out_set() */
  bool lost;           /* active, but another instrument has taken the pin */
  bool invert;
  uint16_t duty;       /* requested, 0.01 % units */
  uint32_t freq_hz;    /* requested */
  uint32_t freq_mhz;   /* achieved, millihertz (slice-wide) */
  uint16_t level;      /* compare value */
  uint16_t top;        /* slice wrap */
  uint8_t div;         /* slice integer clock divider */
  uint16_t phase_deg;  /* slice phase offset applied by pwm_out_sync() */
};

/* Drives 'dio' with a PWM of 'freq_hz' and 'duty' (0..PWM_OUT_DUTY_FULL).
 * Fails with -EBUSY if the pair partner is a live PWM at a different
 * frequency (or the pin is held exclusively), -EINVAL if the frequency
 * is out of range. Changing duty or polarity of a running output is
 * glitch-free. 'out' (optional) gets the achieved settings. */
int pwm_out_set(int dio, uint32_t freq_hz, uint16_t duty, bool invert, struct pwm_out_status *out);

/* Phase offset (0..359 degrees, delay) of the slice that 'dio' belongs
 * to - both pins of the pair share it. Takes effect on pwm_out_sync(). */
int pwm_out_phase(int dio, uint16_t phase_deg);

/* Restarts the slices behind 'dio_mask' (0 = every live slice) in the
 * same clock cycle, each with its counter preset to its phase offset. */
int pwm_out_sync(uint8_t dio_mask);

/* Stops the PWM on the DIOs in 'dio_mask' and hands the pins back as
 * plain GPIO inputs. */
void pwm_out_stop(uint8_t dio_mask);

void pwm_out_get_status(int dio, struct pwm_out_status *out);
