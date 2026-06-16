/* MSC UF2 transport adapter for dfu_core.
 *
 * Extracts the sniff-and-feed logic from usb_msc.c's tud_msc_write10_cb.
 * Has NO TinyUSB dependency — pure bytes→dfu_core. The MSC driver itself
 * (usb_msc.c) stays in the consumer and calls dfu_msc_uf2_write() per
 * 512-byte sector, and dfu_msc_uf2_reset() on USB mount.
 *
 * The MSC drive is the only OTA transport that's UF2-only: there's no
 * out-of-band "this is firmware" marker on a drag-drop write, so we sniff
 * the UF2 magic on each 512-byte sector and forward only those to dfu_core.
 * FAT metadata and any non-UF2 file content pass through to the
 * success-but-ignored path so the host doesn't see I/O errors. */

#include "dfu_msc_uf2.h"

#include <string.h>

#include "esp_log.h"

#include "dfu_core.h"

static const char *TAG = "dfu_msc_uf2";

/* Latched once dfu_core reports a terminal result. A successful complete
   reboots within ~1 s, so this just suppresses re-acquire churn from the
   host's trailing sector flushes. On a genuine failure it stops the
   detect→feed→abort→re-detect spin that an in-place retry would otherwise
   produce. Cleared on (re)mount via dfu_msc_uf2_reset(). */
static bool msc_ota_acquired;
static bool msc_ota_done;

static void msc_ota_event_cb(dfu_source_t src, dfu_event_t evt, esp_err_t err, void *user)
{
    (void)src; (void)user;
    switch (evt) {
    case DFU_EVT_COMPLETE_OK:
        ESP_LOGI(TAG, "OTA via MSC complete, rebooting");
        msc_ota_acquired = false;
        msc_ota_done = true;
        break;
    case DFU_EVT_ERROR:
        ESP_LOGE(TAG, "OTA via MSC failed: %s", esp_err_to_name(err));
        msc_ota_acquired = false;
        msc_ota_done = true;
        break;
    default:
        break;
    }
}

void dfu_msc_uf2_register(void)
{
    msc_ota_acquired = false;
    msc_ota_done = false;
    ESP_LOGI(TAG, "MSC UF2 transport ready");
}

void dfu_msc_uf2_write(const uint8_t *sector, size_t len)
{
    if (len != 512) return;

    /* After a terminal OTA result, ignore further sectors until remount
       (success reboots; failure shouldn't re-trigger the detect/abort spin). */
    if (msc_ota_done) return;

    if (memcmp(sector, "UF2\n", 4) != 0) return;

    if (!msc_ota_acquired) {
        esp_err_t err = dfu_core_acquire(DFU_SRC_MSC, msc_ota_event_cb, NULL);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "dfu_core busy (another source); ignoring UF2 sector");
            return;
        }
        msc_ota_acquired = true;
        ESP_LOGI(TAG, "UF2 drag-drop detected, forwarding to dfu_core");
    }

    esp_err_t err = dfu_core_feed(DFU_SRC_MSC, sector, 512);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "dfu_core_feed: %s", esp_err_to_name(err));
        dfu_core_release(DFU_SRC_MSC);
        msc_ota_acquired = false;
    }
}

void dfu_msc_uf2_reset(void)
{
    msc_ota_done = false;
}
