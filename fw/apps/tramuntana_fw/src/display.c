/*
 * Tramuntana - display. See display.h.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#include "display.h"
#include "images_gen.h"
#include "font_gen.h"

LOG_MODULE_REGISTER(display, LOG_LEVEL_INF);

#define UI_REFRESH_MS    100
#define UI_ACTIVITY_MS   1200
#define UI_TITLE_H       15
#define UI_ROW_H         12
#define UI_ROWS_Y        (UI_TITLE_H + 2)
#define UI_PLOT_Y        (UI_ROWS_Y + UI_ROW_COUNT * UI_ROW_H + 1)
#define UI_PLOT_H        (DISPLAY_H - UI_PLOT_Y - 1)
#define UI_PLOT_X        2
#define UI_MARGIN_X      3

/* Pinout screen: the 2x8 header drawn as seen on the board (J1 on the
 * right edge, pin 1 at the top, odd pins in the inner column), with each
 * pin's name and function next to its column. */
#define PO_ROWS_Y        15
#define PO_ROW_H         18
#define PO_LINE2_DY      9
#define PO_CELL_W        16
#define PO_BODY_X        48
#define PO_BODY_W        (2 * PO_CELL_W)
#define PO_LABEL_GAP     3
#define PO_HOLD_UNTIL    INT64_MAX

#define COLOR_PO_BODY    RGB565(45, 45, 45)
#define COLOR_PO_DIVIDER RGB565(90, 90, 90)
#define COLOR_PO_PIN1    RGB565(255, 230, 0)
#define COLOR_PO_POWER   RGB565(255, 60, 60)
#define COLOR_PO_GND     RGB565(150, 150, 150)
#define COLOR_PO_VREF    RGB565(255, 150, 0)
#define COLOR_PO_GPIO    RGB565(60, 255, 90)
#define COLOR_PO_SPI     RGB565(255, 110, 255)
#define COLOR_PO_I2C     RGB565(90, 160, 255)
#define COLOR_PO_UART    RGB565(170, 130, 255)

struct pinout_pin {
  const char *name;
  const char *func;
  uint16_t color;
};

/* Index = J1 pin - 1. Must match hw/tramuntana_lite_r1a (J1 nets) and the
 * pin table in host/python/tramuntana/pinout.py. */
static const struct pinout_pin pinout_pins[16] = {
  { "3V3", "TARGET", COLOR_PO_POWER },   { "GND", "", COLOR_PO_GND },
  { "AIN0", "SCOPE 0", COLOR_CH0 },      { "GND", "", COLOR_PO_GND },
  { "AIN1", "SCOPE 1", COLOR_CH0 },      { "GND", "", COLOR_PO_GND },
  { "AOUT0", "AWG", COLOR_CH1 },         { "VREF", "NO LOAD", COLOR_PO_VREF },
  { "DIO0", "UART RX", COLOR_PO_UART }, { "DIO1", "UART TX", COLOR_PO_UART },
  { "DIO2", "SPI CS", COLOR_PO_SPI },    { "DIO3", "MISO", COLOR_PO_SPI },
  { "DIO4", "MOSI", COLOR_PO_SPI },      { "DIO5", "SCK", COLOR_PO_SPI },
  { "DIO6", "I2C SCL", COLOR_PO_I2C },  { "DIO7", "I2C SDA", COLOR_PO_I2C },
};

static const struct device *const display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
static uint8_t framebuffer[DISPLAY_W * DISPLAY_H * 2];

/* --- primitives ------------------------------------------------------ */

void display_set_pixel(uint8_t *fb, int x, int y, uint16_t color)
{
  size_t idx;

  if (x < 0 || x >= DISPLAY_W || y < 0 || y >= DISPLAY_H) {
    return;
  }
  /* Panel sits upside down on the board: flip both axes. */
  idx = ((size_t)(DISPLAY_H - 1 - y) * DISPLAY_W + (size_t)(DISPLAY_W - 1 - x)) * 2;
  fb[idx] = (uint8_t)(color >> 8);
  fb[idx + 1] = (uint8_t)color;
}

