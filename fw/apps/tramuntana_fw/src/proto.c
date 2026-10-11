/*
 * Tramuntana - proto. See proto.h for the wire format and command set.
 *
 * One thread owns the API port: it pulls bytes, resynchronises on the
 * 0xAA 0x55 start-of-frame, checks the CRC and hands each request to the
 * handler table below. Replies are written back under the TX mutex, and
 * so are events, which the sniffer threads push through
 * proto_send_event() - so a reply and an event never interleave on the
 * wire even though they come from different threads.
 *
 * Handlers get the request payload and a scratch reply buffer; they
 * return the reply status and fill *rlen with how much they wrote after
 * the status byte. Anything that can block for long (captures) is
 * arm/status/read style, so the port never stalls.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/crc.h>
#include <zephyr/logging/log.h>
#include <pico/bootrom.h>
#include <string.h>
#include <stdio.h>

#include "proto.h"
#include "port.h"
#include "usb.h"
#include "board.h"
#include "display.h"
#include "spi.h"
#include "i2c.h"
#include "scope.h"
#include "awg.h"
#include "pwm.h"
#include "logic.h"
#include "bridge.h"
#include "watch.h"
#include "emu.h"
#include "seq.h"
#include "version.h"

LOG_MODULE_REGISTER(proto, LOG_LEVEL_INF);

#define PROTO_HDR_LEN   6
#define PROTO_CRC_LEN   2
#define PROTO_FRAME_MAX (PROTO_HDR_LEN + PROTO_MAX_PAYLOAD + PROTO_CRC_LEN)

static struct port api_port;
static uint8_t api_rx_ring[4096];
static uint8_t api_tx_ring[8192];
static uint8_t rx_frame[PROTO_FRAME_MAX];
static uint8_t tx_frame[PROTO_FRAME_MAX];
static K_MUTEX_DEFINE(tx_lock);

/* Host link indicator on the dashboard: "API" while requests keep
 * coming, blank after a while without any. */
static int64_t last_request_at;
static volatile bool link_shown;

/* The dashboard's SYS line: target power, pull-ups, host link. */
static void ui_sys_refresh(void)
{
  ui_set(UI_ROW_SYS, "PWR %s PU %s%s", board_power_get() ? "ON" : "off",
         board_pullups_get() ? "ON" : "off", link_shown ? "  API" : "");
}

/* Power or pull-ups changed (API, shell, sequencer, sniffer, emulator). */
static void on_board_change(void)
{
  ui_sys_refresh();
  i2c_ui_refresh();
}

static uint16_t proto_crc(const uint8_t *data, size_t len)
{
  return crc16_itu_t(0xFFFF, data, len);
}

/* Builds and sends one frame. Blocks up to 'timeout' for TX ring space. */
static void proto_send_frame(uint8_t cmd, uint8_t seq, const uint8_t *payload, size_t len,
                             k_timeout_t timeout)
{
  size_t total;
  uint16_t crc;

  if (len > PROTO_MAX_PAYLOAD) {
    len = PROTO_MAX_PAYLOAD;
  }
  k_mutex_lock(&tx_lock, K_FOREVER);
  tx_frame[0] = PROTO_SOF0;
  tx_frame[1] = PROTO_SOF1;
  tx_frame[2] = cmd;
  tx_frame[3] = seq;
  wr16(&tx_frame[4], (uint16_t)len);
  if (len > 0 && payload != tx_frame + PROTO_HDR_LEN) {
    memmove(&tx_frame[PROTO_HDR_LEN], payload, len);
  }
  crc = proto_crc(&tx_frame[2], PROTO_HDR_LEN - 2 + len);
  wr16(&tx_frame[PROTO_HDR_LEN + len], crc);
  total = PROTO_HDR_LEN + len + PROTO_CRC_LEN;
  port_write(&api_port, tx_frame, total, timeout);
  k_mutex_unlock(&tx_lock);
}

static proto_event_tap_t event_tap;

void proto_set_event_tap(proto_event_tap_t tap)
{
  event_tap = tap;
}

void proto_send_event(uint8_t evt, const uint8_t *payload, size_t len)
{
  proto_event_tap_t tap = event_tap;

  if (tap != NULL) {
    tap(evt, payload, len);
  }
  /* Events are best effort: if nobody drains the API port, drop rather
   * than hang the sniffer thread. */
  proto_send_frame(evt, 0, payload, len, K_MSEC(20));
}

/* --- handlers ------------------------------------------------------------ */

/* Handler signature: request payload in/out, reply data written to
 * 'r' (after the status byte), returns a proto_status. */
typedef enum proto_status (*proto_handler_t)(const uint8_t *p, size_t len, uint8_t *r,
                                             size_t *rlen);

enum proto_status proto_errno_to_status(int err)
{
  switch (err) {
  case 0:
    return PROTO_OK;
  case -EINVAL:
    return PROTO_EINVAL;
  case -EBUSY:
    return PROTO_EBUSY;
  case -ETIMEDOUT:
    return PROTO_ETIMEDOUT;
  case -EIO:
    return PROTO_EIO;
  case -ENOTSUP:
    return PROTO_ENOTSUP;
  case -EPERM:
    return PROTO_EPERM;
  case -ENOMEM:
    return PROTO_ENOMEM;
  default:
    return PROTO_EIO;
  }
}

#define errno_to_status proto_errno_to_status

/* --- system --- */

static enum proto_status h_ping(const uint8_t *p, size_t len, uint8_t *r, size_t *rlen)
{
  ARG_UNUSED(p);
  ARG_UNUSED(len);
  memcpy(r, PROTO_MAGIC, 4);
  r[4] = PROTO_VERSION;
  *rlen = 5;
  return PROTO_OK;
}

