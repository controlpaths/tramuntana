/*
 * Tramuntana - USB composite device: three CDC-ACM ports on the Pico's
 * native USB.
 *
 *   cdc_acm_0  "Tramuntana shell"        Zephyr shell + logs (humans)
 *   cdc_acm_1  "Tramuntana API"          binary protocol (src/proto.c)
 *   cdc_acm_2  "Tramuntana UART bridge"  transparent to &uart1 (src/bridge.c)
 *
 * On Linux they enumerate as /dev/ttyACM0..2 in that order (interfaces
 * 0, 2 and 4 of the same device), and the hwinfo serial number makes
 * /dev/serial/by-id/usb-controlpaths_Tramuntana_Lite_r1a_<id>-if0{0,2,4}
 * stable across reboots.
 */
#pragma once

#include <zephyr/device.h>

#define USB_ACM_SHELL  DEVICE_DT_GET(DT_NODELABEL(cdc_acm_0))
#define USB_ACM_API    DEVICE_DT_GET(DT_NODELABEL(cdc_acm_1))
#define USB_ACM_BRIDGE DEVICE_DT_GET(DT_NODELABEL(cdc_acm_2))

/* Builds the descriptors, registers the three CDC-ACM functions and
 * enables the device. Returns 0 or a negative errno. */
int usb_init(void);
