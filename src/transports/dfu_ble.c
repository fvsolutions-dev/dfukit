/* BLE transport adapter for dfu_session.

   The wire protocol (DFU Session Protocol v1.1) and all its state
   lives in dfu_session.{h,c} under lib/esp-idf-common/dfu_core/. This
   file owns only the BLE-specific glue:

     - one primary GATT service with a single characteristic
       (WRITE | WRITE_NO_RSP | NOTIFY, optionally WRITE_ENC),
     - the active connection handle + characteristic value handle,
     - a NimBLE notify implementation of dfu_session_transport_t.send,
     - a GAP listener that calls dfu_session_peer_gone() on disconnect,
     - the BLE-only "tighten connection params on START" hook. */

#include "dfu_ble.h"

#include <inttypes.h>
#include <string.h>

#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_err.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "os/os_mbuf.h"

#include "dfu_session.h"

static const char *TAG = "dfu_ble";

/* 16-bit GATT UUIDs in the project-local 0xEE00 range. Default 0xEEE1
   for the service and 0xEEE2 for the characteristic; both
   Kconfig-tunable so a build can pick different values without code
   edits. Adjacent to the management service (0xEEE0). */
static const ble_uuid16_t svc_uuid = BLE_UUID16_INIT(CONFIG_DFU_TRANSPORT_BLE_SVC_UUID16);
static const ble_uuid16_t chr_uuid = BLE_UUID16_INIT(CONFIG_DFU_TRANSPORT_BLE_CHR_UUID16);

/* BLE-transport-private state. Only this file touches it. The session
   state is owned by dfu_session.c — no shared mutex needed across
   the boundary (transport calls session, session calls transport's
   vtable; neither holds across the call). */
static SemaphoreHandle_t   tx_mtx;
static dfu_session_t      *session;
static uint16_t            conn_handle    = BLE_HS_CONN_HANDLE_NONE;
static uint16_t            ota_val_handle;

static struct ble_gap_event_listener gap_listener;

/* --- transport vtable ---------------------------------------------- */

static int ble_send(void *ctx, const void *buf, size_t len)
{
    (void)ctx;
    uint16_t conn;
    xSemaphoreTake(tx_mtx, portMAX_DELAY);
    conn = conn_handle;
    xSemaphoreGive(tx_mtx);
    if (conn == BLE_HS_CONN_HANDLE_NONE) return -1;

    struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, (uint16_t)len);
    if (!om) return -2;
    int rc = ble_gatts_notify_custom(conn, ota_val_handle, om);
    if (rc != 0) ESP_LOGW(TAG, "notify rc=%d", rc);
    return rc == 0 ? 0 : -3;
}

static size_t ble_max_payload(void *ctx)
{
    (void)ctx;
    uint16_t conn;
    xSemaphoreTake(tx_mtx, portMAX_DELAY);
    conn = conn_handle;
    xSemaphoreGive(tx_mtx);
    uint16_t mtu = (conn != BLE_HS_CONN_HANDLE_NONE) ? ble_att_mtu(conn) : 23;
    if (mtu < 23) mtu = 23;
    /* Reserve 3 bytes for ATT notify header. The session subtracts
       its own protocol header. */
    return (size_t)(mtu - 3);
}

static void ble_on_session_start(void *ctx)
{
    (void)ctx;
    uint16_t conn;
    xSemaphoreTake(tx_mtx, portMAX_DELAY);
    conn = conn_handle;
    xSemaphoreGive(tx_mtx);
    if (conn == BLE_HS_CONN_HANDLE_NONE) return;

    /* Tighten connection params for the transfer. Linux/Windows
       centrals don't request this themselves. 12 * 1.25 ms = 15 ms.
       Supervision is deliberately long (15 s): an OTA rides through
       brief RF dropouts via LL retransmission instead of dying with
       reason 0x08 — observed killing transfers at ~225 KB when the
       2.4 GHz band got noisy. */
    struct ble_gap_upd_params up = {
        .itvl_min = 12, .itvl_max = 12, .latency = 0,
        .supervision_timeout = 1500,  /* 15 s, 10 ms units */
        .min_ce_len = 0, .max_ce_len = 0,
    };
    ble_gap_update_params(conn, &up);
}

