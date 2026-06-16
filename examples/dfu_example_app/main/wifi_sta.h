#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Connect to the AP configured by CONFIG_DFU_EXAMPLE_WIFI_SSID /
// CONFIG_DFU_EXAMPLE_WIFI_PASS and block until an IP is assigned.
esp_err_t wifi_connect_blocking(void);

#ifdef __cplusplus
}
#endif
