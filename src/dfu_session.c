/* DFU Session Protocol v1.1 — transport-agnostic core.

   This file is a direct extraction of the protocol logic that
   originally lived in main/ble/dfu_ble.c. The wire format is
   unchanged; only the I/O boundary has moved (now via the
   dfu_session_transport_t vtable).

   The three integration invariants from dfu_ble.c carry over verbatim:

     1. The session mutex is never held across dfu_core_feed() — the
        worker can block on its own slot_sem while it's blocked inside
        core_cb taking the same mutex → deadlock.

     2. Finalisation (the esp_ota_end + set_boot path) runs on the
        esp_timer task via finalize_timer. Calling dfu_core_complete()
        from inside core_cb risks a queue-full deadlock since the call
        posts to the worker's own queue with portMAX_DELAY.

     3. Transport->send() callbacks fire with the mutex RELEASED so
        transport-side locks (NimBLE host lock, etc.) can be taken
        without inversion.

   See DFU-session.md for the wire spec. */

#include "dfu_session.h"
#include "dfu_session_wire.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"

/* IDF 6.0 ships TF-PSA Mbed TLS 4, which moved mbedtls/sha256.h to
   mbedtls/private/sha256.h and gated the legacy mbedtls_sha256_*
   API behind MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS. Opting back into
   the legacy surface keeps this file v3-style; a future cleanup can
   migrate to the PSA Crypto API. */
#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS
#include "mbedtls/private/sha256.h"

static const char *TAG = "dfu_session";

/* --------------------------------------------------------------------- */
/* Protocol constants (from dfu_session_wire.h; local shorthands)        */
/* --------------------------------------------------------------------- */

#define PROTO_VER         1
#define START_PAYLOAD_LEN sizeof(dfu_wire_start_req_t)   /* 48 B */

/* Must equal dfu_core's per-feed scratch size (DFU_CORE_SCRATCH_BYTES = 4096).
 * Both sides share the same physical constraint: a flush hands exactly one
 * scratch-sized chunk to dfu_core_feed. Changing either value without
 * changing the other breaks flow-control accounting. */
#define STAGING_BYTES     DFU_CORE_SCRATCH_BYTES

/* Pending-feed ring. Worst case in flight: every scratch slot queued
   + one being written. Keep a healthy margin; entries are 8 bytes. */
#define PEND_RING         16

/* Session states mirror the on-wire state enum for trivial cast-to-uint8_t
   in responses. Values must match dfu_wire_state_t in dfu_session_wire.h. */
typedef enum {
    ST_IDLE      = DFU_WIRE_ST_IDLE,
    ST_TRANSFER  = DFU_WIRE_ST_TRANSFER,
    ST_FINISHING = DFU_WIRE_ST_FINISHING,
    ST_READY     = DFU_WIRE_ST_READY,
    ST_SUSPENDED = DFU_WIRE_ST_SUSPENDED,
} session_state_t;

/* How long a suspended session waits for the peer to reconnect and
   resume before the OTA writer is released and the partial image is
   abandoned. See Kconfig DFU_SESSION_RESUME_GRACE_MS. */
#define RESUME_GRACE_MS CONFIG_DFU_SESSION_RESUME_GRACE_MS

/* ---------------------------------------------------------------------
   Session state. mtx guards everything below it. Never hold mtx
   across transport->send() or dfu_core_feed() (see file header).
   ------------------------------------------------------------------- */

struct dfu_session {
    dfu_session_transport_t tx;
    dfu_session_config_t    cfg;

    SemaphoreHandle_t       mtx;
    session_state_t         st;

    uint32_t                image_size;
    uint8_t                 sha_expected[32];
    mbedtls_sha256_context  sha_ctx;
    bool                    sha_ctx_live;

    uint32_t                next_off;       /* next expected wire offset */
    uint32_t                acked;          /* bytes confirmed written   */
    uint32_t                last_prog_ack;  /* acked at last PROGRESS    */
    bool                    gap_signalled;  /* OFFSET_GAP sent for the   */
    uint32_t                gap_off;        /*   current next_off value  */
    uint32_t                crc_fed;        /* CRC32 over fed bytes      */
    uint32_t                crc_acked;      /* CRC32 at acked boundary   */

    uint8_t                 staging[STAGING_BYTES];
    uint32_t                staged;

    struct { uint16_t len; uint32_t crc; } pend[PEND_RING];
    uint8_t                 pend_rd, pend_wr, pend_cnt;

    bool                    core_completed; /* DFU_EVT_COMPLETE_OK seen */
    bool                    finish_rsp_due; /* FINISH response pending  */

    esp_timer_handle_t      idle_timer;
    esp_timer_handle_t      finalize_timer;
    esp_timer_handle_t      reboot_timer;
};

/* --------------------------------------------------------------------- */
/* Forward declarations                                                   */
/* --------------------------------------------------------------------- */

static int      session_send(dfu_session_t *s, const void *buf, uint16_t len);
static void     notify_rsp(dfu_session_t *s, uint8_t opcode, uint8_t status,
                            const void *extra, uint16_t extra_len);