void display_draw_hline(uint8_t *fb, int x0, int x1, int y, uint16_t color)
{
  int x;

  for (x = MIN(x0, x1); x <= MAX(x0, x1); x++) {
    display_set_pixel(fb, x, y, color);
  }
}

void display_draw_vline(uint8_t *fb, int x, int y0, int y1, uint16_t color)
{
  int y;

  for (y = MIN(y0, y1); y <= MAX(y0, y1); y++) {
    display_set_pixel(fb, x, y, color);
  }
}

void display_fill_rect(uint8_t *fb, int x, int y, int w, int h, uint16_t color)
{
  int yy;

  for (yy = y; yy < y + h; yy++) {
    display_draw_hline(fb, x, x + w - 1, yy, color);
  }
}

static int draw_glyph(uint8_t *fb, int x, int y, char c, uint16_t color,
                      const struct font_glyph *table)
{
  const struct font_glyph *g;
  int row, col;

  if ((unsigned char)c < FONT_FIRST_CHAR || (unsigned char)c > FONT_LAST_CHAR) {
    return 0;
  }
  g = &table[(unsigned char)c - FONT_FIRST_CHAR];
  for (row = 0; row < FONT_HEIGHT; row++) {
    for (col = 0; col < g->width; col++) {
      if (g->rows[row] & (1u << col)) {
        display_set_pixel(fb, x + col, y + row, color);
      }
    }
  }
  return g->width + 1;
}

int display_draw_char(uint8_t *fb, int x, int y, char c, uint16_t color)
{
  return draw_glyph(fb, x, y, c, color, font_glyphs);
}

int display_draw_string(uint8_t *fb, int x, int y, const char *str, uint16_t color)
{
  int x0 = x;

  for (; *str != '\0'; str++) {
    x += draw_glyph(fb, x, y, *str, color, font_glyphs);
  }
  return x - x0;
}

int display_draw_string_bold(uint8_t *fb, int x, int y, const char *str, uint16_t color)
{
  int x0 = x;

  for (; *str != '\0'; str++) {
    x += draw_glyph(fb, x, y, *str, color, font_glyphs_bold);
  }
  return x - x0;
}

int display_string_width(const char *str, bool bold)
{
  const struct font_glyph *table = bold ? font_glyphs_bold : font_glyphs;
  int w = 0;

  for (; *str != '\0'; str++) {
    if ((unsigned char)*str >= FONT_FIRST_CHAR && (unsigned char)*str <= FONT_LAST_CHAR) {
      w += table[(unsigned char)*str - FONT_FIRST_CHAR].width + 1;
    }
  }
  return w;
}

static int display_write_fb(const uint8_t *fb)
{
  struct display_buffer_descriptor desc = {
    .buf_size = DISPLAY_W * DISPLAY_H * 2,
    .width = DISPLAY_W,
    .height = DISPLAY_H,
    .pitch = DISPLAY_W,
  };

  return display_write(display_dev, 0, 0, &desc, fb);
}

/* --- dashboard state ---------------------------------------------------- */

enum plot_kind { PLOT_NONE, PLOT_ANALOG, PLOT_DIGITAL };

static struct {
  char rows[UI_ROW_COUNT][UI_LINE_MAX + 1];
  char activity[UI_LINE_MAX + 1];
  int64_t activity_until;
  enum plot_kind plot_kind;
  /* analog: per column, per channel: min/max as plot rows (0 = top) */
  uint8_t plot_a[2][UI_PLOT_W][2];
  bool plot_a_used[2];
  /* digital: per column, bits that were high and bits that were low */
  uint8_t plot_d_hi[UI_PLOT_W];
  uint8_t plot_d_lo[UI_PLOT_W];
  bool shown;       /* dashboard (vs boot logo) on screen */
  int64_t pinout_until;   /* uptime until which the pinout replaces the dashboard; 0 = off */
  bool dirty;
  struct k_spinlock lock;  /* guards the state above; only ever held briefly */
  struct k_sem kick;
} ui;

