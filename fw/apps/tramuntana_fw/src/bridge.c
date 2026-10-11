/*
 * Tramuntana - bridge. See bridge.h.
 *
 * Both ends are 'port' ring buffers (port.h): the third CDC-ACM instance
 * on the USB side and &uart1 (PL011, GPIO6 TX / GPIO7 RX = DIO1 / DIO0,
 * through the RP2350's UART_AUX pin function) on the header side. One thread shuttles bytes both ways and, once per
 * loop, mirrors the host's line coding: whatever baud rate / parity /
 * stop bits the host program set on /dev/ttyACM2 is applied to UART1
 * with uart_configure(), so 'picocom -b 9600 /dev/ttyACM2' just works.
 *
 * In BRIDGE_AUTO (the default) the bridge follows DTR: opening the port
 * on the host claims DIO0/DIO1 for UART1 (exclusively - plain GPIO use of
 * those two pins gets -EBUSY meanwhile), closing it releases them. The
 * bridge has pins of its own, so it runs alongside the SPI and I2C
 * masters.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>
#include <string.h>

#include <hardware/gpio.h>

#include "bridge.h"
#include "port.h"
#include "usb.h"
#include "board.h"
#include "display.h"
#include "proto.h"

LOG_MODULE_REGISTER(bridge, LOG_LEVEL_INF);

static const struct device *const uart_dev = DEVICE_DT_GET(DT_NODELABEL(uart1));

static struct port usb_port;
static struct port hw_port;
static uint8_t usb_rx[2048], usb_tx[2048], hw_rx[2048], hw_tx[2048];

static struct {
  enum bridge_mode mode;
  uint32_t forced_baud;   /* 0 = follow the host */
  uint32_t baud;          /* currently applied */
  struct uart_config applied;
  bool active;
  bool ready;
  /* control lines */
  enum bridge_ctrl_mode ctrl;
  enum bridge_mode mode_before_ctrl;
  uint8_t reset_dio;
  uint8_t boot_dio;
  bool dtr;
  bool rts;
  bool reset_low;
  bool boot_low;
} br;

K_THREAD_STACK_DEFINE(bridge_stack, 2048);
static struct k_thread bridge_thread_data;

static void bridge_ui(void)
{
  if (!br.ready) {
    ui_set(UI_ROW_UART, "n/a");
  } else if (br.active) {
    ui_set(UI_ROW_UART, "%u %u%c%u bridge%s", br.baud, br.applied.data_bits + 5,
           br.applied.parity == UART_CFG_PARITY_NONE ? 'N' :
           br.applied.parity == UART_CFG_PARITY_EVEN ? 'E' : 'O',
           br.applied.stop_bits == UART_CFG_STOP_BITS_2 ? 2 : 1,
           br.ctrl == BRIDGE_CTRL_OFF ? "" : " +ctl");
  } else {
    ui_set(UI_ROW_UART, "%s", br.mode == BRIDGE_OFF ? "off" : "idle (open ACM2)");
  }
}

static void bridge_apply_line_coding(void)
{
  struct uart_config cfg;

  if (uart_config_get(usb_port.dev, &cfg) != 0) {
    cfg.baudrate = 115200;
    cfg.parity = UART_CFG_PARITY_NONE;
    cfg.stop_bits = UART_CFG_STOP_BITS_1;
    cfg.data_bits = UART_CFG_DATA_BITS_8;
  }
  cfg.flow_ctrl = UART_CFG_FLOW_CTRL_NONE;
  if (br.forced_baud != 0) {
    cfg.baudrate = br.forced_baud;
  }
  if (cfg.baudrate == 0) {
    cfg.baudrate = 115200;
  }
  if (memcmp(&cfg, &br.applied, sizeof(cfg)) != 0) {
    if (uart_configure(uart_dev, &cfg) == 0) {
      br.applied = cfg;
      br.baud = cfg.baudrate;
      bridge_ui();
    }
  }
}

static bool bridge_host_open(void)
{
  return br.dtr;
}

/* Open-drain: low = output (the pad's output value stays 0), released =
 * input, so the target's own pull-up sets the high level. */
static void ctrl_pin(uint8_t dio, bool low)
{
  if (dio == BRIDGE_CTRL_NO_PIN) {
    return;
  }
  gpio_set_dir(DIO_GPIO(dio), low ? GPIO_OUT : GPIO_IN);
}

