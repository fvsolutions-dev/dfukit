#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// UDP transport adapter for dfu_session (DFU Session Protocol v1.1).
//
// Layering (mirrors dfu_ble.h / dfu_hid.h):
//   - dfu_core     — flash pipeline (worker task, raw/UF2 auto-detect).
//   - dfu_session  — protocol state machine, manifest + SHA-256,
//                    sliding-window PROGRESS, idle watchdog.
//   - This file    — UDP socket, vtable wiring.
//
// Integration: call dfu_udp_start() from app_main after dfu_core_init()
// and AFTER the IP stack is up (Wi-Fi/Ethernet connected). The caller is
// responsible for bringing up the network before calling this function.
// The UDP port is set via Kconfig (CONFIG_DFU_TRANSPORT_UDP_PORT).
//
// Wire framing: none — UDP preserves message boundaries, so each
// datagram carries exactly one protocol frame. No length prefix.

esp_err_t dfu_udp_start(void);

#ifdef __cplusplus
}
#endif