static enum proto_status h_info(const uint8_t *p, size_t len, uint8_t *r, size_t *rlen)
{
  ARG_UNUSED(p);
  ARG_UNUSED(len);
  memset(r, 0, 16 + 16 + 4 + 8 + 4);
  memcpy(r, FW_VERSION, MIN(strlen(FW_VERSION), 15));
  memcpy(r + 16, "tramuntana_lite", 15);
  wr32(r + 32, board_sysclk_hz());
  board_chip_id(r + 36);
  /* features: spi, i2c, scope, awg, logic, uart, pwm, dio watch + time,
   * uart control lines, emulated targets, sequencer */
  wr32(r + 44, 0x7FF);
  *rlen = 48;
  return PROTO_OK;
}

static enum proto_status h_bootloader(const uint8_t *p, size_t len, uint8_t *r, size_t *rlen)
{
  ARG_UNUSED(p);
  ARG_UNUSED(len);
  ARG_UNUSED(r);
  *rlen = 0;
  /* The reply goes out first; the reset is scheduled just after. */
  return PROTO_OK;
}

static enum proto_status h_led(const uint8_t *p, size_t len, uint8_t *r, size_t *rlen)
{
  ARG_UNUSED(r);
  *rlen = 0;
  if (len < 3) {
    return PROTO_EINVAL;
  }
  if (p[0] == 0xFF) {
    led_idle();
    return PROTO_OK;
  }
  {
    int idx = (p[0] ? 1 : 0) | (p[1] ? 2 : 0) | (p[2] ? 4 : 0);
    static const enum led_color map[8] = {
      LED_OFF, LED_RED, LED_GREEN, LED_YELLOW, LED_BLUE, LED_MAGENTA, LED_CYAN, LED_WHITE,
    };

    led_set(map[idx]);
  }
  return PROTO_OK;
}

static enum proto_status h_power(const uint8_t *p, size_t len, uint8_t *r, size_t *rlen)
{
  if (len >= 1) {
    board_power_set(p[0] != 0);
    ui_activity("PWR %s", p[0] ? "ON" : "OFF");
  }
  r[0] = board_power_get();
  *rlen = 1;
  return PROTO_OK;
}

static enum proto_status h_pullups(const uint8_t *p, size_t len, uint8_t *r, size_t *rlen)
{
  if (len >= 1) {
    board_pullups_set(p[0] != 0);
  }
  r[0] = board_pullups_get();
  *rlen = 1;
  return PROTO_OK;
}

static enum proto_status h_dio_dir(const uint8_t *p, size_t len, uint8_t *r, size_t *rlen)
{
  ARG_UNUSED(r);
  *rlen = 0;
  if (len < 2) {
    return PROTO_EINVAL;
  }
  return errno_to_status(board_dio_dir(p[0], p[1]));
}

static enum proto_status h_dio_write(const uint8_t *p, size_t len, uint8_t *r, size_t *rlen)
{
  ARG_UNUSED(r);
  *rlen = 0;
  if (len < 2) {
    return PROTO_EINVAL;
  }
  return errno_to_status(board_dio_write(p[0], p[1]));
}

static enum proto_status h_dio_read(const uint8_t *p, size_t len, uint8_t *r, size_t *rlen)
{
  ARG_UNUSED(p);
  ARG_UNUSED(len);
  r[0] = board_dio_read();
  *rlen = 1;
  return PROTO_OK;
}

static enum proto_status h_status(const uint8_t *p, size_t len, uint8_t *r, size_t *rlen)
{
  bool bridge_active;
  uint32_t baud, rx, tx;
  struct awg_status awg;

  ARG_UNUSED(p);
  ARG_UNUSED(len);
  bridge_status(&bridge_active, &baud, &rx, &tx);
  awg_get_status(&awg);
  wr32(r, board_pins_snapshot());
  r[4] = board_power_get();
  r[5] = board_pullups_get();
  r[6] = (uint8_t)scope_state();
  r[7] = (uint8_t)logic_state();
  r[8] = awg.running;
  r[9] = spi_sniff_active();
  r[10] = i2c_sniff_active();
  r[11] = bridge_active;
  wr32(r + 12, api_port.rx_dropped);
  *rlen = 16;
  return PROTO_OK;
}

static enum proto_status h_display_text(const uint8_t *p, size_t len, uint8_t *r, size_t *rlen)
{
  char text[UI_LINE_MAX + 1];

  ARG_UNUSED(r);
  *rlen = 0;
  len = MIN(len, (size_t)UI_LINE_MAX);
  memcpy(text, p, len);
  text[len] = '\0';
  ui_set(UI_ROW_NOTE, "%s", text);
  return PROTO_OK;
}

static enum proto_status h_display_pinout(const uint8_t *p, size_t len, uint8_t *r, size_t *rlen)
{
  ARG_UNUSED(r);
  *rlen = 0;
  if (len < 2) {
    return PROTO_EINVAL;
  }
  ui_pinout(rd16(p));
  return PROTO_OK;
}

static enum proto_status h_adc_read(const uint8_t *p, size_t len, uint8_t *r, size_t *rlen)
{
  int mv;

  *rlen = 0;
  if (len < 1) {
    return PROTO_EINVAL;
  }
  mv = board_adc_read_mv(p[0]);
  if (mv < 0) {
    return errno_to_status(mv);
  }
  wr16(r, (uint16_t)mv);
  *rlen = 2;
  return PROTO_OK;
}

volatile bool proto_event_stamps;

static enum proto_status h_time(const uint8_t *p, size_t len, uint8_t *r, size_t *rlen)
{
  if (len >= 1) {
    proto_event_stamps = p[0] != 0;
  }
  wr32(r, board_time_us());
  *rlen = 4;
  return PROTO_OK;
}