static void ctrl_apply(void)
{
  if (br.ctrl == BRIDGE_CTRL_DIRECT) {
    br.reset_low = br.rts;
    br.boot_low = br.dtr;
  } else if (br.ctrl == BRIDGE_CTRL_ESP) {
    br.reset_low = br.rts && !br.dtr;
    br.boot_low = br.dtr && !br.rts;
  } else {
    br.reset_low = false;
    br.boot_low = false;
    return;
  }
  ctrl_pin(br.reset_dio, br.reset_low);
  ctrl_pin(br.boot_dio, br.boot_low);
}

/* Reads the host's DTR/RTS; on a change drives the control pins and
 * tells the host (EVT_LINE, for the timeline). */
static void bridge_poll_lines(void)
{
  uint32_t dtr = 0, rts = 0;

  uart_line_ctrl_get(usb_port.dev, UART_LINE_CTRL_DTR, &dtr);
  uart_line_ctrl_get(usb_port.dev, UART_LINE_CTRL_RTS, &rts);
  if ((dtr != 0) == br.dtr && (rts != 0) == br.rts) {
    return;
  }
  br.dtr = dtr != 0;
  br.rts = rts != 0;
  ctrl_apply();
  {
    uint8_t evt[6];

    wr32(evt, board_time_us());
    evt[4] = br.dtr;
    evt[5] = br.rts;
    proto_send_event(EVT_LINE, evt, sizeof(evt));
  }
}

static int bridge_activate(void)
{
  int ret = board_pins_claim(DIO_MASK_UART, OWNER_UART, true);

  if (ret != 0) {
    return ret;
  }
  memset(&br.applied, 0, sizeof(br.applied));
  bridge_apply_line_coding();
  port_rx_flush(&hw_port);
  port_rx_flush(&usb_port);
  br.active = true;
  bridge_ui();
  ui_activity("UART on");
  return 0;
}

static void bridge_deactivate(void)
{
  board_pins_release(DIO_MASK_UART, OWNER_UART);
  br.active = false;
  bridge_ui();
}

static void bridge_thread(void *a, void *b, void *c)
{
  uint8_t buf[256];
  int idle_loops = 0;

  ARG_UNUSED(a);
  ARG_UNUSED(b);
  ARG_UNUSED(c);

  for (;;) {
    bool want;

    bridge_poll_lines();
    want = (br.mode == BRIDGE_ON) || (br.mode == BRIDGE_AUTO && bridge_host_open());

    if (want && !br.active) {
      if (bridge_activate() != 0) {
        k_sleep(K_MSEC(200)); /* pins busy: retry later */
        continue;
      }
    } else if (!want && br.active) {
      bridge_deactivate();
    }

    if (!br.active) {
      k_sleep(K_MSEC(50));
      continue;
    }

    if (++idle_loops >= 10) {
      bridge_apply_line_coding();
      idle_loops = 0;
    }

    {
      size_t n = port_read(&usb_port, buf, sizeof(buf), K_NO_WAIT);
      bool moved = false;

      if (n > 0) {
        port_write(&hw_port, buf, n, K_MSEC(100));
        moved = true;
      }
      n = port_read(&hw_port, buf, sizeof(buf), K_NO_WAIT);
      if (n > 0) {
        port_write(&usb_port, buf, n, K_MSEC(20));
        moved = true;
      }
      if (!moved) {
        k_sleep(K_MSEC(1));
      }
    }
  }
}

int bridge_conf(enum bridge_mode mode, uint32_t baud)
{
  if (mode > BRIDGE_OFF) {
    return -EINVAL;
  }
  if (!br.ready) {
    return -ENODEV;
  }
  br.mode = mode;
  br.forced_baud = baud;
  bridge_ui();
  return 0;
}

void bridge_status(bool *active, uint32_t *baud, uint32_t *rx, uint32_t *tx)
{
  *active = br.active;
  *baud = br.baud;
  *rx = hw_port.rx_total;   /* bytes received from the target */
  *tx = hw_port.tx_total;   /* bytes sent to the target */
}

enum bridge_mode bridge_get_mode(void)
{
  return br.mode;
}

