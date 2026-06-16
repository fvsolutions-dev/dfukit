#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "dfu_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* DFU Session Protocol v1.1 — transport-agnostic implementation. Sits
   between dfu_core (the OTA writer) and a wire transport (BLE GATT,
   USB CDC, UDP, ...). Owns the session state machine, manifest +
   SHA-256 verification, sliding-window flow control, the staging
   buffer, the pending-feed ring, and the idle/finalize timers.

   The protocol bytes are defined in DFU-session.md. This header
   exposes only the C API; transports talk to the session via the
   vtable below and never need to know the wire format.

   Concurrency: the session has one internal mutex. The mutex is
   NEVER held across a call to transport->send() or to
   dfu_core_feed(), preserving these invariants:

     - transport->send() may take its own locks (e.g. NimBLE host
       lock) without inversion.
     - dfu_core's worker may call back into core_cb (which takes the
       session mutex) without deadlocking against an in-flight feed.

   Finalisation (esp_ota_end + set_boot) is dispatched onto the
   esp_timer task — never inline in the dfu_core callback — because
   dfu_core_complete() posts to the worker's own queue with
   portMAX_DELAY. */

typedef struct dfu_session dfu_session_t;

/* Transport vtable. All callbacks are invoked from session contexts
   with the session mutex RELEASED. send() may be called from the
   transport's own task, the dfu_core worker task, or an esp_timer
   task — implementations must be thread-safe. */
typedef struct {
    /* Push one protocol frame to the connected peer. Return 0 on
       success, negative on error. The session never calls this with
       a buffer larger than max_payload(ctx). */
    int    (*send)(void *ctx, const void *buf, size_t len);

    /* Current outbound MTU. Read at START time to advertise the
       per-DATA payload limit to the host. Transports with no real
       MTU (UART/UDP) return a static cap. */
    size_t (*max_payload)(void *ctx);

    /* Optional. Fires on entry to TRANSFER (i.e. a successful
       START acquire). BLE uses it to tighten connection params;
       UART/UDP can leave it NULL. */
    void   (*on_session_start)(void *ctx);

    /* Optional. Fires when the session has decided to tear down
       (idle timeout, ABORT, fatal core error). Lets the transport
       drop the peer / signal end. May be NULL. */
    void   (*on_release)(void *ctx);

    void          *ctx;          /* transport-private state */
    dfu_source_t   source_id;    /* DFU_SRC_BLE / _USB_CDC / _UDP */
    const char    *name;         /* diagnostic label, e.g. "ble" */
} dfu_session_transport_t;

typedef struct {
    uint32_t window_bytes;       /* sliding-window byte budget         */
    uint32_t idle_timeout_ms;    /* abort a stalled TRANSFER           */
    uint16_t hw_id;              /* matched against the START manifest */
} dfu_session_config_t;

/* Lifecycle. The session pointer is heap-allocated by init; the
   transport vtable + config are copied into the session, so the
   caller may use stack storage for both. */
esp_err_t dfu_session_init(dfu_session_t **out,
                           const dfu_session_transport_t *tx,
                           const dfu_session_config_t *cfg);

void      dfu_session_deinit(dfu_session_t *s);

/* Transport -> session, one call per received frame. The first byte
   is the opcode; the session does its own state checks, dispatch,
   dfu_core feeding, and response generation. Returns ESP_OK if the
   frame was consumed (including the case where the session sent a
   protocol-level error response). */
esp_err_t dfu_session_feed_frame(dfu_session_t *s,
                                 const void *buf, size_t len);

/* Transport tells the session the peer disappeared (BLE disconnect,
   UDP idle, CDC line down). Aborts an in-flight session
   idempotently. */
void      dfu_session_peer_gone(dfu_session_t *s);

bool      dfu_session_in_progress(const dfu_session_t *s);

#ifdef __cplusplus
}
#endif