/* Rendering works from this snapshot so the SPI write of a frame never
 * holds anything an instrument might be waiting on. */
static struct {
  char rows[UI_ROW_COUNT][UI_LINE_MAX + 1];
  char activity[UI_LINE_MAX + 1];
  enum plot_kind plot_kind;
  uint8_t plot_a[2][UI_PLOT_W][2];
  bool plot_a_used[2];
  uint8_t plot_d_hi[UI_PLOT_W];
  uint8_t plot_d_lo[UI_PLOT_W];
  bool pinout;
} snap;

static const char *const ui_labels[UI_ROW_COUNT] = {
  [UI_ROW_SYS] = "SYS", [UI_ROW_SPI] = "SPI", [UI_ROW_I2C] = "I2C", [UI_ROW_AWG] = "AWG",
  [UI_ROW_SCOPE] = "SCP", [UI_ROW_LOGIC] = "LOG", [UI_ROW_UART] = "URT", [UI_ROW_NOTE] = "",
};

static void ui_mark_dirty(void)
{
  ui.dirty = true;
  k_sem_give(&ui.kick);
}

void ui_set(enum ui_row row, const char *fmt, ...)
{
  char text[UI_LINE_MAX + 1];
  va_list ap;
  k_spinlock_key_t key;

  if (row >= UI_ROW_COUNT) {
    return;
  }
  va_start(ap, fmt);
  vsnprintf(text, sizeof(text), fmt, ap);
  va_end(ap);
  key = k_spin_lock(&ui.lock);
  memcpy(ui.rows[row], text, sizeof(text));
  k_spin_unlock(&ui.lock, key);
  ui_mark_dirty();
}

void ui_activity(const char *fmt, ...)
{
  char text[UI_LINE_MAX + 1];
  va_list ap;
  k_spinlock_key_t key;

  va_start(ap, fmt);
  vsnprintf(text, sizeof(text), fmt, ap);
  va_end(ap);
  key = k_spin_lock(&ui.lock);
  memcpy(ui.activity, text, sizeof(text));
  ui.activity_until = k_uptime_get() + UI_ACTIVITY_MS;
  k_spin_unlock(&ui.lock, key);
  ui_mark_dirty();
}

void ui_show(void)
{
  ui.shown = true;
  ui_mark_dirty();
}

bool ui_is_shown(void)
{
  return ui.shown;
}

void ui_pinout(uint16_t seconds)
{
  k_spinlock_key_t key = k_spin_lock(&ui.lock);

  if (seconds == 0) {
    ui.pinout_until = 0;
  }
  else if (seconds == UI_PINOUT_HOLD) {
    ui.pinout_until = PO_HOLD_UNTIL;
  }
  else {
    ui.pinout_until = k_uptime_get() + (int64_t)seconds * 1000;
  }
  k_spin_unlock(&ui.lock, key);
  ui.shown = true;
  ui_mark_dirty();
}

/* Drops an expired pinout; true if the screen has to go back to the dashboard. */
static bool ui_pinout_expire(void)
{
  bool expired = false;
  k_spinlock_key_t key = k_spin_lock(&ui.lock);

  if (ui.pinout_until != 0 && k_uptime_get() >= ui.pinout_until) {
    ui.pinout_until = 0;
    expired = true;
  }
  k_spin_unlock(&ui.lock, key);
  return expired;
}

void ui_plot_clear(void)
{
  k_spinlock_key_t key = k_spin_lock(&ui.lock);

  ui.plot_kind = PLOT_NONE;
  ui.plot_a_used[0] = ui.plot_a_used[1] = false;
  k_spin_unlock(&ui.lock, key);
  ui_mark_dirty();
}