/* Drop the BLE link on session teardown so the central can't pin us
   in CONNECTED forever after a stalled / aborted transfer. Without
   this, an idle-timeout fires inside dfu_session but the link stays
   up (the host never sent anything to provoke a disconnect) — the
   device then doesn't re-enter advertising and goes invisible until
   supervision timeout finally drops the link minutes later. The
   DISCONNECT event handler in ble_advertising.c already restarts
   the primary instance, so we just need to terminate. */
static void ble_on_release(void *ctx)
{
    (void)ctx;
    uint16_t conn;
    xSemaphoreTake(tx_mtx, portMAX_DELAY);
    conn = conn_handle;
    xSemaphoreGive(tx_mtx);
    if (conn == BLE_HS_CONN_HANDLE_NONE) return;
    int rc = ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
    if (rc != 0 && rc != BLE_HS_ENOTCONN) {
        ESP_LOGW(TAG, "terminate rc=%d", rc);
    }
}

/* --- GATT plumbing ------------------------------------------------- */

static int ota_chr_access(uint16_t conn, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr_handle; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;

    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len < 1 || len > 517) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    static uint8_t buf[517];
    os_mbuf_copydata(ctxt->om, 0, len, buf);

    xSemaphoreTake(tx_mtx, portMAX_DELAY);
    conn_handle = conn;
    xSemaphoreGive(tx_mtx);

    /* Hand the frame to the session state machine. */
    static uint32_t rx_count;
    esp_err_t err = dfu_session_feed_frame(session, buf, len);
    if (rx_count == 0 || (rx_count % 100) == 0) {
        ESP_LOGD(TAG, "rx #%" PRIu32 " len=%u op=0x%02x rc=%d",
                 rx_count, (unsigned)len, buf[0], (int)err);
    }
    rx_count++;
    return 0;
}

static const struct ble_gatt_svc_def svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid       = &chr_uuid.u,
                .access_cb  = ota_chr_access,
                .val_handle = &ota_val_handle,
                .flags      = BLE_GATT_CHR_F_WRITE
                            | BLE_GATT_CHR_F_WRITE_NO_RSP
                            | BLE_GATT_CHR_F_NOTIFY
#if CONFIG_DFU_TRANSPORT_BLE_REQUIRE_ENC
                            | BLE_GATT_CHR_F_WRITE_ENC
#endif
                ,
            },
            { 0 },
        },
    },
    { 0 },
};

static int gap_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    if (event->type == BLE_GAP_EVENT_DISCONNECT) {
        xSemaphoreTake(tx_mtx, portMAX_DELAY);
        bool ours = (event->disconnect.conn.conn_handle == conn_handle);
        if (ours) conn_handle = BLE_HS_CONN_HANDLE_NONE;
        xSemaphoreGive(tx_mtx);
        if (ours) dfu_session_peer_gone(session);
    }
    return 0;
}

/* --- public API ---------------------------------------------------- */

bool dfu_ble_in_progress(void)
{
    return dfu_session_in_progress(session);
}

esp_err_t dfu_ble_register(void)
{
    if (session) return ESP_OK;
    tx_mtx = xSemaphoreCreateMutex();
    if (!tx_mtx) return ESP_ERR_NO_MEM;

    const dfu_session_transport_t tx = {
        .send             = ble_send,
        .max_payload      = ble_max_payload,
        .on_session_start = ble_on_session_start,
        .on_release       = ble_on_release,
        .ctx              = NULL,
        .source_id        = DFU_SRC_BLE,
        .name             = "ble",
    };
    const dfu_session_config_t cfg = {
        .window_bytes     = CONFIG_DFU_SESSION_WINDOW_BYTES,
        .idle_timeout_ms  = CONFIG_DFU_SESSION_IDLE_TIMEOUT_MS,
        .hw_id            = CONFIG_DFU_TRANSPORT_HW_ID,
    };
    esp_err_t err = dfu_session_init(&session, &tx, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "dfu_session_init failed: %s", esp_err_to_name(err));
        return err;
    }

    int rc = ble_gatts_count_cfg(svcs);
    if (rc == 0) rc = ble_gatts_add_svcs(svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "gatts registration rc=%d", rc);
        return ESP_FAIL;
    }
    ble_gap_event_listener_register(&gap_listener, gap_cb, NULL);
    ESP_LOGI(TAG, "registered (svc 0x%04X chr 0x%04X val_handle=%u)",
             (unsigned)CONFIG_DFU_TRANSPORT_BLE_SVC_UUID16,
             (unsigned)CONFIG_DFU_TRANSPORT_BLE_CHR_UUID16,
             (unsigned)ota_val_handle);
    return ESP_OK;
}
