#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// BLE transport adapter for the transport-agnostic dfu_session
// (which implements DFU Session Protocol v1.1).
//
// Layering:
//   - dfu_core owns the flash pipeline (worker task, raw/UF2 detect,
//     esp_ota_*).
//   - dfu_session owns the protocol state machine, manifest +
//     SHA-256 verification, sliding-window PROGRESS, idle watchdog.
//     See DFU-session.md.
//   - This adapter owns only the BLE-specific glue: one primary
//     GATT service with one characteristic (WWR down, Notify up),
//     active conn-handle tracking, the NimBLE notify implementation
//     of the dfu_session_transport_t.send vtable entry, and the GAP
//     disconnect listener that calls dfu_session_peer_gone().
//
// Integration:
//   1. dfu_core_init() from app_main as usual.
//   2. Call dfu_ble_register() during NimBLE GATT server setup —
//      after ble_svc_gap_init() / ble_svc_gatt_init() and BEFORE the
//      host task starts (nimble_port_freertos_init / ble_gatts_start).
//   3. If you want phones to scan-filter on the OTA UUID, add
//      CONFIG_DFU_TRANSPORT_BLE_SVC_UUID16 to the AD payload.
//
// GATT UUIDs (16-bit, project-local 0xEE00 range):
//   Service UUID: CONFIG_DFU_TRANSPORT_BLE_SVC_UUID16 (default 0xEEE1).
//   OTA char  UUID: CONFIG_DFU_TRANSPORT_BLE_CHR_UUID16 (default 0xEEE2).

esp_err_t dfu_ble_register(void);

// True while a BLE OTA session is in TRANSFER/FINISHING/READY.
bool dfu_ble_in_progress(void);

#ifdef __cplusplus
}
#endif