static void     notify_progress(dfu_session_t *s, uint8_t status);

static void     reset_session_locked(dfu_session_t *s);
static void     abort_session(dfu_session_t *s, uint8_t status_for_client);
static esp_err_t flush_staging(dfu_session_t *s);

static void     handle_start(dfu_session_t *s, const uint8_t *pl, uint16_t len);
static void     handle_data(dfu_session_t *s, const uint8_t *buf, uint16_t len);
static void     handle_finish(dfu_session_t *s);
static void     handle_apply(dfu_session_t *s, const uint8_t *pl, uint16_t len);
static void     handle_confirm(dfu_session_t *s);
static void     handle_get_state(dfu_session_t *s);

static void     core_cb(dfu_source_t src, dfu_event_t evt, esp_err_t err, void *user);

static void     idle_cb(void *arg);
static void     finalize_cb(void *arg);
static void     reboot_cb(void *arg);

/* --------------------------------------------------------------------- */
/* Public API — lifecycle order: init … in_progress … feed … peer_gone … deinit
   ------------------------------------------------------------------- */

esp_err_t dfu_session_init(dfu_session_t **out,
                           const dfu_session_transport_t *tx,
                           const dfu_session_config_t *cfg)
{
    if (!out || !tx || !cfg) return ESP_ERR_INVALID_ARG;
    if (!tx->send || !tx->max_payload) return ESP_ERR_INVALID_ARG;

    dfu_session_t *s = calloc(1, sizeof(*s));
    if (!s) return ESP_ERR_NO_MEM;
    s->tx  = *tx;
    s->cfg = *cfg;

    /* The advertised window MUST exceed the staging buffer, or the
       protocol deadlocks: the host exhausts its send-credit one frame
       before staging fills, staging never flushes to dfu_core, no
       bytes are acked, PROGRESS never fires, and both sides wait on
       each other until the idle timeout. (Seen live: window == staging
       == 4096 stalls at exactly frame 17 with 243-byte chunks.) Clamp
       to 2x staging so a full staging flush always happens with host
       credit to spare. */
    if (s->cfg.window_bytes <= STAGING_BYTES) {
        ESP_LOGW(TAG, "[%s] window %u <= staging %u would deadlock; clamping to %u",
                 s->tx.name, (unsigned)s->cfg.window_bytes,
                 (unsigned)STAGING_BYTES, (unsigned)(2 * STAGING_BYTES));
        s->cfg.window_bytes = 2 * STAGING_BYTES;
    }

    s->mtx = xSemaphoreCreateMutex();
    if (!s->mtx) { free(s); return ESP_ERR_NO_MEM; }

    const esp_timer_create_args_t idle_args = { .callback = idle_cb,     .arg = s, .name = "dfu_session_idle" };
    const esp_timer_create_args_t fin_args  = { .callback = finalize_cb, .arg = s, .name = "dfu_session_fin"  };
    const esp_timer_create_args_t rb_args   = { .callback = reboot_cb,   .arg = s, .name = "dfu_session_rb"   };
    if (esp_timer_create(&idle_args, &s->idle_timer)     != ESP_OK ||
        esp_timer_create(&fin_args,  &s->finalize_timer) != ESP_OK ||
        esp_timer_create(&rb_args,   &s->reboot_timer)   != ESP_OK) {
        dfu_session_deinit(s);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "[%s] initialised (window %u B, staging %u B, hw_id 0x%04x)",
             s->tx.name, (unsigned)s->cfg.window_bytes,
             (unsigned)STAGING_BYTES, (unsigned)s->cfg.hw_id);
    *out = s;
    return ESP_OK;
}

void dfu_session_deinit(dfu_session_t *s)
{
    if (!s) return;
    if (s->idle_timer)     { esp_timer_stop(s->idle_timer);     esp_timer_delete(s->idle_timer);     }
    if (s->finalize_timer) { esp_timer_stop(s->finalize_timer); esp_timer_delete(s->finalize_timer); }
    if (s->reboot_timer)   { esp_timer_stop(s->reboot_timer);   esp_timer_delete(s->reboot_timer);   }
    if (s->mtx) vSemaphoreDelete(s->mtx);
    free(s);
}

bool dfu_session_in_progress(const dfu_session_t *s)
{
    if (!s || !s->mtx) return false;
    /* Drop the const for the take/give; the read is otherwise pure. */
    dfu_session_t *m = (dfu_session_t *)s;
    xSemaphoreTake(m->mtx, portMAX_DELAY);
    bool live = (s->st != ST_IDLE);
    xSemaphoreGive(m->mtx);
    return live;
}

