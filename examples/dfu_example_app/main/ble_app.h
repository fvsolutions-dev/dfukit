#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bring up NimBLE as a minimal connectable advertiser that makes the
 * dfu_core BLE GATT service scannable. Initialises the host, registers
 * the OTA service (dfu_ble_register), and starts the NimBLE task. Only
 * compiled when CONFIG_DFU_TRANSPORT_BLE is set. */
esp_err_t ble_start(void);

#ifdef __cplusplus
}
#endif