void ui_plot_analog(int channel, const uint16_t *samples, uint32_t stride, uint32_t offset,
                    uint32_t n)
{
  uint32_t x;

  static uint8_t cols[UI_PLOT_W][2];
  k_spinlock_key_t key;

  if (channel < 0 || channel > 1 || n == 0) {
    return;
  }
  for (x = 0; x < UI_PLOT_W; x++) {
    uint32_t start = x * n / UI_PLOT_W;
    uint32_t end = (x + 1) * n / UI_PLOT_W;
    uint16_t vmin, vmax;
    uint32_t j;

    if (end <= start) {
      end = start + 1;
    }
    if (end > n) {
      end = n;
    }
    vmin = vmax = samples[start * stride + offset];
    for (j = start + 1; j < end; j++) {
      uint16_t v = samples[j * stride + offset];

      vmin = MIN(vmin, v);
      vmax = MAX(vmax, v);
    }
    cols[x][0] = (uint8_t)((UI_PLOT_H - 1) - ((uint32_t)vmax * (UI_PLOT_H - 1)) / 4095);
    cols[x][1] = (uint8_t)((UI_PLOT_H - 1) - ((uint32_t)vmin * (UI_PLOT_H - 1)) / 4095);
  }
  key = k_spin_lock(&ui.lock);
  if (ui.plot_kind != PLOT_ANALOG) {
    ui.plot_a_used[0] = ui.plot_a_used[1] = false;
    ui.plot_kind = PLOT_ANALOG;
  }
  memcpy(ui.plot_a[channel], cols, sizeof(cols));
  ui.plot_a_used[channel] = true;
  k_spin_unlock(&ui.lock, key);
  ui_mark_dirty();
}

void ui_plot_digital(const uint8_t *samples, uint32_t n)
{
  uint32_t x;

  static uint8_t hi_cols[UI_PLOT_W], lo_cols[UI_PLOT_W];
  k_spinlock_key_t key;

  if (n == 0) {
    return;
  }
  for (x = 0; x < UI_PLOT_W; x++) {
    uint32_t start = x * n / UI_PLOT_W;
    uint32_t end = (x + 1) * n / UI_PLOT_W;
    uint8_t hi = 0, lo = 0;
    uint32_t j;

    if (end <= start) {
      end = start + 1;
    }
    if (end > n) {
      end = n;
    }
    for (j = start; j < end; j++) {
      hi |= samples[j];
      lo |= (uint8_t)~samples[j];
    }
    hi_cols[x] = hi;
    lo_cols[x] = lo;
  }
  key = k_spin_lock(&ui.lock);
  ui.plot_kind = PLOT_DIGITAL;
  memcpy(ui.plot_d_hi, hi_cols, sizeof(hi_cols));
  memcpy(ui.plot_d_lo, lo_cols, sizeof(lo_cols));
  k_spin_unlock(&ui.lock, key);
  ui_mark_dirty();
}

/* --- rendering ------------------------------------------------------------ */

static void ui_snapshot(void)
{
  k_spinlock_key_t key = k_spin_lock(&ui.lock);

  memcpy(snap.rows, ui.rows, sizeof(snap.rows));
  if (ui.activity[0] != '\0' && k_uptime_get() >= ui.activity_until) {
    ui.activity[0] = '\0';
  }
  memcpy(snap.activity, ui.activity, sizeof(snap.activity));
  snap.plot_kind = ui.plot_kind;
  memcpy(snap.plot_a, ui.plot_a, sizeof(snap.plot_a));
  memcpy(snap.plot_a_used, ui.plot_a_used, sizeof(snap.plot_a_used));
  memcpy(snap.plot_d_hi, ui.plot_d_hi, sizeof(snap.plot_d_hi));
  memcpy(snap.plot_d_lo, ui.plot_d_lo, sizeof(snap.plot_d_lo));
  snap.pinout = (ui.pinout_until != 0);
  ui.dirty = false;
  k_spin_unlock(&ui.lock, key);
}