esp_err_t dfu_session_feed_frame(dfu_session_t *s,
                                 const void *buf, size_t len)
{
    if (!s || !buf || len == 0) return ESP_ERR_INVALID_ARG;
    const uint8_t *b = (const uint8_t *)buf;
    switch (b[0]) {
    case OP_DATA:      handle_data(s, b, (uint16_t)len);                 break;
    case OP_START:     handle_start(s, b + 1, (uint16_t)(len - 1));      break;
    case OP_FINISH:    handle_finish(s);                                 break;
    case OP_APPLY:     handle_apply(s, b + 1, (uint16_t)(len - 1));      break;
    case OP_CONFIRM:   handle_confirm(s);                                break;
    case OP_ABORT:
        abort_session(s, STS_OK);
        notify_rsp(s, OP_ABORT, (uint8_t)STS_OK, NULL, 0);
        break;
    case OP_GET_STATE: handle_get_state(s);                              break;
    default:
        notify_rsp(s, b[0], (uint8_t)STS_INVALID_STATE, NULL, 0);
        break;
    }
    return ESP_OK;
}

void dfu_session_peer_gone(dfu_session_t *s)
{
    if (!s) return;
    xSemaphoreTake(s->mtx, portMAX_DELAY);
    if (s->st == ST_TRANSFER) {
        /* Suspend instead of aborting: keep dfu_core acquired, keep
           the SHA/CRC/ack state, and rewind the receive cursor to the
           last flush boundary (staging holds received-but-unhashed
           bytes — the host re-sends them after resume). A new START
           with the identical manifest within RESUME_GRACE_MS picks up
           at resume_off = next_off; anything else aborts cleanly. */
        s->next_off -= s->staged;
        s->staged = 0;
        s->gap_signalled = false;
        s->st = ST_SUSPENDED;
        uint32_t at = s->next_off;
        xSemaphoreGive(s->mtx);
        ESP_LOGW(TAG, "[%s] peer gone mid-transfer; holding for resume at %" PRIu32 " B (%u ms grace)",
                 s->tx.name, at, (unsigned)RESUME_GRACE_MS);
        esp_timer_stop(s->idle_timer);
        esp_timer_start_once(s->idle_timer, (uint64_t)RESUME_GRACE_MS * 1000);
        return;
    }
    bool finishing = (s->st == ST_FINISHING);
    xSemaphoreGive(s->mtx);
    if (finishing) {
        /* Finalisation needs no peer: the SHA check and esp_ota_end
           run to completion on their own and the session parks in
           READY (the FINISH response just fails to send — logged).
           The reconnecting client probes GET_STATE, sees READY, and
           goes straight to APPLY instead of re-transferring. */
        ESP_LOGW(TAG, "[%s] peer gone while finishing; continuing to READY",
                 s->tx.name);
    }
}

/* --------------------------------------------------------------------- */
/* Command/flow handlers — protocol order: START, DATA, FINISH, APPLY,
   CONFIRM, GET_STATE
   ------------------------------------------------------------------- */