static enum proto_status h_dio_watch(const uint8_t *p, size_t len, uint8_t *r, size_t *rlen)
{
  ARG_UNUSED(r);
  *rlen = 0;
  if (len < 1) {
    return PROTO_EINVAL;
  }
  return errno_to_status(watch_set(p[0]));
}

/* --- spi --- */

static enum proto_status h_spi_conf(const uint8_t *p, size_t len, uint8_t *r, size_t *rlen)
{
  uint32_t actual = 0;
  int ret;

  *rlen = 0;
  if (len < 6) {
    return PROTO_EINVAL;
  }
  ret = spi_drv_conf(rd32(p), p[4], p[5] != 0, &actual);
  if (ret != 0) {
    return errno_to_status(ret);
  }
  wr32(r, actual);
  *rlen = 4;
  return PROTO_OK;
}

static enum proto_status h_spi_sel(const uint8_t *p, size_t len, uint8_t *r, size_t *rlen)
{
  ARG_UNUSED(p);
  ARG_UNUSED(len);
  ARG_UNUSED(r);
  *rlen = 0;
  return errno_to_status(spi_drv_sel());
}

static enum proto_status h_spi_unsel(const uint8_t *p, size_t len, uint8_t *r, size_t *rlen)
{
  ARG_UNUSED(p);
  ARG_UNUSED(len);
  ARG_UNUSED(r);
  *rlen = 0;
  return errno_to_status(spi_drv_unsel());
}

static enum proto_status h_spi_write(const uint8_t *p, size_t len, uint8_t *r, size_t *rlen)
{
  ARG_UNUSED(r);
  *rlen = 0;
  if (len == 0 || len > SPI_MAX_XFER) {
    return PROTO_EINVAL;
  }
  return errno_to_status(spi_drv_xfer(p, NULL, len, 0));
}

static enum proto_status h_spi_read(const uint8_t *p, size_t len, uint8_t *r, size_t *rlen)
{
  uint16_t n;
  int ret;

  *rlen = 0;
  if (len < 3) {
    return PROTO_EINVAL;
  }
  n = rd16(p);
  if (n == 0 || n > SPI_MAX_XFER) {
    return PROTO_EINVAL;
  }
  ret = spi_drv_xfer(NULL, r, n, p[2]);
  if (ret != 0) {
    return errno_to_status(ret);
  }
  *rlen = n;
  return PROTO_OK;
}

static enum proto_status h_spi_xfer(const uint8_t *p, size_t len, uint8_t *r, size_t *rlen)
{
  int ret;

  *rlen = 0;
  if (len == 0 || len > SPI_MAX_XFER) {
    return PROTO_EINVAL;
  }
  ret = spi_drv_xfer(p, r, len, 0);
  if (ret != 0) {
    return errno_to_status(ret);
  }
  *rlen = len;
  return PROTO_OK;
}

static enum proto_status h_spi_txn(const uint8_t *p, size_t len, uint8_t *r, size_t *rlen)
{
  uint16_t rn;
  int ret;

  *rlen = 0;
  if (len < 2) {
    return PROTO_EINVAL;
  }
  rn = rd16(p);
  if (rn > SPI_MAX_XFER || len - 2 > SPI_MAX_XFER) {
    return PROTO_EINVAL;
  }
  ret = spi_drv_txn(p + 2, len - 2, r, rn);
  if (ret != 0) {
    return errno_to_status(ret);
  }
  *rlen = rn;
  return PROTO_OK;
}

static enum proto_status h_spi_sniff(const uint8_t *p, size_t len, uint8_t *r, size_t *rlen)
{
  ARG_UNUSED(r);
  *rlen = 0;
  if (len < 1) {
    return PROTO_EINVAL;
  }
  if (p[0]) {
    return errno_to_status(spi_sniff_start(len >= 2 ? p[1] : spi_drv_mode()));
  }
  return errno_to_status(spi_sniff_stop());
}

/* --- i2c --- */

static enum proto_status h_i2c_conf(const uint8_t *p, size_t len, uint8_t *r, size_t *rlen)
{
  uint32_t actual = 0;
  int ret;

  *rlen = 0;
  if (len < 5) {
    return PROTO_EINVAL;
  }
  ret = i2c_drv_conf(rd32(p), p[4] != 0, &actual);
  if (ret != 0) {
    return errno_to_status(ret);
  }
  wr32(r, actual);
  *rlen = 4;
  return PROTO_OK;
}

static enum proto_status h_i2c_scan(const uint8_t *p, size_t len, uint8_t *r, size_t *rlen)
{
  int ret;

  ARG_UNUSED(p);
  ARG_UNUSED(len);
  *rlen = 0;
  ret = i2c_drv_scan(r);
  if (ret != 0) {
    return errno_to_status(ret);
  }
  *rlen = 16;
  return PROTO_OK;
}

static enum proto_status h_i2c_write(const uint8_t *p, size_t len, uint8_t *r, size_t *rlen)
{
  size_t acked = 0;
  int ret;

  *rlen = 0;
  if (len < 1 || len - 1 > I2C_MAX_XFER) {
    return PROTO_EINVAL;
  }
  ret = i2c_drv_write(p[0], p + 1, len - 1, &acked);
  wr16(r, (uint16_t)acked);
  *rlen = 2;
  return errno_to_status(ret);
}

static enum proto_status h_i2c_read(const uint8_t *p, size_t len, uint8_t *r, size_t *rlen)
{
  uint16_t n;
  int ret;

  *rlen = 0;
  if (len < 3) {
    return PROTO_EINVAL;
  }
  n = rd16(p + 1);
  if (n == 0 || n > I2C_MAX_XFER) {
    return PROTO_EINVAL;
  }
  ret = i2c_drv_read(p[0], r, n);
  if (ret != 0) {
    return errno_to_status(ret);
  }
  *rlen = n;
  return PROTO_OK;
}

