#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bring up the USB composite device: CDC (log), MSC (UF2 drag-drop OTA),
 * HID (OTA), DFU (dfu-util OTA). Installs the TinyUSB PHY, starts the
 * device task, and registers the descriptor + class callbacks. Call once
 * from app_main after the transport adapters are registered. */
esp_err_t usb_start(void);

#ifdef __cplusplus
}
#endif