static void handle_start(dfu_session_t *s, const uint8_t *pl, uint16_t len)
{
    /* Every reject below logs its reason at WARN. A START that fails is the
       one frame the host can do nothing about, and "the client just said
       BUSY / INVALID_STATE" is unreadable without the device's side of it. */
    if (len < START_PAYLOAD_LEN) {
        ESP_LOGW(TAG, "[%s] START payload %u B < %u B expected",
                 s->tx.name, (unsigned)len, (unsigned)START_PAYLOAD_LEN);
        notify_rsp(s, OP_START, STS_INVALID_STATE, NULL, 0);
        return;
    }
    /* Unpack via the canonical wire struct so field offsets stay in sync
       with the header's _Static_asserts. */
    dfu_wire_start_req_t req;
    memcpy(&req, pl, sizeof(req));
    (void)req.fw_ver; (void)req.min_fw;  /* version gating hook */

    if (req.proto != PROTO_VER) {
        ESP_LOGW(TAG, "[%s] START proto %u != %u", s->tx.name,
                 (unsigned)req.proto, (unsigned)PROTO_VER);
        notify_rsp(s, OP_START, STS_INVALID_STATE, NULL, 0);
        return;
    }
    if (req.hw_id != s->cfg.hw_id) {
        ESP_LOGW(TAG, "[%s] START hw_id 0x%04x != ours 0x%04x", s->tx.name,
                 (unsigned)req.hw_id, (unsigned)s->cfg.hw_id);
        notify_rsp(s, OP_START, STS_HW_MISMATCH, NULL, 0);
        return;
    }

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    /* UF2 containers are ~8% bigger than the raw image. 2x the slot
       is a safe sanity cap. */
    if (!part || req.size == 0 || req.size > part->size * 2) {
        ESP_LOGW(TAG, "[%s] START size %" PRIu32 " B rejected (update slot %" PRIu32 " B%s)",
                 s->tx.name, req.size, part ? (uint32_t)part->size : 0,
                 part ? "" : ", none found — single-app partition table?");
        notify_rsp(s, OP_START, STS_TOO_LARGE, NULL, 0);
        return;
    }
    uint32_t size = req.size;

    /* Resume: a suspended session (peer vanished mid-transfer) plus a
       START carrying the identical manifest continues where the flush
       boundary left off. SHA/CRC/ack state and the dfu_core acquire
       all survived the suspend; just rearm and tell the host where to
       pick up. */
    xSemaphoreTake(s->mtx, portMAX_DELAY);
    bool resume_match = (s->st == ST_SUSPENDED) &&
                        (s->image_size == size) &&
                        (memcmp(s->sha_expected, req.sha256, 32) == 0);
    uint32_t resume_off = 0;
    if (resume_match) {
        s->st = ST_TRANSFER;
        resume_off = s->next_off;
    }
    xSemaphoreGive(s->mtx);

    if (resume_match) {
        dfu_core_report_total(size);
        if (s->tx.on_session_start) s->tx.on_session_start(s->tx.ctx);
        esp_timer_stop(s->idle_timer);
        esp_timer_start_once(s->idle_timer,
                             (uint64_t)s->cfg.idle_timeout_ms * 1000);

        size_t rmtu = s->tx.max_payload ? s->tx.max_payload(s->tx.ctx) : 64;
        if (rmtu < 16) rmtu = 16;
        dfu_wire_start_rsp_t rrsp = {
            .resume      = resume_off,
            .max_payload = (uint16_t)(rmtu - 5),
            .window      = s->cfg.window_bytes,
        };
        ESP_LOGI(TAG, "[%s] session RESUMED at %" PRIu32 " / %" PRIu32 " B",
                 s->tx.name, resume_off, size);
        notify_rsp(s, OP_START, STS_OK, &rrsp, sizeof(rrsp));
        return;
    }

    /* Re-START while a session is open (including a suspended one
       with a different manifest) = client restart; drop old. NOT via
       abort_session: its on_release hook tears down the transport
       link, which would disconnect the very peer that just sent this
       START. Release + reset only. */
    if (dfu_session_in_progress(s)) {
        dfu_core_release(s->tx.source_id);
        xSemaphoreTake(s->mtx, portMAX_DELAY);
        reset_session_locked(s);
        xSemaphoreGive(s->mtx);
    }

    esp_err_t acq = dfu_core_acquire(s->tx.source_id, core_cb, s);
    if (acq != ESP_OK) {
        /* Two very different faults answer the host with the same BUSY:
           another transport owns the writer, or dfu_core_init() was never
           called at boot. dfu_core logs which one at ERROR; name the
           consequence here so the two lines read as one story. */
        ESP_LOGE(TAG, "[%s] START -> BUSY: dfu_core_acquire failed: %s",
                 s->tx.name, esp_err_to_name(acq));
        notify_rsp(s, OP_START, STS_BUSY, NULL, 0);
        return;
    }

    /* Optional hint for dfu_core's periodic UART status log; cleared
       automatically on the next reset_session. */
    dfu_core_report_total(size);

    xSemaphoreTake(s->mtx, portMAX_DELAY);
    reset_session_locked(s);
    s->image_size = size;
    memcpy(s->sha_expected, req.sha256, 32);
    mbedtls_sha256_init(&s->sha_ctx);
    mbedtls_sha256_starts(&s->sha_ctx, 0 /* SHA-256 */);
    s->sha_ctx_live = true;
    s->st = ST_TRANSFER;
    xSemaphoreGive(s->mtx);

    if (s->tx.on_session_start) s->tx.on_session_start(s->tx.ctx);

    esp_timer_start_once(s->idle_timer,
                         (uint64_t)s->cfg.idle_timeout_ms * 1000);

    size_t mtu = s->tx.max_payload ? s->tx.max_payload(s->tx.ctx) : 64;
    if (mtu < 16) mtu = 16;
    dfu_wire_start_rsp_t rsp = {
        .resume      = 0,                          /* no resume yet */
        .max_payload = (uint16_t)(mtu - 5),        /* DATA header */
        .window      = s->cfg.window_bytes,
    };
    ESP_LOGI(TAG, "[%s] session start: %" PRIu32 " B sha256=%08" PRIx32 "… mtu %u",
             s->tx.name, size,
             (uint32_t)(((uint32_t)req.sha256[0] << 24) | ((uint32_t)req.sha256[1] << 16) |
                        ((uint32_t)req.sha256[2] << 8)  |  (uint32_t)req.sha256[3]),
             (unsigned)mtu);
    notify_rsp(s, OP_START, STS_OK, &rsp, sizeof(rsp));
}

