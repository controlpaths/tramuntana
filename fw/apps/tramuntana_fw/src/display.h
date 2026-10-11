/*
 * Tramuntana - display: the ST7735R panel (128x160) and the dashboard
 * drawn on it.
 *
 * The screen is a status board rather than a per-command readout: one
 * line per instrument (what it's configured to / what it last did), a
 * title bar with a short "activity" flash, and a small plot area at the
 * bottom that shows the last scope trace or logic capture. Instruments
 * call ui_set()/ui_plot_*() whenever their state changes; a low-priority
 * thread re-renders at most every UI_REFRESH_MS, so no instrument ever
 * blocks on the ~30ms SPI write of a full frame.
 *
 * Coordinates are plain top-left origin; the 180-degree rotation the
 * panel needs on this board happens in display_set_pixel(), same as the
 * images pre-rotated by fw/imgs/convert_images.py.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define DISPLAY_W 128
#define DISPLAY_H 160

#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

#define COLOR_BG     RGB565(0, 0, 0)
#define COLOR_TEXT   RGB565(255, 255, 255)
#define COLOR_DIM    RGB565(110, 110, 110)
#define COLOR_LABEL  RGB565(170, 170, 170)
#define COLOR_CH0    RGB565(0, 255, 255)
#define COLOR_CH1    RGB565(255, 230, 0)
#define COLOR_ALERT  RGB565(255, 60, 60)
#define COLOR_OK     RGB565(60, 255, 90)

/* Framebuffer primitives (fb is DISPLAY_W*DISPLAY_H*2 bytes, RGB565 BE). */
void display_set_pixel(uint8_t *fb, int x, int y, uint16_t color);
void display_draw_hline(uint8_t *fb, int x0, int x1, int y, uint16_t color);
void display_draw_vline(uint8_t *fb, int x, int y0, int y1, uint16_t color);
void display_fill_rect(uint8_t *fb, int x, int y, int w, int h, uint16_t color);
int display_draw_char(uint8_t *fb, int x, int y, char c, uint16_t color);
int display_draw_string(uint8_t *fb, int x, int y, const char *str, uint16_t color);
int display_draw_string_bold(uint8_t *fb, int x, int y, const char *str, uint16_t color);
int display_string_width(const char *str, bool bold);

/* Splashes the logo and starts the dashboard thread. */
void display_init(void);

/* --- dashboard ------------------------------------------------------ */

enum ui_row {
  UI_ROW_SYS,     /* power / pull-ups / host link */
  UI_ROW_SPI,
  UI_ROW_I2C,
  UI_ROW_AWG,
  UI_ROW_SCOPE,
  UI_ROW_LOGIC,
  UI_ROW_UART,
  UI_ROW_NOTE,    /* free text from the host (API DISPLAY_TEXT) */
  UI_ROW_COUNT,
};

#define UI_LINE_MAX 24   /* characters per row that fit the width */
#define UI_PLOT_W   124

/* Replaces one row's text (printf-style) and schedules a redraw. */
void ui_set(enum ui_row row, const char *fmt, ...);
/* Short text shown right-aligned in the title bar for ~1s (e.g. "SPI 4B"). */
void ui_activity(const char *fmt, ...);
/* Forces the dashboard to show even if a raw image was drawn (boot logo). */
void ui_show(void);
/* True once the dashboard has replaced the boot logo. */
bool ui_is_shown(void);

/* Shows a drawing of the J1 header (every pin's name and function) instead
 * of the dashboard for 'seconds', to help wiring a target. 0 goes back to
 * the dashboard at once, UI_PINOUT_HOLD keeps it until the next call. */
#define UI_PINOUT_HOLD 0xFFFF
void ui_pinout(uint16_t seconds);

/* Plot area: analog traces (12-bit samples, min/max per column; channel
 * 0 cyan, 1 yellow; 'stride'/'offset' index into interleaved buffers) or
 * a digital capture (bit i = DIOi, 8 lanes). Each call replaces the plot. */
void ui_plot_clear(void);
void ui_plot_analog(int channel, const uint16_t *samples, uint32_t stride, uint32_t offset,
                    uint32_t n);
void ui_plot_digital(const uint8_t *samples, uint32_t n);
