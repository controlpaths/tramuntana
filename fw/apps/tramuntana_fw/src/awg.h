/*
 * Tramuntana - awg: waveform generator on AOUT0 (PWM 4A through the
 * board's 2k7/1nF RC filter), fed by DMA so it runs in the background.
 * See awg.c.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

enum awg_shape {
  AWG_SINE = 0, AWG_TRI = 1, AWG_SAW = 2, AWG_SQUARE = 3, AWG_STAIRS = 4, AWG_ARB = 5,
  AWG_DC = 6, AWG_STEP = 7, AWG_OFF = 8,
};

#define AWG_MIN_FREQ_HZ  1
#define AWG_MAX_FREQ_HZ  100000
#define AWG_ARB_MAX      4096
#define AWG_VREF_MV      3300

struct awg_status {
  bool running;
  enum awg_shape shape;
  uint32_t freq_mhz;   /* achieved, millihertz */
  uint16_t amp_mv;
  uint16_t off_mv;
  uint16_t n;          /* table length */
  uint16_t top;        /* PWM wrap */
};

void awg_init(void);
/* Periodic shapes (SINE..ARB). 'param': duty % for SQUARE, steps for
 * STAIRS, ignored otherwise. For ARB the table loaded with awg_arb_load()
 * is played back at 'freq' periods per second. */
int awg_set(enum awg_shape shape, uint32_t freq_hz, uint16_t amp_mv, uint16_t off_mv,
            uint16_t param, struct awg_status *out);
int awg_dc(uint16_t mv);
int awg_step(uint16_t from_mv, uint16_t to_mv, uint16_t delay_ms);
void awg_off(void);
int awg_arb_load(uint16_t offset, const uint8_t *samples, size_t n);
uint16_t awg_arb_len(void);
void awg_get_status(struct awg_status *out);
const char *awg_shape_name(enum awg_shape s);
