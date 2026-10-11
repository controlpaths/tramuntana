/*
 * Tramuntana - USB composite device. See usb.h.
 *
 * Same recipe as Zephyr's samples/subsys/usb/common/sample_usbd_init.c:
 * one device, one full-speed configuration, every CDC-ACM instance in
 * the devicetree registered into it (usbd_register_all_classes()). The
 * stock CONFIG_CDC_ACM_SERIAL_INITIALIZE_AT_BOOT helper only ever
 * registers the first instance, hence this file.
 */

#include <zephyr/kernel.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/logging/log.h>

#include "usb.h"

LOG_MODULE_REGISTER(usb, LOG_LEVEL_INF);

#define TRAMUNTANA_USB_VID 0x2fe3 /* Zephyr Project's VID, as the old firmware */
#define TRAMUNTANA_USB_PID 0x0100

USBD_DEVICE_DEFINE(tramuntana_usbd, DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)),
                   TRAMUNTANA_USB_VID, TRAMUNTANA_USB_PID);

USBD_DESC_LANG_DEFINE(usb_lang);
USBD_DESC_MANUFACTURER_DEFINE(usb_mfr, "controlpaths");
USBD_DESC_PRODUCT_DEFINE(usb_product, "Tramuntana Lite r1a");
USBD_DESC_SERIAL_NUMBER_DEFINE(usb_sn);
USBD_DESC_CONFIG_DEFINE(usb_fs_cfg_desc, "Tramuntana");

USBD_CONFIGURATION_DEFINE(usb_fs_config, 0, 250, &usb_fs_cfg_desc);

int usb_init(void)
{
  int err;

  err = usbd_add_descriptor(&tramuntana_usbd, &usb_lang);
  if (err == 0) {
    err = usbd_add_descriptor(&tramuntana_usbd, &usb_mfr);
  }
  if (err == 0) {
    err = usbd_add_descriptor(&tramuntana_usbd, &usb_product);
  }
  if (err == 0) {
    err = usbd_add_descriptor(&tramuntana_usbd, &usb_sn);
  }
  if (err != 0) {
    LOG_ERR("descriptor setup failed (%d)", err);
    return err;
  }

  err = usbd_add_configuration(&tramuntana_usbd, USBD_SPEED_FS, &usb_fs_config);
  if (err != 0) {
    LOG_ERR("add configuration failed (%d)", err);
    return err;
  }

  err = usbd_register_all_classes(&tramuntana_usbd, USBD_SPEED_FS, 1, NULL);
  if (err != 0) {
    LOG_ERR("register classes failed (%d)", err);
    return err;
  }

  /* CDC-ACM uses interface association descriptors: advertise that at
   * the device level so hosts group each port's two interfaces. */
  usbd_device_set_code_triple(&tramuntana_usbd, USBD_SPEED_FS, USB_BCC_MISCELLANEOUS, 0x02,
                              0x01);

  err = usbd_init(&tramuntana_usbd);
  if (err != 0) {
    LOG_ERR("usbd_init failed (%d)", err);
    return err;
  }

  err = usbd_enable(&tramuntana_usbd);
  if (err != 0) {
    LOG_ERR("usbd_enable failed (%d)", err);
    return err;
  }
  return 0;
}
