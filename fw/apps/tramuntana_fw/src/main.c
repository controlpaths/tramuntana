/*
 * Tramuntana Lite r1a - main firmware.
 *
 * A bench tool for poking at embedded systems: SPI and I2C master and
 * sniffer, oscilloscope, waveform generator, 8-channel logic analyzer
 * and a USB-UART bridge, all on the RP2350 of a Pico 2 running Zephyr.
 *
 * Structure (src/):
 *
 *   usb.c       USB composite device: three CDC-ACM ports
 *   port.c      irq-driven ring-buffered serial port helper
 *   proto.c/h   binary API on the second port (the protocol reference)
 *   shell_cmds.c  human shell on the first port
 *   bridge.c    USB <-> UART1 bridge on the third port
 *   board.c     LED, target power, pull-ups, DIO GPIO, pin arbiter
 *   display.c   ST7735R dashboard
 *   arena.c     capture memory (DMA ring buffers)
 *   spi.c  i2c.c  scope.c  awg.c  logic.c  pwm.c   the instruments
 *   emu.c       the board as an I2C/SPI target (register file on PIO1)
 *   seq.c       sequencer: host-uploaded steps run with us timing
 *   watch.c     timestamped DIO edges for the host's timeline
 *
 * Host side: host/python (library + CLI) and host/webapp (Web Serial).
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "usb.h"
#include "board.h"
#include "display.h"
#include "proto.h"
#include "spi.h"
#include "i2c.h"
#include "scope.h"
#include "awg.h"
#include "logic.h"
#include "bridge.h"
#include "watch.h"
#include "emu.h"
#include "seq.h"
#include "stamp.h"
#include "version.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

#define SPLASH_MS 1500

int main(void)
{
  usb_init();
  board_init();
  display_init();
  stamp_init();

  spi_drv_init();
  i2c_drv_init();
  scope_init();
  awg_init();
  logic_init();
  bridge_init();
  watch_init();
  emu_init();
  seq_init();
  proto_init();

  ui_set(UI_ROW_SYS, "PWR off PU off");
  ui_set(UI_ROW_NOTE, "fw %s", FW_VERSION);
  led_flash(LED_WHITE, 300);

  printk("Tramuntana Lite r1a fw %s - type 'help'\n", FW_VERSION);

  k_sleep(K_MSEC(SPLASH_MS));
  ui_show();
  return 0;
}