static enum proto_status h_i2c_write_read(const uint8_t *p, size_t len, uint8_t *r, size_t *rlen)
{
  uint8_t wn;
  uint16_t rn;
  int ret;

  *rlen = 0;
  if (len < 2) {
    return PROTO_EINVAL;
  }
  wn = p[1];
  if (len < (size_t)2 + wn + 2) {
    return PROTO_EINVAL;
  }
  rn = rd16(p + 2 + wn);
  if (rn == 0 || rn > I2C_MAX_XFER) {
    return PROTO_EINVAL;
  }
  ret = i2c_drv_write_read(p[0], p + 2, wn, r, rn);
  if (ret != 0) {
    return errno_to_status(ret);
  }
  *rlen = rn;
  return PROTO_OK;
}

static enum proto_status h_i2c_start(const uint8_t *p, size_t len, uint8_t *r, size_t *rlen)
{
  bool ack = false;
  int ret;

  *rlen = 0;
  if (len < 1) {
    return PROTO_EINVAL;
  }
  ret = i2c_drv_start(p[0], &ack);
  if (ret != 0) {
    return errno_to_status(ret);
  }
  r[0] = ack;
  *rlen = 1;
  return PROTO_OK;
}

static enum proto_status h_i2c_stop(const uint8_t *p, size_t len, uint8_t *r, size_t *rlen)
{
  ARG_UNUSED(p);
  ARG_UNUSED(len);
  ARG_UNUSED(r);
  *rlen = 0;
  return errno_to_status(i2c_drv_stop());
}

static enum proto_status h_i2c_raw_write(const uint8_t *p, size_t len, uint8_t *r, size_t *rlen)
{
  size_t acked = 0;
  int ret;

  *rlen = 0;
  if (len == 0 || len > I2C_MAX_XFER) {
    return PROTO_EINVAL;
  }
  ret = i2c_drv_raw_write(p, len, &acked);
  wr16(r, (uint16_t)acked);
  *rlen = 2;
  return errno_to_status(ret);
}

static enum proto_status h_i2c_raw_read(const uint8_t *p, size_t len, uint8_t *r, size_t *rlen)
{
  uint16_t n;
  int ret;

  *rlen = 0;
  if (len < 3) {
    return PROTO_EINVAL;
  }
  n = rd16(p);
  if (n == 0 || n > I2C_MAX_XFER) {
    return PROTO_EINVAL;
  }
  ret = i2c_drv_raw_read(r, n, p[2] != 0);
  if (ret != 0) {
    return errno_to_status(ret);
  }
  *rlen = n;
  return PROTO_OK;
}

static enum proto_status h_i2c_sniff(const uint8_t *p, size_t len, uint8_t *r, size_t *rlen)
{
  ARG_UNUSED(r);
  *rlen = 0;
  if (len < 1) {
    return PROTO_EINVAL;
  }
  if (p[0]) {
    return errno_to_status(i2c_sniff_start(len >= 2 && p[1] != 0));
  }
  return errno_to_status(i2c_sniff_stop());
}

static enum proto_status h_i2c_reset(const uint8_t *p, size_t len, uint8_t *r, size_t *rlen)
{
  ARG_UNUSED(p);
  ARG_UNUSED(len);
  ARG_UNUSED(r);
  *rlen = 0;
  return errno_to_status(i2c_drv_reset());
}

/* --- scope --- */

static enum proto_status h_scope_arm(const uint8_t *p, size_t len, uint8_t *r, size_t *rlen)
{
  struct scope_params sp;
  uint32_t rate = 0;
  int ret;

  *rlen = 0;
  if (len < 12) {
    return PROTO_EINVAL;
  }
  sp.channels = p[0];
  sp.n = rd16(p + 1);
  sp.clkdiv = rd16(p + 3);
  sp.trig_mode = p[5];
  sp.trig_level = rd16(p + 6);
  sp.pre = rd16(p + 8);
  sp.timeout_ms = rd16(p + 10);
  ret = scope_arm(&sp, &rate);
  if (ret != 0) {
    return errno_to_status(ret);
  }
  wr32(r, rate);
  *rlen = 4;
  return PROTO_OK;
}

static enum proto_status h_scope_status(const uint8_t *p, size_t len, uint8_t *r, size_t *rlen)
{
  uint8_t ch;
  uint16_t n, ti;
  uint32_t rate;

  ARG_UNUSED(p);
  ARG_UNUSED(len);
  scope_status(&ch, &n, &rate, &ti);
  r[0] = (uint8_t)scope_state();
  r[1] = ch;
  wr16(r + 2, n);
  wr32(r + 4, rate);
  wr16(r + 8, ti);
  *rlen = 10;
  return PROTO_OK;
}

static enum proto_status h_scope_read(const uint8_t *p, size_t len, uint8_t *r, size_t *rlen)
{
  uint16_t offset, count;
  int ret;

  *rlen = 0;
  if (len < 4) {
    return PROTO_EINVAL;
  }
  offset = rd16(p);
  count = rd16(p + 2);
  if (count == 0 || count > 2048) {
    return PROTO_EINVAL;
  }
  ret = scope_read(offset, count, r);
  if (ret < 0) {
    return errno_to_status(ret);
  }
  *rlen = (size_t)ret * 2;
  return PROTO_OK;
}

static enum proto_status h_scope_abort(const uint8_t *p, size_t len, uint8_t *r, size_t *rlen)
{
  ARG_UNUSED(p);
  ARG_UNUSED(len);
  ARG_UNUSED(r);
  *rlen = 0;
  return errno_to_status(scope_abort());
}

/* --- awg --- */

