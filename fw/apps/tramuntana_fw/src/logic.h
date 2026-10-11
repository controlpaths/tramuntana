/*
 * Tramuntana - logic: 8 channel logic analyzer on DIO0..DIO7, sampled by
 * a PIO state machine (up to 150MS/s) into a 128KB DMA ring. See logic.c.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "proto.h"

#define LOGIC_MAX_SAMPLES 131072
#define LOGIC_MAX_CLKDIV  65535

enum logic_trigger {
  LOGIC_TRIG_NONE = 0,
  LOGIC_TRIG_RISING = 1,    /* PIO waits for an edge on trig_pin; with pre > 0 a one-pin pattern */
  LOGIC_TRIG_FALLING = 2,
  LOGIC_TRIG_PATTERN = 3,   /* (sample & mask) becomes == value, CPU scanned, pre-trigger ok */
};

struct logic_params {
  uint32_t n;          /* total samples */
  uint16_t clkdiv;     /* sample rate = sysclk / clkdiv (>= 1) */
  uint8_t trig_mode;
  uint8_t trig_pin;    /* DIO index for edge triggers */
  uint8_t trig_mask;
  uint8_t trig_value;
  uint32_t pre;        /* samples kept before the trigger (pattern, or edge at <= 15MS/s) */
  uint16_t timeout_ms; /* 0: default */
};

void logic_init(void);
int logic_arm(const struct logic_params *p, uint32_t *rate_hz);
int logic_abort(void);
enum capture_state logic_state(void);
void logic_status(uint32_t *n, uint32_t *rate_hz, uint32_t *trig_index);
/* Samples are returned with bit i = DIOi. */
int logic_read(uint32_t offset, uint16_t count, uint8_t *out);
int logic_capture_blocking(const struct logic_params *p, uint32_t *rate_hz);