static void handle_data(dfu_session_t *s, const uint8_t *buf, uint16_t len)
{
    if (len < 6) return;  /* opcode + offset + >=1 payload byte */
    uint32_t off;
    memcpy(&off, buf + 1, 4);
    const uint8_t *payload = buf + 5;
    uint16_t plen = len - 5;

    /* No per-frame log here. Each ESP_LOGI line costs ~9 ms at 115200
       bps which blocks the NimBLE host task and starves the ACL RX
       path. flush_staging still logs once per 4 KB chunk (sparse). */

    xSemaphoreTake(s->mtx, portMAX_DELAY);
    if (s->st != ST_TRANSFER) {
        xSemaphoreGive(s->mtx);
        notify_progress(s, STS_INVALID_STATE);
        return;
    }
    if (off != s->next_off) {
        if (off < s->next_off) {
            /* Retransmission overlap. The host rewound (e.g. after an
               OFFSET_GAP, or a UDP datagram arrived out of order/twice)
               and is resending from a point we've already consumed into
               staging. Skip the prefix [off, next_off) and accept only
               the new tail. This makes DATA idempotent over the overlap
               region and is what breaks the UDP livelock: the host
               rewinds to `acked` on a gap, but next_off may sit ahead of
               acked (bytes staged, not yet flushed/acked) — without this,
               every resent frame re-trips "got < want" forever. */
            uint32_t skip = s->next_off - off;
            if (skip >= plen) {
                /* Pure duplicate, nothing new past next_off. Drop it; the
                   host gets its window credit from the periodic PROGRESS
                   that acks already mints. */
                xSemaphoreGive(s->mtx);
                return;
            }
            payload += skip;
            plen    -= (uint16_t)skip;
            off      = s->next_off;
        } else {
            /* off > next_off: a genuine forward gap — a datagram was lost.
               Every later in-flight datagram also lands here, so throttle:
               emit OFFSET_GAP once per next_off value, not once per frame.
               Re-armed the moment next_off advances (a DATA is consumed). */
            bool already = (s->gap_signalled && s->gap_off == s->next_off);
            s->gap_signalled = true;
            s->gap_off       = s->next_off;
            xSemaphoreGive(s->mtx);
            if (!already) {
                ESP_LOGW(TAG, "[%s] offset gap: got %" PRIu32 " want %" PRIu32,
                         s->tx.name, off, s->next_off);
                notify_progress(s, STS_OFFSET_GAP);  /* client rewinds */
            }
            return;
        }
    }
    s->gap_signalled = false;   /* next_off is about to advance */
    if (off + plen > s->image_size) {
        xSemaphoreGive(s->mtx);
        notify_progress(s, STS_TOO_LARGE);
        return;
    }
    /* SHA is updated at flush time (flush_staging), NOT here. Hashing
       per-frame would make the SHA state run ahead of the flushed
       bytes, which breaks resume: after a suspend we rewind next_off
       to the last flush boundary and the host re-sends from there —
       re-hashing those bytes would corrupt the digest. */
    s->next_off += plen;
    bool last = (s->next_off == s->image_size);
    xSemaphoreGive(s->mtx);

    /* Copy into staging, flushing as it fills. plen may straddle a
       boundary. */
    uint16_t done = 0;
    while (done < plen) {
        xSemaphoreTake(s->mtx, portMAX_DELAY);
        uint32_t room = STAGING_BYTES - s->staged;
        uint16_t take = (plen - done) < room ? (plen - done) : (uint16_t)room;
        memcpy(s->staging + s->staged, payload + done, take);
        s->staged += take;
        bool full = (s->staged == STAGING_BYTES);
        xSemaphoreGive(s->mtx);
        done += take;

        if (full || (last && done == plen)) {
            esp_err_t err = flush_staging(s);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "[%s] feed failed: %s",
                         s->tx.name, esp_err_to_name(err));
                abort_session(s, STS_FLASH_ERR);
                return;
            }
        }
    }
    esp_timer_stop(s->idle_timer);
    esp_timer_start_once(s->idle_timer,
                         (uint64_t)s->cfg.idle_timeout_ms * 1000);
}

static void handle_finish(dfu_session_t *s)
{
    xSemaphoreTake(s->mtx, portMAX_DELAY);
    if (s->st != ST_TRANSFER || s->next_off != s->image_size) {
        session_state_t cur = s->st;
        xSemaphoreGive(s->mtx);
        ESP_LOGW(TAG, "[%s] FINISH in state %d", s->tx.name, (int)cur);
        notify_rsp(s, OP_FINISH, STS_INVALID_STATE, NULL, 0);
        return;
    }
    s->st = ST_FINISHING;
    s->finish_rsp_due = true;
    bool drained = (s->acked == s->image_size);
    xSemaphoreGive(s->mtx);

    esp_timer_stop(s->idle_timer);
    if (drained) esp_timer_start_once(s->finalize_timer, 0);
    /* else: the last FEED_OK triggers finalize_timer from core_cb. */
}

static void handle_apply(dfu_session_t *s, const uint8_t *pl, uint16_t len)
{
    uint16_t delay_ms = 500;
    if (len >= 2) memcpy(&delay_ms, pl, 2);
    if (delay_ms < 200) delay_ms = 200;

    xSemaphoreTake(s->mtx, portMAX_DELAY);
    bool ok = (s->st == ST_READY);
    xSemaphoreGive(s->mtx);
    if (!ok) {
        notify_rsp(s, OP_APPLY, STS_INVALID_STATE, NULL, 0);
        return;
    }
    notify_rsp(s, OP_APPLY, STS_OK, NULL, 0);
    esp_timer_start_once(s->reboot_timer, (uint64_t)delay_ms * 1000);
}

