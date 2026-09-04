/* USB-HID transport adapter for dfu_session. See dfu_hid.h for the
   report framing. Mirrors dfu_ble.c but driven by TinyUSB HID callbacks
   instead of NimBLE GATT. */

#include "dfu_hid.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "sdkconfig.h"
#include "tusb.h"

#include "dfu_session.h"

#if !defined(CFG_TUD_HID) || !CFG_TUD_HID
#error "CONFIG_DFU_TRANSPORT_HID needs the TinyUSB HID class (CFG_TUD_HID). \
Enable USB HID for this build, or set CONFIG_DFU_TRANSPORT_HID=n.
#endif

static const char *TAG = "dfu_hid";

#define HID_REPORT_BYTES   64
#define HID_FRAME_MAX      (HID_REPORT_BYTES - 1)   /* 1-byte length prefix */

static dfu_session_t   *session;
/* IN-endpoint flow control: one report may be in flight. Given on init
   and in on_in_complete; taken by hid_send before tud_hid_report. */
static SemaphoreHandle_t tx_ready;

/* --- transport vtable ---------------------------------------------- */

static int hid_send(void *ctx, const void *buf, size_t len)
{
    (void)ctx;
    if (len > HID_FRAME_MAX) return -1;
    if (!tx_ready) return -2;
    /* Wait for the previous IN report to be collected by the host. */
    if (xSemaphoreTake(tx_ready, pdMS_TO_TICKS(2000)) != pdTRUE) return -3;
    if (!tud_hid_ready()) { xSemaphoreGive(tx_ready); return -4; }

    uint8_t rpt[HID_REPORT_BYTES];
    memset(rpt, 0, sizeof(rpt));
    rpt[0] = (uint8_t)len;
    memcpy(rpt + 1, buf, len);
    if (!tud_hid_report(0, rpt, sizeof(rpt))) {
        xSemaphoreGive(tx_ready);
        return -5;
    }
    return 0;
}

static size_t hid_max_payload(void *ctx)
{
    (void)ctx;
    /* Largest protocol frame one report can carry (after the 1-byte
       length prefix). dfu_session subtracts its own DATA header. */
    return HID_FRAME_MAX;
}

/* --- TinyUSB callback glue (called from usb_hid.c) ----------------- */

void dfu_hid_on_out(const uint8_t *buf, size_t len)
{
    if (!session || len < 1) return;
    uint8_t flen = buf[0];
    if (flen == 0 || flen > HID_FRAME_MAX || (size_t)flen + 1 > len) {
        ESP_LOGW(TAG, "bad HID frame len=%u (report %zu B)", flen, len);
        return;
    }
    dfu_session_feed_frame(session, buf + 1, flen);
}

void dfu_hid_on_in_complete(void)
{
    if (tx_ready) {
        BaseType_t hp = pdFALSE;
        xSemaphoreGiveFromISR(tx_ready, &hp);  /* completes in USB task ctx */
        portYIELD_FROM_ISR(hp);
    }
}

/* --- public API ---------------------------------------------------- */

esp_err_t dfu_hid_register(void)
{
    if (session) return ESP_OK;
    tx_ready = xSemaphoreCreateBinary();
    if (!tx_ready) return ESP_ERR_NO_MEM;
    xSemaphoreGive(tx_ready);   /* endpoint starts free */

    const dfu_session_transport_t tx = {
        .send             = hid_send,
        .max_payload      = hid_max_payload,
        .on_session_start = NULL,
        .on_release       = NULL,
        .ctx              = NULL,
        .source_id        = DFU_SRC_USB_HID,
        .name             = "hid",
    };
    const dfu_session_config_t cfg = {
        .window_bytes     = CONFIG_DFU_SESSION_WINDOW_BYTES,
        .idle_timeout_ms  = CONFIG_DFU_SESSION_IDLE_TIMEOUT_MS,
        .hw_id            = CONFIG_DFU_TRANSPORT_HW_ID,
    };
    esp_err_t err = dfu_session_init(&session, &tx, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "dfu_session_init: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "registered (HID OTA, %d-byte frames)", HID_FRAME_MAX);
    return ESP_OK;
}