static enum proto_status h_awg_set(const uint8_t *p, size_t len, uint8_t *r, size_t *rlen)
{
  struct awg_status st;
  int ret;

  *rlen = 0;
  if (len < 11) {
    return PROTO_EINVAL;
  }
  ret = awg_set((enum awg_shape)p[0], rd32(p + 1), rd16(p + 5), rd16(p + 7), rd16(p + 9), &st);
  if (ret != 0) {
    return errno_to_status(ret);
  }
  wr32(r, st.freq_mhz);
  wr16(r + 4, st.n);
  wr16(r + 6, st.top);
  *rlen = 8;
  return PROTO_OK;
}

static enum proto_status h_awg_off(const uint8_t *p, size_t len, uint8_t *r, size_t *rlen)
{
  ARG_UNUSED(p);
  ARG_UNUSED(len);
  ARG_UNUSED(r);
  *rlen = 0;
  awg_off();
  return PROTO_OK;
}

static enum proto_status h_awg_status(const uint8_t *p, size_t len, uint8_t *r, size_t *rlen)
{
  struct awg_status st;

  ARG_UNUSED(p);
  ARG_UNUSED(len);
  awg_get_status(&st);
  r[0] = st.running;
  r[1] = (uint8_t)st.shape;
  wr32(r + 2, st.freq_mhz);
  wr16(r + 6, st.amp_mv);
  wr16(r + 8, st.off_mv);
  *rlen = 10;
  return PROTO_OK;
}

static enum proto_status h_awg_arb_load(const uint8_t *p, size_t len, uint8_t *r, size_t *rlen)
{
  ARG_UNUSED(r);
  *rlen = 0;
  if (len < 3) {
    return PROTO_EINVAL;
  }
  return errno_to_status(awg_arb_load(rd16(p), p + 2, len - 2));
}

static enum proto_status h_awg_dc(const uint8_t *p, size_t len, uint8_t *r, size_t *rlen)
{
  ARG_UNUSED(r);
  *rlen = 0;
  if (len < 2) {
    return PROTO_EINVAL;
  }
  return errno_to_status(awg_dc(rd16(p)));
}

static enum proto_status h_awg_step(const uint8_t *p, size_t len, uint8_t *r, size_t *rlen)
{
  ARG_UNUSED(r);
  *rlen = 0;
  if (len < 6) {
    return PROTO_EINVAL;
  }
  return errno_to_status(awg_step(rd16(p), rd16(p + 2), rd16(p + 4)));
}

/* --- pwm --- */

static enum proto_status h_pwm_set(const uint8_t *p, size_t len, uint8_t *r, size_t *rlen)
{
  struct pwm_out_status st;
  int ret;

  *rlen = 0;
  if (len < 8) {
    return PROTO_EINVAL;
  }
  ret = pwm_out_set(p[0], rd32(p + 1), rd16(p + 5), p[7] != 0, &st);
  if (ret != 0) {
    return errno_to_status(ret);
  }
  wr32(r, st.freq_mhz);
  wr16(r + 4, st.level);
  wr16(r + 6, st.top);
  r[8] = st.div;
  *rlen = 9;
  return PROTO_OK;
}

static enum proto_status h_pwm_phase(const uint8_t *p, size_t len, uint8_t *r, size_t *rlen)
{
  ARG_UNUSED(r);
  *rlen = 0;
  if (len < 3) {
    return PROTO_EINVAL;
  }
  return errno_to_status(pwm_out_phase(p[0], rd16(p + 1)));
}

static enum proto_status h_pwm_sync(const uint8_t *p, size_t len, uint8_t *r, size_t *rlen)
{
  ARG_UNUSED(r);
  *rlen = 0;
  return errno_to_status(pwm_out_sync(len >= 1 ? p[0] : 0));
}

static enum proto_status h_pwm_stop(const uint8_t *p, size_t len, uint8_t *r, size_t *rlen)
{
  ARG_UNUSED(r);
  *rlen = 0;
  if (len < 1) {
    return PROTO_EINVAL;
  }
  pwm_out_stop(p[0]);
  return PROTO_OK;
}

static enum proto_status h_pwm_status(const uint8_t *p, size_t len, uint8_t *r, size_t *rlen)
{
  struct pwm_out_status st;
  int dio;

  ARG_UNUSED(p);
  ARG_UNUSED(len);
  for (dio = 0; dio < DIO_COUNT; dio++, r += 13) {
    pwm_out_get_status(dio, &st);
    r[0] = (st.active ? BIT(0) : 0) | (st.lost ? BIT(1) : 0) | (st.invert ? BIT(2) : 0);
    wr32(r + 1, st.freq_hz);
    wr32(r + 5, st.freq_mhz);
    wr16(r + 9, st.duty);
    wr16(r + 11, st.phase_deg);
  }
  *rlen = DIO_COUNT * 13;
  return PROTO_OK;
}

/* --- logic --- */

static enum proto_status h_logic_arm(const uint8_t *p, size_t len, uint8_t *r, size_t *rlen)
{
  struct logic_params lp;
  uint32_t rate = 0;
  int ret;

  *rlen = 0;
  if (len < 16) {
    return PROTO_EINVAL;
  }
  lp.n = rd32(p);
  lp.clkdiv = rd16(p + 4);
  lp.trig_mode = p[6];
  lp.trig_pin = p[7];
  lp.trig_mask = p[8];
  lp.trig_value = p[9];
  lp.pre = rd32(p + 10);
  lp.timeout_ms = rd16(p + 14);
  ret = logic_arm(&lp, &rate);
  if (ret != 0) {
    return errno_to_status(ret);
  }
  wr32(r, rate);
  *rlen = 4;
  return PROTO_OK;
}

static enum proto_status h_logic_status(const uint8_t *p, size_t len, uint8_t *r, size_t *rlen)
{
  uint32_t n, rate, ti;

