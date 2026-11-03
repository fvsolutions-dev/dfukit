#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Unified, transport-agnostic OTA writer.
//
// One internal worker task owns the esp_ota_* state. Callers (USB DFU, CDC,
// BLE, MSC, …) identify themselves by an dfu_source_t and stream bytes in;
// the worker auto-detects raw .bin vs UF2, writes to the inactive OTA slot,
// finalizes, and arms a deferred reboot.
//
// Concurrency: at most one source is active at a time. acquire() returns
// ESP_ERR_INVALID_STATE if a different source is already mid-flight.
//
// Per-feed completions surface via the optional callback registered at
// acquire(). The callback fires from the worker task after each feed,
// complete, or fatal error. Transports that need to ack the host (e.g.
// DFU's `tud_dfu_finish_flashing`) hook it here.

typedef enum {
    DFU_SRC_NONE = 0,
    DFU_SRC_USB_DFU,
    DFU_SRC_USB_CDC,
    DFU_SRC_BLE,
    DFU_SRC_MSC,
    DFU_SRC_USB_HID,
    DFU_SRC_UDP,
    DFU_SRC_HTTP,
} dfu_source_t;

typedef enum {
    DFU_EVT_FEED_OK,        // a feed chunk was written
    DFU_EVT_COMPLETE_OK,    // image finalized, reboot armed
    DFU_EVT_ERROR,          // fatal — session released, transport should ack-fail
} dfu_event_t;

typedef void (*dfu_core_cb_t)(dfu_source_t src, dfu_event_t evt, esp_err_t err, void *user);

// Maximum bytes per dfu_core_feed() call.  The session staging buffer
// (STAGING_BYTES in dfu_session.c) must equal this value: a full staging
// flush hands exactly one DFU_CORE_SCRATCH_BYTES chunk to dfu_core_feed.
#define DFU_CORE_SCRATCH_BYTES 4096

// Create the worker task and internal state. Call once from app_main.
esp_err_t dfu_core_init(void);

// Claim the OTA writer for this source. cb may be NULL.
//   ESP_OK                — acquired (or re-acquired by the same src)
//   ESP_ERR_INVALID_STATE — a different source already owns it
esp_err_t dfu_core_acquire(dfu_source_t src, dfu_core_cb_t cb, void *user);

// Copy bytes into the worker's pipeline. Format (raw vs UF2) is auto-
// detected on the very first feed of the session. Non-blocking from the
// caller's perspective (copies to a scratch slot and queues). The cb fires
// with DFU_EVT_FEED_OK or DFU_EVT_ERROR after the worker has processed
// this chunk.
//   ESP_OK                — chunk queued
//   ESP_ERR_INVALID_STATE — src doesn't own the OTA
//   ESP_ERR_TIMEOUT       — worker pipeline full (caller can retry)
esp_err_t dfu_core_feed(dfu_source_t src, const void *data, size_t len);

// End-of-stream marker. Worker finalizes (esp_ota_end + set_boot) and arms
// a reboot after reboot_delay_ms (use 0 to skip the reboot). The cb fires
// with DFU_EVT_COMPLETE_OK or DFU_EVT_ERROR.
esp_err_t dfu_core_complete(dfu_source_t src, uint32_t reboot_delay_ms);

// Abort the in-progress OTA (if any) and release ownership.
void dfu_core_release(dfu_source_t src);

bool dfu_core_in_progress(void);

// Optional hint from the transport layer: total expected image size in
// bytes. Used only for periodic UART status logs (see
// CONFIG_DFU_CORE_STATUS_LOG_PERIOD_MS) so a percent can be reported.
// Pass 0 to clear. Safe to call from any context.
void dfu_core_report_total(uint32_t total_bytes);

// Other targets than this chip's own OTA slot (target 0): a device behind this one, for
// example, updated through it. The worker hands each target the bytes as they arrive, in
// order (no format detection), and doesn't reboot this chip at the end. All calls run on the
// worker task; each may block (its pace is the feed's pace).
typedef struct {
    const char *name;
    uint16_t hw_id;          // what a session's START names it by (dfu_session)
    uint32_t max_size;       // the largest image it takes, in bytes
    // A new image of total bytes (0 when not known) begins
    esp_err_t (*begin)(void *ctx, uint32_t total);
    esp_err_t (*write)(void *ctx, const void *data, size_t len);
    // All of it was written: check it and switch to it
    esp_err_t (*finish)(void *ctx);
    // Broken off, after begin
    void (*abort)(void *ctx);
    void *ctx;
} dfu_core_target_t;

#define DFU_CORE_MAX_TARGETS 4

// Register target id (1 to DFU_CORE_MAX_TARGETS); the target must stay valid
esp_err_t dfu_core_register_target(uint8_t id, const dfu_core_target_t *target);

// The target the source writes to: after acquire, before the first feed. 0 is this chip's own
// OTA slot, the default
esp_err_t dfu_core_select_target(dfu_source_t src, uint8_t id);

// The registered target named by hw_id: its id, or 0 when there is none
uint8_t dfu_core_target_by_hw_id(uint16_t hw_id);

// A registered target, or NULL
const dfu_core_target_t *dfu_core_target(uint8_t id);

// Commit the running slot so it isn't rolled back on the next boot.
// Schedules a one-shot timer that fires after delay_ms, marks valid, and
// then queues a background pre-erase of the inactive slot — so the next
// OTA's esp_ota_begin doesn't block on a 1.5 MB sector-erase loop.
void dfu_core_mark_running_valid_after_ms(uint32_t delay_ms);

#ifdef __cplusplus
}
#endif