static void handle_confirm(dfu_session_t *s)
{
    /* Idempotent: valid->valid is fine; mark-valid also kicks
       dfu_core's deferred pre-erase of the now-inactive slot. */
    dfu_core_mark_running_valid_after_ms(0);
    notify_rsp(s, OP_CONFIRM, STS_OK, NULL, 0);
}

static void handle_get_state(dfu_session_t *s)
{
    dfu_wire_get_state_rsp_t rsp;
    xSemaphoreTake(s->mtx, portMAX_DELAY);
    rsp.state      = (uint8_t)s->st;
    rsp.acked      = s->acked;
    rsp.image_size = s->image_size;
    xSemaphoreGive(s->mtx);
    /* GET_STATE is the host's lost-PROGRESS recovery path — log it at
       DEBUG so a stall trace shows whether the poll reached us and
       whether the reply went out (notify_rsp warns on send failure). */
    ESP_LOGD(TAG, "[%s] GET_STATE -> state=%u acked=%" PRIu32,
             s->tx.name, (unsigned)rsp.state, rsp.acked);
    notify_rsp(s, OP_GET_STATE, STS_OK, &rsp, sizeof(rsp));
}

/* --------------------------------------------------------------------- */
/* dfu_core callback — worker task context                                */
/* --------------------------------------------------------------------- */

static void core_cb(dfu_source_t src, dfu_event_t evt, esp_err_t err, void *user)
{
    (void)src;
    dfu_session_t *s = (dfu_session_t *)user;

    switch (evt) {
    case DFU_EVT_FEED_OK: {
        bool want_prog = false, want_finalize = false;
        xSemaphoreTake(s->mtx, portMAX_DELAY);
        if (s->pend_cnt > 0) {
            s->acked += s->pend[s->pend_rd].len;
            s->crc_acked = s->pend[s->pend_rd].crc;
            s->pend_rd = (s->pend_rd + 1) % PEND_RING;
            s->pend_cnt--;
        }
        if (s->st == ST_TRANSFER &&
            s->acked - s->last_prog_ack >= s->cfg.window_bytes / 2) {
            want_prog = true;
        }
        if (s->st == ST_FINISHING && s->acked == s->image_size) {
            want_finalize = true;
        }
        xSemaphoreGive(s->mtx);
        if (want_prog)     notify_progress(s, STS_OK);
        if (want_finalize) esp_timer_start_once(s->finalize_timer, 0);
        break;
    }
    case DFU_EVT_COMPLETE_OK: {
        bool rsp_due;
        xSemaphoreTake(s->mtx, portMAX_DELAY);
        s->core_completed = true;
        rsp_due = s->finish_rsp_due && (s->st == ST_FINISHING) && !s->sha_ctx_live;
        if (rsp_due) {
            s->st = ST_READY;
            s->finish_rsp_due = false;
        }
        uint32_t crc = s->crc_acked;
        xSemaphoreGive(s->mtx);
        /* Raw path: SHA already verified in finalize_cb; this is
           esp_ota_end (image header + appended-signature check)
           succeeding. UF2 path: this can arrive before FINISH;
           finalize_cb handles it. */
        if (rsp_due) {
            ESP_LOGI(TAG, "[%s] image verified, crc32=0x%08" PRIx32, s->tx.name, crc);
            notify_rsp(s, OP_FINISH, STS_OK, &crc, 4);
        }
        break;
    }
    case DFU_EVT_ERROR: {
        ESP_LOGE(TAG, "[%s] dfu_core error: %s", s->tx.name, esp_err_to_name(err));
        bool rsp_due;
        xSemaphoreTake(s->mtx, portMAX_DELAY);
        rsp_due = s->finish_rsp_due;
        reset_session_locked(s);
        xSemaphoreGive(s->mtx);
        /* esp_ota_end failing = image/signature rejection -> SIG_INVALID. */
        if (rsp_due) notify_rsp(s, OP_FINISH, STS_SIG_INVALID, NULL, 0);
        else         notify_progress(s, STS_FLASH_ERR);
        if (s->tx.on_release) s->tx.on_release(s->tx.ctx);
        break;
    }
    }
}

/* --------------------------------------------------------------------- */
/* Timers: idle watchdog, SHA finalisation, reboot                        */
/* --------------------------------------------------------------------- */

