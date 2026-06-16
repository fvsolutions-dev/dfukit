/* ble_app — minimal NimBLE advertiser for the dfu_core example.
 *
 * Just enough BLE to make the dfu_core OTA GATT service scannable: a
 * legacy connectable advertisement carrying the OTA service UUID and a
 * device name, with re-advertise on disconnect. No management service,
 * no pairing, no extended advertising. dfu_ble_register() adds the OTA
 * characteristic; the protocol logic lives in the component.
 */

#include "ble_app.h"

#include "sdkconfig.h"

#if CONFIG_DFU_TRANSPORT_BLE

#include <string.h>

#include "esp_log.h"

#include "dfu_ble.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "os/os_mbuf.h"

static const char *TAG = "ble_app";

static void on_ble_sync(void);
static void on_ble_reset(int reason);
static void ble_host_task(void *param);
static int  gap_event_cb(struct ble_gap_event *event, void *arg);

esp_err_t ble_start(void)
{
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) { ESP_LOGE(TAG, "nimble_port_init: %d", err); return err; }

    ble_hs_cfg.sync_cb  = on_ble_sync;
    ble_hs_cfg.reset_cb = on_ble_reset;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set("dfu_example");

    err = dfu_ble_register();
    if (err != ESP_OK) { ESP_LOGE(TAG, "dfu_ble_register: %d", err); return err; }

    nimble_port_freertos_init(ble_host_task);
    return ESP_OK;
}

static void start_advertising(void)
{
    struct ble_gap_adv_params adv_params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
        .itvl_min  = BLE_GAP_ADV_ITVL_MS(500),
        .itvl_max  = BLE_GAP_ADV_ITVL_MS(500),
    };
    int rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                               &adv_params, gap_event_cb, NULL);
    if (rc != 0) ESP_LOGE(TAG, "adv_start rc=%d", rc);
}

static void on_ble_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble ensure_addr rc=%d", rc);
        return;
    }

    // Legacy ADV_IND carrying the DFU service UUID so scan-filters work.
    static const ble_uuid16_t ota_svc_uuid =
        BLE_UUID16_INIT(CONFIG_DFU_TRANSPORT_BLE_SVC_UUID16);

    struct os_mbuf *mbuf = os_msys_get_pkthdr(BLE_HCI_MAX_ADV_DATA_LEN, 0);
    if (!mbuf) { ESP_LOGE(TAG, "adv mbuf alloc failed"); return; }

    const char *name = ble_svc_gap_device_name();
    rc = ble_hs_adv_set_fields_mbuf(
        &(struct ble_hs_adv_fields){
            .flags               = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP,
            .uuids16             = (ble_uuid16_t *)&ota_svc_uuid,
            .num_uuids16         = 1,
            .uuids16_is_complete = 1,
            .name                = (uint8_t *)name,
            .name_len            = (uint8_t)strlen(name),
            .name_is_complete    = 1,
        },
        mbuf);
    if (rc != 0) { os_mbuf_free_chain(mbuf); ESP_LOGE(TAG, "adv fields rc=%d", rc); return; }

    uint8_t adv_data[BLE_HCI_MAX_ADV_DATA_LEN];
    uint16_t adv_len = OS_MBUF_PKTLEN(mbuf);
    if (adv_len > sizeof(adv_data)) adv_len = sizeof(adv_data);
    os_mbuf_copydata(mbuf, 0, adv_len, adv_data);
    os_mbuf_free_chain(mbuf);

    rc = ble_gap_adv_set_data(adv_data, (uint8_t)adv_len);
    if (rc != 0) { ESP_LOGE(TAG, "adv_set_data rc=%d", rc); return; }

    start_advertising();
    ESP_LOGI(TAG, "BLE advertising started");
}

static void on_ble_reset(int reason)
{
    ESP_LOGW(TAG, "BLE reset reason=%d", reason);
}

static void ble_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        ESP_LOGI(TAG, "BLE connect status=%d", event->connect.status);
        if (event->connect.status != 0) start_advertising();  // re-arm on fail
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "BLE disconnect reason=%d", event->disconnect.reason);
        start_advertising();
        break;
    default:
        break;
    }
    return 0;
}

#endif /* CONFIG_DFU_TRANSPORT_BLE */
