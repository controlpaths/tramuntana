/*
 * Tramuntana - scope: 1 or 2 channel capture off AIN0/AIN1 (12-bit ADC,
 * up to ~500kS/s) through a DMA ring, with pre-trigger. See scope.c.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "proto.h"

#define SCOPE_MAX_SAMPLES   16384   /* total (both channels together) */
#define SCOPE_ADC_CLOCK_HZ  48000000
#define SCOPE_MIN_CLKDIV    96      /* one 12-bit conversion = 96 ADC clocks */

enum scope_trigger { SCOPE_TRIG_NONE = 0, SCOPE_TRIG_RISING = 1, SCOPE_TRIG_FALLING = 2 };

struct scope_params {
  uint8_t channels;    /* 1: AIN0, 2: AIN0+AIN1 interleaved */
  uint16_t n;          /* samples per channel */
  uint16_t clkdiv;     /* ADC clock divider (0 or <96 -> 96: 500kS/s) */
  uint8_t trig_mode;   /* enum scope_trigger, always on AIN0 */
  uint16_t trig_level; /* raw 12-bit */
  uint16_t pre;        /* samples per channel kept before the trigger */
  uint16_t timeout_ms; /* 0: default */
};

void scope_init(void);
int scope_arm(const struct scope_params *p, uint32_t *rate_hz);
int scope_abort(void);
enum capture_state scope_state(void);
void scope_status(uint8_t *channels, uint16_t *n, uint32_t *rate_hz, uint16_t *trig_index);
/* Copies 'count' samples (interleaved ch0,ch1,... when 2ch) starting at
 * sample 'offset' of the finished capture. Returns copied count or -errno. */
int scope_read(uint16_t offset, uint16_t count, uint8_t *out);
/* Arms and waits for the capture to finish (shell use). */
int scope_capture_blocking(const struct scope_params *p, uint32_t *rate_hz);
const uint16_t *scope_samples(void);