static void idle_cb(void *arg)
{
    dfu_session_t *s = (dfu_session_t *)arg;
    xSemaphoreTake(s->mtx, portMAX_DELAY);
    session_state_t st = s->st;
    xSemaphoreGive(s->mtx);
    if (st == ST_TRANSFER) {
        /* No DATA for idle_timeout_ms. Either the peer died without a
           disconnect, or the link wedged (seen live: BlueZ stops
           delivering ACL both ways while the LL stays up). Suspend for
           resume — same as peer_gone — and tear the link down via
           on_release so the host's reconnect gets a fresh, unwedged
           connection. The host re-STARTs with the same manifest and
           picks up at the flush boundary. */
        ESP_LOGW(TAG, "[%s] transfer idle; suspending for resume + dropping link",
                 s->tx.name);
        notify_progress(s, STS_TIMEOUT);  /* best effort on a dead link */
        xSemaphoreTake(s->mtx, portMAX_DELAY);
        s->next_off -= s->staged;
        s->staged = 0;
        s->gap_signalled = false;
        s->st = ST_SUSPENDED;
        xSemaphoreGive(s->mtx);
        esp_timer_start_once(s->idle_timer, (uint64_t)RESUME_GRACE_MS * 1000);
        if (s->tx.on_release) s->tx.on_release(s->tx.ctx);
    } else if (st == ST_SUSPENDED) {
        /* Resume grace expired with no matching re-START. Abandon the
           partial image. No on_release: the original peer is long
           gone, and a freshly-connected central that didn't resume
           shouldn't get its link torn down. */
        ESP_LOGW(TAG, "[%s] resume grace expired, abandoning partial image",
                 s->tx.name);
        dfu_core_release(s->tx.source_id);
        xSemaphoreTake(s->mtx, portMAX_DELAY);
        reset_session_locked(s);
        xSemaphoreGive(s->mtx);
    }
}

static void finalize_cb(void *arg)
{
    /* Finalisation. Runs on the esp_timer task — never inside core_cb.
       See file header invariant 2. */
    dfu_session_t *s = (dfu_session_t *)arg;
    uint8_t digest[32];

    xSemaphoreTake(s->mtx, portMAX_DELAY);
    if (s->st != ST_FINISHING || s->acked != s->image_size) {
        xSemaphoreGive(s->mtx);
        return;
    }
    mbedtls_sha256_finish(&s->sha_ctx, digest);
    mbedtls_sha256_free(&s->sha_ctx);
    s->sha_ctx_live = false;
    bool sha_ok = (memcmp(digest, s->sha_expected, 32) == 0);
    bool done   = s->core_completed;
    uint32_t crc = s->crc_acked;
    xSemaphoreGive(s->mtx);

    if (!sha_ok) {
        ESP_LOGE(TAG, "[%s] manifest SHA-256 mismatch", s->tx.name);
        if (done) {
            /* UF2 auto-complete already ran set_boot_partition; un-stage. */
            const esp_partition_t *run = esp_ota_get_running_partition();
            if (run) esp_ota_set_boot_partition(run);
        }
        dfu_core_release(s->tx.source_id);
        notify_rsp(s, OP_FINISH, STS_SHA_MISMATCH, NULL, 0);
        xSemaphoreTake(s->mtx, portMAX_DELAY);
        reset_session_locked(s);
        xSemaphoreGive(s->mtx);
        if (s->tx.on_release) s->tx.on_release(s->tx.ctx);
        return;
    }

    if (done) {
        /* dfu_core already finalized (UF2 path). Straight to READY. */
        xSemaphoreTake(s->mtx, portMAX_DELAY);
        s->st = ST_READY;
        s->finish_rsp_due = false;
        xSemaphoreGive(s->mtx);
        ESP_LOGI(TAG, "[%s] image verified, crc32=0x%08" PRIx32, s->tx.name, crc);
        notify_rsp(s, OP_FINISH, STS_OK, &crc, 4);
        return;
    }

    /* Raw path: esp_ota_end + set_boot. Response sent on DFU_EVT_COMPLETE_OK. */
    esp_err_t err = dfu_core_complete(s->tx.source_id, 0 /* no auto reboot */);
    if (err != ESP_OK) {
        notify_rsp(s, OP_FINISH, STS_FLASH_ERR, NULL, 0);
        abort_session(s, STS_OK);
    }
}

static void reboot_cb(void *arg) { (void)arg; esp_restart(); }

/* --------------------------------------------------------------------- */
/* Wire/send helpers — never call with mtx held.                          */
/* --------------------------------------------------------------------- */

static int session_send(dfu_session_t *s, const void *buf, uint16_t len)
{
    if (s->tx.send == NULL) return 0;
    int rc = s->tx.send(s->tx.ctx, buf, len);
    if (rc != 0) {
        ESP_LOGW(TAG, "[%s] transport send rc=%d", s->tx.name, rc);
    }
    return rc;
}

static void notify_rsp(dfu_session_t *s, uint8_t opcode, uint8_t status,
                       const void *extra, uint16_t extra_len)
{
    uint8_t buf[2 + 16];
    buf[0] = opcode | OP_RSP_FLAG;
    buf[1] = status;
    if (extra_len > sizeof(buf) - 2) extra_len = sizeof(buf) - 2;
    if (extra && extra_len) memcpy(buf + 2, extra, extra_len);
    session_send(s, buf, (uint16_t)(2 + extra_len));
}