static void ui_render_pinout(uint8_t *fb)
{
  int row, side;

  memset(fb, 0, sizeof(framebuffer));

  display_draw_string_bold(fb, UI_MARGIN_X, 1, "J1 PINOUT", COLOR_TEXT);
  display_draw_hline(fb, 0, DISPLAY_W - 1, PO_ROWS_Y - 2, COLOR_DIM);
  display_fill_rect(fb, PO_BODY_X, PO_ROWS_Y, PO_BODY_W, DISPLAY_H - PO_ROWS_Y, COLOR_PO_BODY);
  display_draw_vline(fb, PO_BODY_X + PO_CELL_W, PO_ROWS_Y, DISPLAY_H - 1, COLOR_PO_DIVIDER);

  for (row = 0; row < 8; row++) {
    int y = PO_ROWS_Y + row * PO_ROW_H;

    /* side 0: odd pin, inner column, label on the left; side 1: even pin. */
    for (side = 0; side < 2; side++) {
      int pin = 2 * row + side + 1;
      const struct pinout_pin *p = &pinout_pins[pin - 1];
      int cell_x = PO_BODY_X + side * PO_CELL_W;
      bool two_lines = (p->func[0] != '\0');
      int name_y = two_lines ? y : y + PO_LINE2_DY / 2;
      char num[3];
      int x;

      snprintf(num, sizeof(num), "%d", pin);
      x = cell_x + (PO_CELL_W - display_string_width(num, false)) / 2;
      if (pin == 1) {
        /* Square pad, as on the silkscreen. */
        display_fill_rect(fb, cell_x + 1, y + 1, PO_CELL_W - 2, PO_ROW_H - 3, COLOR_PO_PIN1);
        display_draw_string(fb, x, y + 3, num, COLOR_BG);
      }
      else {
        display_draw_string(fb, x, y + 3, num, COLOR_TEXT);
      }

      if (side == 0) {
        int right = PO_BODY_X - PO_LABEL_GAP;

        display_draw_string_bold(fb, right - display_string_width(p->name, true), name_y, p->name,
                                 p->color);
        if (two_lines) {
          display_draw_string(fb, right - display_string_width(p->func, false), y + PO_LINE2_DY,
                              p->func, COLOR_LABEL);
        }
      }
      else {
        int left = PO_BODY_X + PO_BODY_W + PO_LABEL_GAP;

        display_draw_string_bold(fb, left, name_y, p->name, p->color);
        if (two_lines) {
          display_draw_string(fb, left, y + PO_LINE2_DY, p->func, COLOR_LABEL);
        }
      }
    }
  }
}

static void ui_render(uint8_t *fb)
{
  int y, i;

  memset(fb, 0, sizeof(framebuffer));

  /* Title bar. */
  display_draw_string_bold(fb, UI_MARGIN_X, 2, "TRAMUNTANA", COLOR_TEXT);
  if (snap.activity[0] != '\0') {
    int w = display_string_width(snap.activity, false);

    display_draw_string(fb, DISPLAY_W - UI_MARGIN_X - w, 2, snap.activity, COLOR_CH1);
  }
  display_draw_hline(fb, 0, DISPLAY_W - 1, UI_TITLE_H - 1, COLOR_DIM);

  /* Rows. */
  y = UI_ROWS_Y;
  for (i = 0; i < UI_ROW_COUNT; i++) {
    int x = UI_MARGIN_X;

    if (ui_labels[i][0] != '\0') {
      display_draw_string_bold(fb, x, y, ui_labels[i], COLOR_LABEL);
      x += 25;
    }
    display_draw_string(fb, x, y, snap.rows[i], (i == UI_ROW_NOTE) ? COLOR_CH1 : COLOR_TEXT);
    y += UI_ROW_H;
  }

  /* Plot area. */
  display_draw_hline(fb, 0, DISPLAY_W - 1, UI_PLOT_Y - 1, COLOR_DIM);
  if (snap.plot_kind == PLOT_ANALOG) {
    int ch, x;

    for (i = 1; i < 4; i++) {
      display_draw_vline(fb, UI_PLOT_X + i * UI_PLOT_W / 4, UI_PLOT_Y, UI_PLOT_Y + UI_PLOT_H - 1,
                         RGB565(40, 40, 40));
    }
    display_draw_hline(fb, UI_PLOT_X, UI_PLOT_X + UI_PLOT_W - 1, UI_PLOT_Y + UI_PLOT_H / 2,
                       RGB565(40, 40, 40));
    for (ch = 0; ch < 2; ch++) {
      if (!snap.plot_a_used[ch]) {
        continue;
      }
      for (x = 0; x < UI_PLOT_W; x++) {
        display_draw_vline(fb, UI_PLOT_X + x, UI_PLOT_Y + snap.plot_a[ch][x][0],
                           UI_PLOT_Y + snap.plot_a[ch][x][1], ch ? COLOR_CH1 : COLOR_CH0);
      }
    }
  } else if (snap.plot_kind == PLOT_DIGITAL) {
    int lane_h = UI_PLOT_H / 8;
    int ch, x;

    for (ch = 0; ch < 8; ch++) {
      int y_hi = UI_PLOT_Y + ch * lane_h;
      int y_lo = y_hi + lane_h - 2;
      uint16_t color = (ch & 1) ? COLOR_CH1 : COLOR_CH0;

      for (x = 0; x < UI_PLOT_W; x++) {
        bool hi = (snap.plot_d_hi[x] >> ch) & 1;
        bool lo = (snap.plot_d_lo[x] >> ch) & 1;

        if (hi && lo) {
          display_draw_vline(fb, UI_PLOT_X + x, y_hi, y_lo, color);
        } else if (hi) {
          display_set_pixel(fb, UI_PLOT_X + x, y_hi, color);
        } else {
          display_set_pixel(fb, UI_PLOT_X + x, y_lo, color);
        }
      }
    }
  }
}

