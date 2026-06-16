#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* USB-HID transport adapter for dfu_session. The HID interface is a
   generic 64-byte vendor IN/OUT report. HID reports are fixed-size, so
   each report carries a 1-byte length prefix followed by one DFU Session
   protocol frame:  [len][frame bytes…][zero pad to 64].

   usb_hid.c forwards the relevant TinyUSB callbacks here:
     - OUT reports (host→device) -> dfu_hid_on_out()
     - IN report completion       -> dfu_hid_on_in_complete()
   so the IN endpoint's one-report-in-flight flow control is respected. */

esp_err_t dfu_hid_register(void);

/* Called from tud_hid_set_report_cb (OUTPUT). buf/len is the raw 64-byte
   report. */
void dfu_hid_on_out(const uint8_t *buf, size_t len);

/* Called from tud_hid_report_complete_cb — the previous IN report has
   been collected by the host, so the next may be sent. */
void dfu_hid_on_in_complete(void);
