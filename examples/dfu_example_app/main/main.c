/* dfu_example_app — standalone example app for the dfu_core component.
 *
 * Exercises all four DFU transports (BLE, USB-HID, USB-MSC UF2, UDP) on
 * the smallest firmware that actually compiles and runs — no application
 * logic or peripherals beyond what each transport needs. A persistent
 * boot counter is written to NVS so a successful flash is visually
 * verifiable: counter N → N+1 over each transport.
 *
 * The boot sequence (app_main) reads top-to-bottom; the noisy bits live
 * in their own files: usb_composite.c (TinyUSB CDC/MSC/HID/DFU) and
 * ble_app.c (NimBLE advertiser).
 *
 * Transport prerequisites:
 *   BLE   — NimBLE comes up in ble_app; no pairing required.
 *   HID   — USB composite enumerates; use dfukit or the pydfu scripts.
 *   MSC   — Drag-drop a .uf2 onto the drive that appears on the host.
 *   DFU   — dfu-util -d 303a:4003 -D dfu_example_app.bin -R
 *   UDP   — Wi-Fi STA connects on boot; dfukit or pydfu target the IP.
 */

#include <inttypes.h>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "dfu_core.h"
#include "usb_composite.h"
#if CONFIG_DFU_TRANSPORT_HID
#include "dfu_hid.h"
#endif
#if CONFIG_DFU_TRANSPORT_MSC
#include "dfu_msc_uf2.h"
#endif
#if CONFIG_DFU_TRANSPORT_BLE
#include "ble_app.h"
#endif
#if CONFIG_DFU_TRANSPORT_UDP
#include "wifi_sta.h"
#include "dfu_udp.h"
#endif

static const char *TAG = "dfu_example";

static uint32_t bump_boot_counter(void);

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    uint32_t counter = bump_boot_counter();
    ESP_LOGI(TAG, "build=" __DATE__ " " __TIME__ " counter=%" PRIu32, counter);

    ESP_ERROR_CHECK(dfu_core_init());

#if CONFIG_DFU_TRANSPORT_MSC
    dfu_msc_uf2_register();
#endif
#if CONFIG_DFU_TRANSPORT_HID
    ESP_ERROR_CHECK(dfu_hid_register());
#endif

    ESP_ERROR_CHECK(usb_start());

#if CONFIG_DFU_TRANSPORT_BLE
    ESP_ERROR_CHECK(ble_start());
#endif

#if CONFIG_DFU_TRANSPORT_UDP
    ESP_ERROR_CHECK(wifi_connect_blocking());
    ESP_ERROR_CHECK(dfu_udp_start());
#endif

    dfu_core_mark_running_valid_after_ms(CONFIG_DFU_CORE_MARK_VALID_DELAY_MS);

    ESP_LOGI(TAG, "ready — BLE / USB-HID / USB-MSC UF2 / USB-DFU / UDP");

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        ESP_LOGI(TAG, "alive counter=%" PRIu32, counter);
    }
}

// ---------------------------------------------------------------------------
// Boot counter — proof of a successful flash (counter N → N+1).

static uint32_t bump_boot_counter(void)
{
    nvs_handle_t h;
    uint32_t n = 0;
    if (nvs_open("dfu_example", NVS_READWRITE, &h) == ESP_OK) {
        nvs_get_u32(h, "cnt", &n);
        n++;
        nvs_set_u32(h, "cnt", n);
        nvs_commit(h);
        nvs_close(h);
    }
    return n;
}