static void ui_thread(void *a, void *b, void *c)
{
  ARG_UNUSED(a);
  ARG_UNUSED(b);
  ARG_UNUSED(c);

  bool pinout_on_screen = false;

  for (;;) {
    k_sem_take(&ui.kick, K_MSEC(UI_ACTIVITY_MS));
    if (!ui.shown) {
      continue;
    }
    if (ui_pinout_expire()) {
      ui.dirty = true;
    }
    if (!ui.dirty && (ui.activity[0] == '\0' || k_uptime_get() < ui.activity_until)) {
      continue;
    }
    ui_snapshot();
    if (snap.pinout) {
      /* The drawing is static: instrument updates only need a frame once
       * the dashboard is back. */
      if (!pinout_on_screen) {
        ui_render_pinout(framebuffer);
        display_write_fb(framebuffer);
        pinout_on_screen = true;
      }
      continue;
    }
    pinout_on_screen = false;
    ui_render(framebuffer);
    display_write_fb(framebuffer);
    /* Coalesce bursts of updates into one frame per UI_REFRESH_MS. */
    k_sleep(K_MSEC(UI_REFRESH_MS));
  }
}

K_THREAD_STACK_DEFINE(ui_stack, 2048);
static struct k_thread ui_thread_data;

void display_init(void)
{
  const struct tramuntana_image *logo = NULL;
  size_t i;

  k_sem_init(&ui.kick, 0, 1);
  ui.shown = false;

  if (!device_is_ready(display_dev)) {
    LOG_ERR("display not ready");
    return;
  }
  display_blanking_off(display_dev);

  for (i = 0; i < TRAMUNTANA_IMAGE_COUNT; i++) {
    if (strcmp(tramuntana_images[i].name, "logo") == 0) {
      logo = &tramuntana_images[i];
    }
  }
  if (logo != NULL) {
    struct display_buffer_descriptor desc = {
      .buf_size = logo->pixels_len,
      .width = logo->width,
      .height = logo->height,
      .pitch = logo->width,
    };

    display_write(display_dev, 0, 0, &desc, logo->pixels);
  }

  k_thread_create(&ui_thread_data, ui_stack, K_THREAD_STACK_SIZEOF(ui_stack), ui_thread, NULL,
                  NULL, NULL, K_PRIO_PREEMPT(12), 0, K_NO_WAIT);
  k_thread_name_set(&ui_thread_data, "ui");
}