  ARG_UNUSED(p);
  ARG_UNUSED(len);
  logic_status(&n, &rate, &ti);
  r[0] = (uint8_t)logic_state();
  wr32(r + 1, n);
  wr32(r + 5, rate);
  wr32(r + 9, ti);
  *rlen = 13;
  return PROTO_OK;
}

static enum proto_status h_logic_read(const uint8_t *p, size_t len, uint8_t *r, size_t *rlen)
{
  uint32_t offset;
  uint16_t count;
  int ret;

  *rlen = 0;
  if (len < 6) {
    return PROTO_EINVAL;
  }
  offset = rd32(p);
  count = rd16(p + 4);
  if (count == 0 || count > 4096) {
    return PROTO_EINVAL;
  }
  ret = logic_read(offset, count, r);
  if (ret < 0) {
    return errno_to_status(ret);
  }
  *rlen = (size_t)ret;
  return PROTO_OK;
}

static enum proto_status h_logic_abort(const uint8_t *p, size_t len, uint8_t *r, size_t *rlen)
{
  ARG_UNUSED(p);
  ARG_UNUSED(len);
  ARG_UNUSED(r);
  *rlen = 0;
  return errno_to_status(logic_abort());
}

/* --- uart bridge --- */

static enum proto_status h_uart_conf(const uint8_t *p, size_t len, uint8_t *r, size_t *rlen)
{
  ARG_UNUSED(r);
  *rlen = 0;
  if (len < 5) {
    return PROTO_EINVAL;
  }
  return errno_to_status(bridge_conf((enum bridge_mode)p[0], rd32(p + 1)));
}

static enum proto_status h_uart_status(const uint8_t *p, size_t len, uint8_t *r, size_t *rlen)
{
  bool active;
  uint32_t baud, rx, tx;

  ARG_UNUSED(p);
  ARG_UNUSED(len);
  struct bridge_ctrl_status cs;

  bridge_status(&active, &baud, &rx, &tx);
  bridge_ctrl_get(&cs);
  r[0] = active;
  wr32(r + 1, baud);
  wr32(r + 5, rx);
  wr32(r + 9, tx);
  /* since 1.1: control lines */
  r[13] = (uint8_t)bridge_get_mode();
  r[14] = (uint8_t)cs.mode;
  r[15] = cs.reset_dio;
  r[16] = cs.boot_dio;
  r[17] = (cs.dtr ? 1 : 0) | (cs.rts ? 2 : 0) | (cs.reset_low ? 4 : 0) | (cs.boot_low ? 8 : 0);
  *rlen = 18;
  return PROTO_OK;
}

static enum proto_status h_uart_ctrl(const uint8_t *p, size_t len, uint8_t *r, size_t *rlen)
{
  ARG_UNUSED(r);
  *rlen = 0;
  if (len < 3) {
    return PROTO_EINVAL;
  }
  return errno_to_status(bridge_ctrl((enum bridge_ctrl_mode)p[0], p[1], p[2]));
}

/* --- emulated targets --- */

static enum proto_status h_emu_i2c(const uint8_t *p, size_t len, uint8_t *r, size_t *rlen)
{
  ARG_UNUSED(r);
  *rlen = 0;
  if (len < 1) {
    return PROTO_EINVAL;
  }
  if (!p[0]) {
    emu_stop();
    return PROTO_OK;
  }
  if (len < 5) {
    return PROTO_EINVAL;
  }
  return errno_to_status(emu_i2c_start(p[1], p[2], p[3], p[4] != 0));
}

static enum proto_status h_emu_spi(const uint8_t *p, size_t len, uint8_t *r, size_t *rlen)
{
  struct emu_spi_params sp;

  ARG_UNUSED(r);
  *rlen = 0;
  if (len < 1) {
    return PROTO_EINVAL;
  }
  if (!p[0]) {
    emu_stop();
    return PROTO_OK;
  }
  if (len < 10) {
    return PROTO_EINVAL;
  }
  sp.mode = p[1];
  sp.cs = p[2];
  sp.sck = p[3];
  sp.mosi = p[4];
  sp.miso = p[5];
  sp.addr_mask = p[6];
  sp.read_mask = p[7];
  sp.read_value = p[8];
  sp.status = p[9];
  return errno_to_status(emu_spi_start(&sp));
}

static enum proto_status h_emu_regs(const uint8_t *p, size_t len, uint8_t *r, size_t *rlen)
{
  *rlen = 0;
  if (len < 1) {
    return PROTO_EINVAL;
  }
  switch (p[0]) {
  case 0:
    if (len < 2 || len - 2 > EMU_REGS) {
      return PROTO_EINVAL;
    }
    emu_regs_set(p[1], p + 2, len - 2);
    return PROTO_OK;
  case 1: {
    uint16_t n;

    if (len < 4) {
      return PROTO_EINVAL;
    }
    n = rd16(p + 2);
    if (n == 0 || n > EMU_REGS) {
      return PROTO_EINVAL;
    }
    emu_regs_get(p[1], r, n);
    *rlen = n;
    return PROTO_OK;
  }
  case 2:
    if (len < 33) {
      return PROTO_EINVAL;
    }
    emu_ro_set(p + 1);
    return PROTO_OK;
  default:
    return PROTO_EINVAL;
  }
}

static enum proto_status h_emu_status(const uint8_t *p, size_t len, uint8_t *r, size_t *rlen)
{
  struct emu_stats st;