static bool ctrl_dio_ok(uint8_t dio)
{
  return dio == BRIDGE_CTRL_NO_PIN || (dio >= 2 && dio < DIO_COUNT);
}

static uint8_t ctrl_mask(uint8_t reset_dio, uint8_t boot_dio)
{
  return (reset_dio != BRIDGE_CTRL_NO_PIN ? BIT(reset_dio) : 0) |
         (boot_dio != BRIDGE_CTRL_NO_PIN ? BIT(boot_dio) : 0);
}

int bridge_ctrl(enum bridge_ctrl_mode mode, uint8_t reset_dio, uint8_t boot_dio)
{
  uint8_t mask;
  int ret, i;

  if (mode > BRIDGE_CTRL_ESP) {
    return -EINVAL;
  }
  if (!br.ready) {
    return -ENODEV;
  }
  if (mode != BRIDGE_CTRL_OFF &&
      (!ctrl_dio_ok(reset_dio) || !ctrl_dio_ok(boot_dio) ||
       (reset_dio == boot_dio && reset_dio != BRIDGE_CTRL_NO_PIN))) {
    return -EINVAL;
  }
  /* Drop the previous pins first (released = plain inputs again). */
  if (br.ctrl != BRIDGE_CTRL_OFF) {
    board_pins_release(ctrl_mask(br.reset_dio, br.boot_dio), OWNER_UART);
    br.mode = br.mode_before_ctrl;
    br.ctrl = BRIDGE_CTRL_OFF;
  }
  if (mode == BRIDGE_CTRL_OFF) {
    br.reset_low = br.boot_low = false;
    bridge_ui();
    return 0;
  }
  mask = ctrl_mask(reset_dio, boot_dio);
  ret = board_pins_claim(mask, OWNER_UART, true);
  if (ret != 0) {
    return ret;
  }
  /* The claim muxed them to UART_AUX like DIO0/1: these two are plain
   * open-drain GPIOs instead. Input first, output value 0 for "low". */
  for (i = 0; i < DIO_COUNT; i++) {
    if (mask & BIT(i)) {
      gpio_set_dir(DIO_GPIO(i), GPIO_IN);
      gpio_put(DIO_GPIO(i), 0);
      gpio_set_function(DIO_GPIO(i), GPIO_FUNC_SIO);
    }
  }
  br.reset_dio = reset_dio;
  br.boot_dio = boot_dio;
  br.mode_before_ctrl = br.mode;
  br.mode = BRIDGE_ON;
  br.ctrl = mode;
  ctrl_apply();
  bridge_ui();
  return 0;
}

int bridge_target_write(const uint8_t *data, size_t n)
{
  if (!br.active) {
    return -EPERM;
  }
  return port_write(&hw_port, data, n, K_MSEC(100)) == n ? 0 : -ETIMEDOUT;
}

void bridge_ctrl_get(struct bridge_ctrl_status *out)
{
  out->mode = br.ctrl;
  out->reset_dio = br.reset_dio;
  out->boot_dio = br.boot_dio;
  out->dtr = br.dtr;
  out->rts = br.rts;
  out->reset_low = br.reset_low;
  out->boot_low = br.boot_low;
}

void bridge_init(void)
{
  if (port_init(&usb_port, USB_ACM_BRIDGE, usb_rx, sizeof(usb_rx), usb_tx, sizeof(usb_tx), true) != 0 ||
      port_init(&hw_port, uart_dev, hw_rx, sizeof(hw_rx), hw_tx, sizeof(hw_tx), false) != 0) {
    LOG_ERR("bridge ports not ready");
    br.ready = false;
    bridge_ui();
    return;
  }
  br.ready = true;
  br.mode = BRIDGE_AUTO;
  /* Above the sequencer (seq.c, priority 2): its busy waits must not hold
   * up the target's bytes, a new baud rate or a DTR/RTS change for the
   * length of a run. The work here is short (a few bytes, then sleep), so
   * it only adds microseconds of jitter to a sequence. */
  k_thread_create(&bridge_thread_data, bridge_stack, K_THREAD_STACK_SIZEOF(bridge_stack),
                  bridge_thread, NULL, NULL, NULL, K_PRIO_PREEMPT(1), 0, K_NO_WAIT);
  k_thread_name_set(&bridge_thread_data, "bridge");
  bridge_ui();
}
