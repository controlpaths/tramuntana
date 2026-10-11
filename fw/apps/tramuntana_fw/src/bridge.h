/*
 * Tramuntana - bridge: transparent USB <-> UART1 bridge. Bytes arriving
 * on the third CDC-ACM port go out on DIO7 (TX) and whatever comes in on
 * DIO6 (RX) goes back to the host, at the baud rate the host set on the
 * CDC port. See bridge.c.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

enum bridge_mode {
  BRIDGE_AUTO = 0,   /* on while the host holds DTR on the bridge port */
  BRIDGE_ON = 1,
  BRIDGE_OFF = 2,
};

/* Control lines: the host's RTS/DTR on the bridge port drive two DIOs as
 * open-drain outputs (asserted = pulled low, deasserted = released), the
 * way a USB-serial adapter's RTS#/DTR# pins reset a target and pick its
 * boot mode - so esptool and tools that reset a target the same way
 * work through the bridge unchanged. The pins only ever pull low.
 *
 *   BRIDGE_CTRL_DIRECT  reset = RTS, boot = DTR (an adapter wired
 *                       straight to RESET/BOOT; note that opening the
 *                       port asserts both, holding the target in reset)
 *   BRIDGE_CTRL_ESP     the two-transistor auto-reset circuit of ESP32 /
 *                       ESP8266 boards: reset low only when RTS && !DTR,
 *                       boot low only when DTR && !RTS (both asserted, as
 *                       on open, does nothing)
 *
 * While control lines are on, the bridge stays active whatever DTR says
 * (BRIDGE_ON), since DTR is now a signal rather than "port open". */
enum bridge_ctrl_mode {
  BRIDGE_CTRL_OFF = 0,
  BRIDGE_CTRL_DIRECT = 1,
  BRIDGE_CTRL_ESP = 2,
};

#define BRIDGE_CTRL_NO_PIN 0xFF

struct bridge_ctrl_status {
  enum bridge_ctrl_mode mode;
  uint8_t reset_dio;
  uint8_t boot_dio;
  bool dtr;
  bool rts;
  bool reset_low;
  bool boot_low;
};

void bridge_init(void);
int bridge_conf(enum bridge_mode mode, uint32_t baud);
void bridge_status(bool *active, uint32_t *baud, uint32_t *rx, uint32_t *tx);
enum bridge_mode bridge_get_mode(void);
/* reset_dio / boot_dio: DIO2..DIO7 or BRIDGE_CTRL_NO_PIN. */
int bridge_ctrl(enum bridge_ctrl_mode mode, uint8_t reset_dio, uint8_t boot_dio);
/* Sends bytes to the target's RX (DIO1) from the board itself (the
 * sequencer), alongside the host's own stream. -EPERM if the bridge isn't
 * active. */
int bridge_target_write(const uint8_t *data, size_t n);
void bridge_ctrl_get(struct bridge_ctrl_status *out);
