// TinyUSB configuration for the dfu_example_app example.
// All four classes (CDC, MSC, HID, DFU) are always compiled in; each
// appears in the composite descriptor unconditionally.

#pragma once

#include "sdkconfig.h"
#include "tusb_option.h"

#ifdef __cplusplus
extern "C" {
#endif

// CFG_TUSB_MCU is injected by espressif__tinyusb's CMakeLists via
// -DCFG_TUSB_MCU=OPT_MCU_ESP32S3; no need to set it here.

#define CFG_TUSB_OS            OPT_OS_FREERTOS
#define CFG_TUSB_RHPORT0_MODE  (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)

#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif
#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN     __attribute__((aligned(4)))
#endif

#define CFG_TUD_ENDPOINT0_SIZE 64

// CDC: one interface, log-only (device → host).
#define CFG_TUD_CDC            1
#define CFG_TUD_CDC_RX_BUFSIZE 512
#define CFG_TUD_CDC_TX_BUFSIZE 512
#define CFG_TUD_CDC_EP_BUFSIZE 64

// MSC: one LUN, virtual disk for UF2 drag-drop.
#define CFG_TUD_MSC            1
#define CFG_TUD_MSC_EP_BUFSIZE 512

// HID: one interface, vendor IN+OUT, 64-byte reports.
#define CFG_TUD_HID            1
#define CFG_TUD_HID_EP_BUFSIZE 64

// DFU: one alt, app-side (NOT ROM DFU).
#define CFG_TUD_DFU            1
#define CFG_TUD_DFU_XFER_BUFSIZE 4096
#define CFG_TUD_DFU_ALT        1

// Unused classes.
#define CFG_TUD_DFU_RUNTIME    0
#define CFG_TUD_MIDI           0
#define CFG_TUD_VENDOR         0
#define CFG_TUD_ECM_RNDIS      0
#define CFG_TUD_NCM            0
#define CFG_TUD_BTH            0
#define CFG_TUD_AUDIO          0
#define CFG_TUD_VIDEO          0
#define CFG_TUD_USBTMC         0

#ifdef __cplusplus
}
#endif