  ARG_UNUSED(p);
  ARG_UNUSED(len);
  emu_get_stats(&st);
  r[0] = (uint8_t)st.kind;
  wr32(r + 1, st.txns);
  wr32(r + 5, st.reads);
  wr32(r + 9, st.writes);
  wr32(r + 13, st.naks);
  /* since 1.1: low-level counters */
  wr32(r + 17, st.starts);
  wr32(r + 21, st.stops);
  wr32(r + 25, st.bytes);
  *rlen = 29;
  return PROTO_OK;
}

/* --- sequencer --- */

static enum proto_status h_seq_load(const uint8_t *p, size_t len, uint8_t *r, size_t *rlen)
{
  ARG_UNUSED(r);
  *rlen = 0;
  if (len < 2) {
    return PROTO_EINVAL;
  }
  return errno_to_status(seq_load(rd16(p), p + 2, len - 2));
}

static enum proto_status h_seq_run(const uint8_t *p, size_t len, uint8_t *r, size_t *rlen)
{
  ARG_UNUSED(r);
  *rlen = 0;
  if (len < 6) {
    return PROTO_EINVAL;
  }
  return errno_to_status(seq_run(rd16(p), rd32(p + 2)));
}

static enum proto_status h_seq_status(const uint8_t *p, size_t len, uint8_t *r, size_t *rlen)
{
  struct seq_status st;

  ARG_UNUSED(p);
  ARG_UNUSED(len);
  seq_get_status(&st);
  r[0] = (uint8_t)st.state;
  r[1] = st.error;
  wr16(r + 2, st.pc);
  wr16(r + 4, st.result_len);
  *rlen = 6;
  return PROTO_OK;
}

static enum proto_status h_seq_result(const uint8_t *p, size_t len, uint8_t *r, size_t *rlen)
{
  int ret;

  *rlen = 0;
  if (len < 4) {
    return PROTO_EINVAL;
  }
  ret = seq_result(rd16(p), MIN(rd16(p + 2), (uint16_t)4096), r);
  if (ret < 0) {
    return errno_to_status(ret);
  }
  *rlen = (size_t)ret;
  return PROTO_OK;
}

static enum proto_status h_seq_abort(const uint8_t *p, size_t len, uint8_t *r, size_t *rlen)
{
  ARG_UNUSED(p);
  ARG_UNUSED(len);
  ARG_UNUSED(r);
  *rlen = 0;
  return errno_to_status(seq_abort());
}

/* --- dispatch ----------------------------------------------------------- */

static const proto_handler_t handlers[0x80] = {
  [CMD_PING] = h_ping,
  [CMD_INFO] = h_info,
  [CMD_BOOTLOADER] = h_bootloader,
  [CMD_LED] = h_led,
  [CMD_POWER] = h_power,
  [CMD_PULLUPS] = h_pullups,
  [CMD_DIO_DIR] = h_dio_dir,
  [CMD_DIO_WRITE] = h_dio_write,
  [CMD_DIO_READ] = h_dio_read,
  [CMD_STATUS] = h_status,
  [CMD_DISPLAY_TEXT] = h_display_text,
  [CMD_ADC_READ] = h_adc_read,
  [CMD_DISPLAY_PINOUT] = h_display_pinout,
  [CMD_SPI_CONF] = h_spi_conf,
  [CMD_SPI_SEL] = h_spi_sel,
  [CMD_SPI_UNSEL] = h_spi_unsel,
  [CMD_SPI_WRITE] = h_spi_write,
  [CMD_SPI_READ] = h_spi_read,
  [CMD_SPI_XFER] = h_spi_xfer,
  [CMD_SPI_TXN] = h_spi_txn,
  [CMD_SPI_SNIFF] = h_spi_sniff,
  [CMD_I2C_CONF] = h_i2c_conf,
  [CMD_I2C_SCAN] = h_i2c_scan,
  [CMD_I2C_WRITE] = h_i2c_write,
  [CMD_I2C_READ] = h_i2c_read,
  [CMD_I2C_WRITE_READ] = h_i2c_write_read,
  [CMD_I2C_START] = h_i2c_start,
  [CMD_I2C_STOP] = h_i2c_stop,
  [CMD_I2C_RAW_WRITE] = h_i2c_raw_write,
  [CMD_I2C_RAW_READ] = h_i2c_raw_read,
  [CMD_I2C_SNIFF] = h_i2c_sniff,
  [CMD_I2C_RESET] = h_i2c_reset,
  [CMD_SCOPE_ARM] = h_scope_arm,
  [CMD_SCOPE_STATUS] = h_scope_status,
  [CMD_SCOPE_READ] = h_scope_read,
  [CMD_SCOPE_ABORT] = h_scope_abort,
  [CMD_AWG_SET] = h_awg_set,
  [CMD_AWG_OFF] = h_awg_off,
  [CMD_AWG_STATUS] = h_awg_status,
  [CMD_AWG_ARB_LOAD] = h_awg_arb_load,
  [CMD_AWG_DC] = h_awg_dc,
  [CMD_AWG_STEP] = h_awg_step,
  [CMD_PWM_SET] = h_pwm_set,
  [CMD_PWM_PHASE] = h_pwm_phase,
  [CMD_PWM_SYNC] = h_pwm_sync,
  [CMD_PWM_STOP] = h_pwm_stop,
  [CMD_PWM_STATUS] = h_pwm_status,
  [CMD_LOGIC_ARM] = h_logic_arm,
  [CMD_LOGIC_STATUS] = h_logic_status,
  [CMD_LOGIC_READ] = h_logic_read,
  [CMD_LOGIC_ABORT] = h_logic_abort,
  [CMD_UART_CONF] = h_uart_conf,
  [CMD_UART_STATUS] = h_uart_status,
  [CMD_UART_CTRL] = h_uart_ctrl,
  [CMD_TIME] = h_time,
  [CMD_DIO_WATCH] = h_dio_watch,
  [CMD_EMU_I2C] = h_emu_i2c,
  [CMD_EMU_SPI] = h_emu_spi,
  [CMD_EMU_REGS] = h_emu_regs,
  [CMD_EMU_STATUS] = h_emu_status,
  [CMD_SEQ_LOAD] = h_seq_load,
  [CMD_SEQ_RUN] = h_seq_run,
  [CMD_SEQ_STATUS] = h_seq_status,
  [CMD_SEQ_RESULT] = h_seq_result,
  [CMD_SEQ_ABORT] = h_seq_abort,
};