static void notify_progress(dfu_session_t *s, uint8_t status)
{
    uint8_t buf[11];
    xSemaphoreTake(s->mtx, portMAX_DELAY);
    uint32_t a = s->acked, c = s->crc_acked;
    uint8_t  state = (uint8_t)s->st;
    xSemaphoreGive(s->mtx);

    dfu_wire_progress_body_t body = {
        .status = status,
        .state  = state,
        .acked  = a,
        .crc    = c,
    };
    buf[0] = OP_PROGRESS;
    memcpy(buf + 1, &body, sizeof(body));
    int rc = session_send(s, buf, (uint16_t)(1 + sizeof(body)));
    ESP_LOGD(TAG, "[%s] PROGRESS acked=%" PRIu32 " status=%u rc=%d",
             s->tx.name, a, (unsigned)status, rc);

    /* Only mark this ack point as reported if the notify actually went
       out. On failure the threshold check in core_cb stays armed, so
       the next FEED_OK retries — otherwise one dropped notify starves
       the host's send window forever (GET_STATE is the host's fallback,
       but don't rely on a single recovery path). Use the snapshot `a`,
       not s->acked, in case more chunks were acked during the send. */
    if (rc == 0) {
        xSemaphoreTake(s->mtx, portMAX_DELAY);
        if (a > s->last_prog_ack) s->last_prog_ack = a;
        xSemaphoreGive(s->mtx);
    }
}

/* --------------------------------------------------------------------- */
/* Low-level utils                                                         */
/* --------------------------------------------------------------------- */

static void reset_session_locked(dfu_session_t *s)
{
    s->st = ST_IDLE;
    s->image_size = s->next_off = s->acked = s->last_prog_ack = 0;
    s->gap_signalled = false;
    s->gap_off = 0;
    s->crc_fed = s->crc_acked = 0;
    s->staged = 0;
    s->pend_rd = s->pend_wr = s->pend_cnt = 0;
    s->core_completed = false;
    s->finish_rsp_due = false;
    if (s->sha_ctx_live) {
        mbedtls_sha256_free(&s->sha_ctx);
        s->sha_ctx_live = false;
    }
    esp_timer_stop(s->idle_timer);
}

static void abort_session(dfu_session_t *s, uint8_t status_for_client)
{
    dfu_core_release(s->tx.source_id);
    if (status_for_client != STS_OK) notify_progress(s, status_for_client);
    xSemaphoreTake(s->mtx, portMAX_DELAY);
    reset_session_locked(s);
    xSemaphoreGive(s->mtx);
    if (s->tx.on_release) s->tx.on_release(s->tx.ctx);
}

/* Hand the staging buffer to dfu_core. Pre-registers the pending entry
   so a fast FEED_OK can't race the bookkeeping; rolls it back if the
   feed fails. */
static esp_err_t flush_staging(dfu_session_t *s)
{
    xSemaphoreTake(s->mtx, portMAX_DELAY);
    uint32_t n = s->staged;
    if (n == 0) {
        xSemaphoreGive(s->mtx);
        return ESP_OK;
    }
    if (s->pend_cnt == PEND_RING) {
        /* Should be impossible if window <= ring capacity; treat as fatal. */
        xSemaphoreGive(s->mtx);
        return ESP_ERR_NO_MEM;
    }
    /* Hash + CRC at the flush boundary so both always cover exactly
       the bytes handed to dfu_core. This alignment is what makes
       resume possible: on suspend we drop the (unhashed) staging
       remainder and the host re-sends from the flush boundary. */
    mbedtls_sha256_update(&s->sha_ctx, s->staging, n);
    s->crc_fed = esp_rom_crc32_le(s->crc_fed, s->staging, n);
    s->pend[s->pend_wr].len = (uint16_t)n;
    s->pend[s->pend_wr].crc = s->crc_fed;
    s->pend_wr = (s->pend_wr + 1) % PEND_RING;
    s->pend_cnt++;
    xSemaphoreGive(s->mtx);

    /* Time the feed so we can see if dfu_core_feed is blocking the caller
       (which on BLE is the NimBLE host task, starving the ACL RX path). */
    int64_t feed_t0 = esp_timer_get_time();
    esp_err_t err = dfu_core_feed(s->tx.source_id, s->staging, n);
    int64_t feed_t1 = esp_timer_get_time();
    ESP_LOGD(TAG, "[%s] flush %" PRIu32 " B -> feed rc=%d took=%" PRId64 "us",
             s->tx.name, n, (int)err, (feed_t1 - feed_t0));
    if (err == ESP_ERR_TIMEOUT) {
        /* Worker pipeline full; one retry. Window flow control should
           make this rare. */
        err = dfu_core_feed(s->tx.source_id, s->staging, n);
        ESP_LOGW(TAG, "[%s] flush retry rc=%d", s->tx.name, (int)err);
    }

    xSemaphoreTake(s->mtx, portMAX_DELAY);
    if (err != ESP_OK) {
        /* Entry was never queued; un-register it (it is the newest). */
        s->pend_wr = (s->pend_wr + PEND_RING - 1) % PEND_RING;
        s->pend_cnt--;
        /* crc_fed rollback not needed on the error path: session dies. */
    } else {
        s->staged = 0;
    }
    xSemaphoreGive(s->mtx);
    return err;
}