/* Reply data is built straight into tx_frame's payload area (after the
 * status byte) to avoid a second 4KB copy; proto_send_frame() notices
 * the aliasing and skips the memmove. */
static void proto_dispatch(uint8_t cmd, uint8_t seq, const uint8_t *payload, size_t len)
{
  uint8_t *reply;
  size_t rlen = 0;
  enum proto_status st;

  k_mutex_lock(&tx_lock, K_FOREVER);
  reply = tx_frame + PROTO_HDR_LEN;
  if (cmd <= PROTO_CMD_MAX && handlers[cmd] != NULL) {
    st = handlers[cmd](payload, len, reply + 1, &rlen);
  } else {
    st = PROTO_ENOCMD;
  }
  reply[0] = (uint8_t)st;
  proto_send_frame(cmd | PROTO_REPLY_FLAG, seq, reply, 1 + rlen, K_MSEC(500));
  k_mutex_unlock(&tx_lock);

  if (cmd == CMD_BOOTLOADER && st == PROTO_OK) {
    k_sleep(K_MSEC(100)); /* let the reply drain */
    reset_usb_boot(0, 0);
  }
}

/* --- receive thread ---------------------------------------------------- */

#define PROTO_PARTIAL_TIMEOUT_MS 500

static void proto_thread(void *a, void *b, void *c)
{
  size_t have = 0;
  int64_t link_shown_at = 0;
  int64_t last_byte_at = 0;
  uint32_t dropped_seen = 0;

  ARG_UNUSED(a);
  ARG_UNUSED(b);
  ARG_UNUSED(c);

  for (;;) {
    size_t got = port_read(&api_port, rx_frame + have, sizeof(rx_frame) - have, K_MSEC(100));

    if (api_port.rx_dropped != dropped_seen) {
      LOG_WRN("API rx overrun: %u bytes dropped", api_port.rx_dropped - dropped_seen);
      dropped_seen = api_port.rx_dropped;
    }
    if (got == 0) {
      /* A frame that stopped arriving halfway is never going to finish
       * (bytes lost, or a host that gave up): start over. */
      if (have > 0 && k_uptime_get() - last_byte_at > PROTO_PARTIAL_TIMEOUT_MS) {
        have = 0;
      }
      if (link_shown_at != 0 && k_uptime_get() - last_request_at > 5000) {
        link_shown_at = 0;
        link_shown = false;
        ui_sys_refresh();
      }
      continue;
    }
    have += got;
    last_byte_at = k_uptime_get();

    for (;;) {
      size_t len, frame_len, i;
      uint8_t cmd, seq;

      /* Resync: drop bytes until a start-of-frame pair. */
      for (i = 0; i + 1 < have; i++) {
        if (rx_frame[i] == PROTO_SOF0 && rx_frame[i + 1] == PROTO_SOF1) {
          break;
        }
      }
      if (i > 0) {
        memmove(rx_frame, rx_frame + i, have - i);
        have -= i;
      }
      if (have < PROTO_HDR_LEN) {
        break;
      }
      len = rd16(&rx_frame[4]);
      if (len > PROTO_MAX_PAYLOAD) {
        /* Garbage header: skip this SOF and keep looking. */
        memmove(rx_frame, rx_frame + 2, have - 2);
        have -= 2;
        continue;
      }
      frame_len = PROTO_HDR_LEN + len + PROTO_CRC_LEN;
      if (have < frame_len) {
        if (have == sizeof(rx_frame)) {
          /* Buffer full and still no complete frame: whatever this is,
           * it isn't a frame - skip the SOF so the loop can't stall. */
          memmove(rx_frame, rx_frame + 2, have - 2);
          have -= 2;
          continue;
        }
        break;
      }
      if (proto_crc(&rx_frame[2], PROTO_HDR_LEN - 2 + len) != rd16(&rx_frame[PROTO_HDR_LEN + len])) {
        LOG_WRN("bad crc, cmd 0x%02x", rx_frame[2]);
        memmove(rx_frame, rx_frame + 2, have - 2);
        have -= 2;
        continue;
      }
      cmd = rx_frame[2];
      seq = rx_frame[3];
      last_request_at = k_uptime_get();
      if (link_shown_at == 0) {
        link_shown_at = last_request_at;
        link_shown = true;
        ui_sys_refresh();
      }
      proto_dispatch(cmd, seq, &rx_frame[PROTO_HDR_LEN], len);
      memmove(rx_frame, rx_frame + frame_len, have - frame_len);
      have -= frame_len;
    }
  }
}

K_THREAD_STACK_DEFINE(proto_stack, 6144);
static struct k_thread proto_thread_data;

void proto_init(void)
{
  int ret = port_init(&api_port, USB_ACM_API, api_rx_ring, sizeof(api_rx_ring), api_tx_ring,
                      sizeof(api_tx_ring), true);

  if (ret != 0) {
    LOG_ERR("API port not ready (%d)", ret);
    return;
  }
  board_on_change(on_board_change);
  k_thread_create(&proto_thread_data, proto_stack, K_THREAD_STACK_SIZEOF(proto_stack),
                  proto_thread, NULL, NULL, NULL, K_PRIO_PREEMPT(5), 0, K_NO_WAIT);
  k_thread_name_set(&proto_thread_data, "api");
}
