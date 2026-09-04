#include "dfu_core.h"
#include "dfu_core_uf2.h"

#include <inttypes.h>
#include <string.h>

#if CONFIG_DFU_CORE_DELTA_ENABLE
#include "esp_delta_ota.h"
#endif

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "sdkconfig.h"

static const char *TAG = "dfu_core";

// One worker task processes every OTA-related operation: feed, complete,
// abort, and the background pre-erase. Keeps esp_ota_* state strictly on
// one task so we don't need esp_ota mutexes of our own.

// Slot count and feed timeout come from Kconfig — see component Kconfig.
// DFU_CORE_SCRATCH_BYTES is defined in dfu_core.h (shared with session layer).
#define DFU_CORE_SLOT_COUNT      CONFIG_DFU_CORE_SCRATCH_SLOTS

typedef enum {
    MSG_FEED,
    MSG_COMPLETE,
    MSG_ABORT,
    MSG_PREERASE,
    MSG_MARK_VALID,
} msg_op_t;

typedef struct {
    msg_op_t      op;
    uint8_t       slot;             // index into scratch[][] for FEED
    uint16_t      len;              // bytes valid in scratch[slot]
    uint32_t      reboot_delay_ms;  // for COMPLETE
} msg_t;

static QueueHandle_t       queue;
static SemaphoreHandle_t   slot_sem;    // counting, init to SLOT_COUNT
static SemaphoreHandle_t   state_mtx;   // guards active_*, scratch ownership
static TaskHandle_t        worker_handle;

static uint8_t             scratch[DFU_CORE_SLOT_COUNT][DFU_CORE_SCRATCH_BYTES];
static bool                slot_busy[DFU_CORE_SLOT_COUNT];

static dfu_source_t        active_src = DFU_SRC_NONE;
static dfu_core_cb_t       active_cb;
static void               *active_user;

// Mode is decided from the first feed and locked for the session.
typedef enum { MODE_UNKNOWN, MODE_RAW, MODE_UF2, MODE_DELTA } mode_t;
static mode_t              mode = MODE_UNKNOWN;
static dfu_core_uf2_parser_t uf2_state;
static bool                ota_active;       // raw stream in progress
static const esp_partition_t *ota_part;

#if CONFIG_DFU_CORE_DELTA_ENABLE
// Delta-patch stream (esp_delta_ota / detools): 64-byte container
// header [magic u32 LE | base image SHA-256 | reserved], then a
// heatshrink-compressed detools 'sequential' patch. The base digest is
// checked against the RUNNING partition before a single byte is
// written, so a patch generated against the wrong base is rejected
// outright — host-side bookkeeping never has to be trusted.
#define DELTA_MAGIC      0xfccdde10u
#define DELTA_HDR_BYTES  64
static esp_delta_ota_handle_t delta_handle;
static uint8_t             delta_hdr[DELTA_HDR_BYTES];
static uint8_t             delta_hdr_have;
#endif

// Set true by handle_feed when the UF2 parser consumes the last block in a
// stream. The worker reads this after the feed returns to decide whether to
// emit DFU_EVT_FEED_OK or auto-emit DFU_EVT_COMPLETE_OK (so MSC and any
// other transport without an explicit end-of-stream signal still gets a
// reboot). Reset by reset_session.
static bool                auto_completed;

// Periodic-log bookkeeping. bytes_written accumulates input bytes that
// passed handle_feed cleanly; image_total is an optional hint set by the
// transport (see dfu_core_report_total). Both reset on session teardown.
static uint32_t            bytes_written;
static uint32_t            image_total;
#if CONFIG_DFU_CORE_STATUS_LOG_PERIOD_MS > 0
static esp_timer_handle_t  status_timer;
#endif

// ---------------------------------------------------------------------------
// Forward declarations

static void         deliver(dfu_event_t evt, esp_err_t err);
static void         reset_session(void);
static esp_err_t    worker_ota_begin(void);
static esp_err_t    worker_ota_write(const void *data, size_t len);
static esp_err_t    worker_ota_finish(void);
static void         worker_ota_abort(void);
#if CONFIG_DFU_CORE_DELTA_ENABLE
static bool         delta_looks_like(const uint8_t *data, size_t len);
static esp_err_t    delta_start(void);
static esp_err_t    delta_feed(const uint8_t *data, size_t len);
#endif
static esp_err_t    handle_feed(const uint8_t *data, size_t len);
static esp_err_t    handle_complete(uint32_t reboot_delay_ms);
static void         worker_task(void *arg);
static void         preerase(void);
static void         arm_reboot(uint32_t delay_ms);
static void         reboot_cb(void *arg);
static void         mark_valid_timer_cb(void *arg);
#if CONFIG_DFU_CORE_STATUS_LOG_PERIOD_MS > 0
static const char  *src_name(dfu_source_t src);
static void         status_log_cb(void *arg);
#endif

// ---------------------------------------------------------------------------
// Public API

esp_err_t dfu_core_init(void)
{
    if (queue) return ESP_OK;
    queue = xQueueCreate(4, sizeof(msg_t));
    slot_sem = xSemaphoreCreateCounting(DFU_CORE_SLOT_COUNT, DFU_CORE_SLOT_COUNT);
    state_mtx = xSemaphoreCreateMutex();
    if (!queue || !slot_sem || !state_mtx) return ESP_ERR_NO_MEM;
    BaseType_t ok = xTaskCreate(worker_task, "ota_worker",
                                CONFIG_DFU_CORE_WORKER_STACK_SIZE, NULL,
                                CONFIG_DFU_CORE_WORKER_PRIORITY, &worker_handle);
    if (ok != pdPASS) return ESP_ERR_NO_MEM;

#if CONFIG_DFU_CORE_STATUS_LOG_PERIOD_MS > 0
    const esp_timer_create_args_t status_args = {
        .callback = &status_log_cb,
        .name     = "dfu_status",
    };
    if (esp_timer_create(&status_args, &status_timer) == ESP_OK) {
        esp_timer_start_periodic(
            status_timer,
            (uint64_t)CONFIG_DFU_CORE_STATUS_LOG_PERIOD_MS * 1000);
    }
#endif

    ESP_LOGI(TAG, "worker up");
    return ESP_OK;
}

esp_err_t dfu_core_acquire(dfu_source_t src, dfu_core_cb_t cb, void *user)
{
    if (src == DFU_SRC_NONE) return ESP_ERR_INVALID_ARG;
    if (!state_mtx) {
        ESP_LOGE(TAG, "acquire by src=%d before dfu_core_init() — no OTA worker",
                 (int)src);
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(state_mtx, portMAX_DELAY);
    if (active_src != DFU_SRC_NONE && active_src != src) {
        dfu_source_t owner = active_src;
        xSemaphoreGive(state_mtx);
        ESP_LOGE(TAG, "acquire by src=%d refused; src=%d owns the writer",
                 (int)src, (int)owner);
        return ESP_ERR_INVALID_STATE;
    }
    if (active_src == DFU_SRC_NONE) {
        active_src = src;
        active_cb = cb;
        active_user = user;
        mode = MODE_UNKNOWN;
        memset(&uf2_state, 0, sizeof(uf2_state));
        ESP_LOGI(TAG, "acquired by src=%d", (int)src);
    }
    xSemaphoreGive(state_mtx);
    return ESP_OK;
}

esp_err_t dfu_core_feed(dfu_source_t src, const void *data, size_t len)
{
    if (!data || len == 0) return ESP_ERR_INVALID_ARG;
    if (len > DFU_CORE_SCRATCH_BYTES) return ESP_ERR_INVALID_SIZE;
    if (!queue) return ESP_ERR_INVALID_STATE;

    // Validate ownership.
    xSemaphoreTake(state_mtx, portMAX_DELAY);
    if (active_src != src) {
        xSemaphoreGive(state_mtx);
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreGive(state_mtx);

    // Wait briefly for a free scratch slot. Practical worst case is a few
    // ms — the time for the worker to esp_ota_write the previous chunk.
    if (xSemaphoreTake(slot_sem, pdMS_TO_TICKS(CONFIG_DFU_CORE_FEED_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    // Pick the first non-busy slot under the mutex.
    xSemaphoreTake(state_mtx, portMAX_DELAY);
    uint8_t slot = DFU_CORE_SLOT_COUNT;
    for (uint8_t i = 0; i < DFU_CORE_SLOT_COUNT; i++) {
        if (!slot_busy[i]) { slot = i; slot_busy[i] = true; break; }
    }
    xSemaphoreGive(state_mtx);
    if (slot >= DFU_CORE_SLOT_COUNT) {
        xSemaphoreGive(slot_sem);
        return ESP_FAIL;
    }

    memcpy(scratch[slot], data, len);
    msg_t msg = { .op = MSG_FEED, .slot = slot, .len = (uint16_t)len };
    if (xQueueSend(queue, &msg, portMAX_DELAY) != pdTRUE) {
        xSemaphoreTake(state_mtx, portMAX_DELAY);
        slot_busy[slot] = false;
        xSemaphoreGive(state_mtx);
        xSemaphoreGive(slot_sem);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t dfu_core_complete(dfu_source_t src, uint32_t reboot_delay_ms)
{
    if (!queue) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(state_mtx, portMAX_DELAY);
    if (active_src == DFU_SRC_NONE) {
        // UF2 stream already auto-completed during the last feed; reboot is
        // already armed, session torn down. Treat this as a no-op success so
        // transports that always call complete() (DFU's MANIFEST) don't see
        // an error after a clean UF2 flash.
        xSemaphoreGive(state_mtx);
        return ESP_OK;
    }
    if (active_src != src) {
        xSemaphoreGive(state_mtx);
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreGive(state_mtx);
    msg_t msg = { .op = MSG_COMPLETE, .reboot_delay_ms = reboot_delay_ms };
    return (xQueueSend(queue, &msg, portMAX_DELAY) == pdTRUE) ? ESP_OK : ESP_FAIL;
}

void dfu_core_release(dfu_source_t src)
{
    if (!queue) return;
    xSemaphoreTake(state_mtx, portMAX_DELAY);
    if (active_src != src) {
        xSemaphoreGive(state_mtx);
        return;
    }
    xSemaphoreGive(state_mtx);
    msg_t msg = { .op = MSG_ABORT };
    xQueueSend(queue, &msg, portMAX_DELAY);
}

bool dfu_core_in_progress(void)
{
    if (!state_mtx) return false;
    xSemaphoreTake(state_mtx, portMAX_DELAY);
    bool active = (active_src != DFU_SRC_NONE);
    xSemaphoreGive(state_mtx);
    return active;
}

void dfu_core_mark_running_valid_after_ms(uint32_t delay_ms)
{
    const esp_timer_create_args_t args = {
        .callback = &mark_valid_timer_cb,
        .name     = "ota_mark_valid",
    };
    esp_timer_handle_t t;
    if (esp_timer_create(&args, &t) == ESP_OK) {
        esp_timer_start_once(t, (uint64_t)delay_ms * 1000);
    }
}

void dfu_core_report_total(uint32_t total_bytes)
{
    if (!state_mtx) return;
    xSemaphoreTake(state_mtx, portMAX_DELAY);
    image_total = total_bytes;
    xSemaphoreGive(state_mtx);
}

// ---------------------------------------------------------------------------
// Worker task and message handlers

static esp_err_t handle_feed(const uint8_t *data, size_t len)
{
    if (mode == MODE_UNKNOWN) {
#if CONFIG_DFU_CORE_DELTA_ENABLE
        if (delta_looks_like(data, len)) {
            ESP_LOGI(TAG, "delta patch stream detected");
            delta_hdr_have = 0;
            mode = MODE_DELTA;
        } else
#endif
        if (dfu_core_uf2_looks_like_uf2(data, len)) {
            ESP_LOGI(TAG, "UF2 stream detected");
            dfu_core_uf2_init(&uf2_state);
            mode = MODE_UF2;
        } else {
            ESP_LOGI(TAG, "raw stream");
            esp_err_t err = worker_ota_begin();
            if (err != ESP_OK) return err;
            mode = MODE_RAW;
        }
    }
#if CONFIG_DFU_CORE_DELTA_ENABLE
    if (mode == MODE_DELTA) {
        return delta_feed(data, len);
    }
#endif
    if (mode == MODE_UF2) {
        esp_err_t err = dfu_core_uf2_feed(&uf2_state, data, len);
        if (err == ESP_OK && dfu_core_uf2_complete(&uf2_state)) {
            auto_completed = true;
        }
        return err;
    }
    return worker_ota_write(data, len);
}

static esp_err_t handle_complete(uint32_t reboot_delay_ms)
{
    esp_err_t err = ESP_OK;
    if (mode == MODE_RAW) {
        err = worker_ota_finish();
#if CONFIG_DFU_CORE_DELTA_ENABLE
    } else if (mode == MODE_DELTA) {
        // Finalize drains esp_delta_ota's internal buffer (final
        // write_cb calls land here), then the sequential writer's
        // finish validates + stages the reconstructed image.
        err = esp_delta_ota_finalize(delta_handle);
        esp_delta_ota_deinit(delta_handle);
        delta_handle = NULL;
        if (err == ESP_OK) {
            err = worker_ota_finish();
        } else {
            ESP_LOGE(TAG, "delta finalize: %s", esp_err_to_name(err));
            worker_ota_abort();
        }
#endif
    } else if (mode == MODE_UF2 && !dfu_core_uf2_complete(&uf2_state)) {
        ESP_LOGE(TAG, "UF2 stream incomplete at complete()");
        worker_ota_abort();
        err = ESP_ERR_INVALID_STATE;
    }
    if (err == ESP_OK) arm_reboot(reboot_delay_ms);
    return err;
}

static void worker_task(void *arg)
{
    (void)arg;
    msg_t msg;
    while (1) {
        if (xQueueReceive(queue, &msg, portMAX_DELAY) != pdTRUE) continue;

        switch (msg.op) {
        case MSG_FEED: {
            // Drop feeds that were already queued when the session ended
            // (error/abort/complete reset active_src to NONE). Without
            // this, a FEED enqueued before a mid-stream error gets
            // processed against a fresh mode=UNKNOWN state and is
            // mis-detected as a NEW raw stream — e.g. leftover delta-
            // patch bytes after a base-mismatch abort were written to
            // flash as raw firmware, corrupting the next upload.
            xSemaphoreTake(state_mtx, portMAX_DELAY);
            bool owned = (active_src != DFU_SRC_NONE);
            xSemaphoreGive(state_mtx);
            if (!owned) {
                xSemaphoreTake(state_mtx, portMAX_DELAY);
                slot_busy[msg.slot] = false;
                xSemaphoreGive(state_mtx);
                xSemaphoreGive(slot_sem);
                break;
            }
            esp_err_t err = handle_feed(scratch[msg.slot], msg.len);
            xSemaphoreTake(state_mtx, portMAX_DELAY);
            slot_busy[msg.slot] = false;
            if (err == ESP_OK) bytes_written += msg.len;
            xSemaphoreGive(state_mtx);
            xSemaphoreGive(slot_sem);
            if (err == ESP_OK) {
                if (auto_completed) {
                    // UF2 stream's last block was just written. Arm reboot
                    // and tear down the session so transports without a
                    // separate end-of-stream signal (MSC) can still finish
                    // without an explicit dfu_core_complete call.
                    arm_reboot(CONFIG_DFU_CORE_REBOOT_DELAY_MS);
                    deliver(DFU_EVT_COMPLETE_OK, ESP_OK);
                    xSemaphoreTake(state_mtx, portMAX_DELAY);
                    reset_session();
                    xSemaphoreGive(state_mtx);
                } else {
                    deliver(DFU_EVT_FEED_OK, ESP_OK);
                }
            } else {
                worker_ota_abort();
                deliver(DFU_EVT_ERROR, err);
                xSemaphoreTake(state_mtx, portMAX_DELAY);
                reset_session();
                xSemaphoreGive(state_mtx);
            }
            break;
        }
        case MSG_COMPLETE: {
            esp_err_t err = handle_complete(msg.reboot_delay_ms);
            /* Deliver BEFORE reset_session: reset clears active_cb, so
               the old order silently dropped DFU_EVT_COMPLETE_OK and
               the session waited in FINISHING forever. Same order as
               the MSG_FEED error path. */
            deliver(err == ESP_OK ? DFU_EVT_COMPLETE_OK : DFU_EVT_ERROR, err);
            xSemaphoreTake(state_mtx, portMAX_DELAY);
            reset_session();
            xSemaphoreGive(state_mtx);
            break;
        }
        case MSG_ABORT: {
            worker_ota_abort();
            xSemaphoreTake(state_mtx, portMAX_DELAY);
            reset_session();
            xSemaphoreGive(state_mtx);
            break;
        }
        case MSG_PREERASE:
            preerase();
            break;
        case MSG_MARK_VALID: {
            esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "mark_app_valid: %s", esp_err_to_name(err));
                break;
            }
            ESP_LOGI(TAG, "running slot accepted, rollback cancelled");
            // Pre-erase the inactive slot ONLY if no OTA is in flight. If the
            // user started flashing within the first ~5 s of boot, we'd
            // otherwise wipe the bytes we just wrote and leave the partition
            // with an invalid image (esp_image_verify → bad magic on
            // set_boot_partition). Skipping is safe — the next OTA's
            // uf2_start will lazy-erase per sector.
#if CONFIG_DFU_CORE_PREERASE_INACTIVE
            xSemaphoreTake(state_mtx, portMAX_DELAY);
            bool active = (active_src != DFU_SRC_NONE);
            xSemaphoreGive(state_mtx);
            if (active) {
                ESP_LOGI(TAG, "deferring preerase: OTA src=%d in progress", (int)active_src);
            } else {
                preerase();
            }
#endif
            break;
        }
        }
    }
}

// ---------------------------------------------------------------------------
// Raw sequential path (worker context only). Delegates to the UF2
// path's address-dispatched writer below: a sequential stream is just
// target_addr = partition_base + cursor. The point of sharing it is
// the preerase awareness — esp_partition_write + skip-erase-when-
// preerased instead of esp_ota_begin/write, which unconditionally
// re-erase (a full-partition erase at first feed measured ~5 s and
// starved the scratch slots under a deep transfer window).
// esp_ota_set_boot_partition validates the image (incl. secure-boot
// signature when enabled) before flipping otadata, so dropping
// esp_ota_end loses no checks.

esp_err_t dfu_core_internal_uf2_start(void);
esp_err_t dfu_core_internal_uf2_write(uint32_t target_addr,
                                      const void *payload, size_t len,
                                      bool *out_written);
esp_err_t dfu_core_internal_uf2_finish(void);
void      dfu_core_internal_uf2_abort(void);

static uint32_t raw_off;   // sequential write cursor into ota_part

static esp_err_t worker_ota_begin(void)
{
    if (ota_active) return ESP_OK;
    ota_part = esp_ota_get_next_update_partition(NULL);
    if (!ota_part) {
        ESP_LOGE(TAG, "no next OTA partition");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "target: %s (offset 0x%" PRIx32 ", size 0x%" PRIx32 ")",
             ota_part->label, ota_part->address, ota_part->size);
    esp_err_t err = dfu_core_internal_uf2_start();
    if (err != ESP_OK) {
        ota_part = NULL;
        return err;
    }
    raw_off = 0;
    ota_active = true;
    return ESP_OK;
}

static esp_err_t worker_ota_write(const void *data, size_t len)
{
    if (!ota_active) {
        esp_err_t err = worker_ota_begin();
        if (err != ESP_OK) return err;
    }
    if (raw_off + len > ota_part->size) return ESP_ERR_INVALID_SIZE;
    esp_err_t err = dfu_core_internal_uf2_write(ota_part->address + raw_off,
                                                data, len, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "raw write @0x%" PRIx32 " (%zu B): %s",
                 raw_off, len, esp_err_to_name(err));
        worker_ota_abort();
        return err;
    }
    raw_off += len;
    return ESP_OK;
}

static esp_err_t worker_ota_finish(void)
{
    if (!ota_active) return ESP_ERR_INVALID_STATE;
    ota_active = false;
    esp_err_t err = dfu_core_internal_uf2_finish();
    ota_part = NULL;
    return err;
}

static void worker_ota_abort(void)
{
#if CONFIG_DFU_CORE_DELTA_ENABLE
    if (delta_handle) {
        esp_delta_ota_deinit(delta_handle);
        delta_handle = NULL;
    }
    delta_hdr_have = 0;
#endif
    if (!ota_active) return;
    ota_active = false;
    ota_part = NULL;
    dfu_core_internal_uf2_abort();
    ESP_LOGW(TAG, "aborted in-progress OTA");
}

#if CONFIG_DFU_CORE_DELTA_ENABLE
// ---------------------------------------------------------------------------
// Delta-patch path (worker context only). esp_delta_ota reads the
// base from the RUNNING partition and emits the reconstructed image
// sequentially — which feeds straight into the raw path's
// preerase-aware writer above.

static bool delta_looks_like(const uint8_t *data, size_t len)
{
    uint32_t magic;
    if (len < 4) return false;
    memcpy(&magic, data, 4);
    return magic == DELTA_MAGIC;
}

static esp_err_t delta_read_cb(uint8_t *buf, size_t size, int src_offset,
                               void *user_data)
{
    (void)user_data;
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (!run || src_offset < 0 ||
        (size_t)src_offset + size > run->size) {
        return ESP_ERR_INVALID_ARG;
    }
    return esp_partition_read(run, (size_t)src_offset, buf, size);
}

static esp_err_t delta_write_cb(const uint8_t *buf, size_t size,
                                void *user_data)
{
    (void)user_data;
    return worker_ota_write(buf, size);
}

static esp_err_t delta_start(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (!run) return ESP_FAIL;

    // The header digest is the base image's appended SHA-256 (what
    // esptool calls the Validation Hash). Compare against the running
    // partition before touching flash: wrong base = corrupted result,
    // and the reconstruction would only fail at the final image
    // verification after a full transfer.
    uint8_t digest[32];
    esp_err_t err = esp_partition_get_sha256(run, digest);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "delta: get_sha256(%s): %s", run->label,
                 esp_err_to_name(err));
        return err;
    }
    if (memcmp(digest, delta_hdr + 4, 32) != 0) {
        ESP_LOGE(TAG, "delta: base mismatch — patch was generated against a "
                 "different image than the one running on %s", run->label);
        return ESP_ERR_INVALID_VERSION;
    }

    esp_delta_ota_cfg_t cfg = {
        .user_data               = NULL,
        .read_cb_with_user_data  = delta_read_cb,
        .write_cb_with_user_data = delta_write_cb,
    };
    delta_handle = esp_delta_ota_init(&cfg);
    if (!delta_handle) return ESP_ERR_NO_MEM;

    err = worker_ota_begin();
    if (err != ESP_OK) {
        esp_delta_ota_deinit(delta_handle);
        delta_handle = NULL;
        return err;
    }
    ESP_LOGI(TAG, "delta: base %s verified, applying patch", run->label);
    return ESP_OK;
}

static esp_err_t delta_feed(const uint8_t *data, size_t len)
{
    // Accumulate the 64-byte container header first; everything after
    // it streams into esp_delta_ota. Feeds can be smaller than the
    // header (USB-DFU EP0 chunks), so this must handle splits.
    size_t off = 0;
    if (delta_hdr_have < DELTA_HDR_BYTES) {
        size_t take = DELTA_HDR_BYTES - delta_hdr_have;
        if (take > len) take = len;
        memcpy(delta_hdr + delta_hdr_have, data, take);
        delta_hdr_have += take;
        off = take;
        if (delta_hdr_have < DELTA_HDR_BYTES) return ESP_OK;
        esp_err_t err = delta_start();
        if (err != ESP_OK) return err;
    }
    if (len > off) {
        return esp_delta_ota_feed_patch(delta_handle, data + off,
                                        (int)(len - off));
    }
    return ESP_OK;
}
#endif // CONFIG_DFU_CORE_DELTA_ENABLE

// ---------------------------------------------------------------------------
// UF2-specific path (address-dispatched, bypasses esp_ota_begin/write/end).
//
// UF2 blocks carry their own target_addr, so we don't write sequentially —
// we translate the address into an offset inside the inactive OTA partition
// and write the payload there with esp_partition_write. Whichever 4 KB
// sector we touch first is erased lazily (tracked in sector_erased). The
// background pre-erase that runs after mark_valid usually clears the whole
// partition, in which case the lazy erase is a no-op marking.

#define FLASH_SECTOR_BYTES 4096
/* Sized for the largest OTA slot this chip family plausibly carries: 8 MB /
   4 KB = 2048 sectors, 256 B of bitmap. The previous 384 covered only a
   1.5 MB slot (pros3's) and truncated silently on the 3 MB controller and
   devkit16 slots — and truncation here corrupts rather than merely slows:
   mark_sector_erased() no-ops out of range, so the 16 sub-sector UF2 blocks
   sharing one 4 KB sector each re-erase it and wipe the previous 15 writes
   (the very hazard the comment in the writer below warns about). Anything
   that still would not fit is refused in uf2_start rather than degrading. */
#define MAX_SECTORS         2048
static uint8_t sector_erased[(MAX_SECTORS + 7) / 8];
static bool    preerase_done;          // set by MSG_PREERASE handler
static const esp_partition_t *uf2_inactive;
static const esp_partition_t *uf2_active;

static bool sector_was_erased(uint32_t sector_idx)
{
    if (preerase_done) return true;
    if (sector_idx >= MAX_SECTORS) return false;
    return (sector_erased[sector_idx >> 3] >> (sector_idx & 7)) & 1;
}

static void mark_sector_erased(uint32_t sector_idx)
{
    if (sector_idx >= MAX_SECTORS) return;
    sector_erased[sector_idx >> 3] |= (uint8_t)(1u << (sector_idx & 7));
}

esp_err_t dfu_core_internal_uf2_start(void)
{
    uf2_inactive = esp_ota_get_next_update_partition(NULL);
    uf2_active = esp_ota_get_running_partition();
    if (!uf2_inactive || !uf2_active) {
        ESP_LOGE(TAG, "UF2: missing partition handles");
        return ESP_FAIL;
    }
    /* Refuse rather than truncate: a slot bigger than the bitmap would let
       sub-sector writes past the limit erase each other (see MAX_SECTORS). */
    const uint32_t need = uf2_inactive->size / FLASH_SECTOR_BYTES;
    if (need > MAX_SECTORS) {
        ESP_LOGE(TAG, "%s needs %" PRIu32 " erase-bitmap sectors, have %d — "
                      "raise MAX_SECTORS", uf2_inactive->label, need, MAX_SECTORS);
        return ESP_ERR_INVALID_SIZE;
    }
    // Clear per-sector erase tracking. preerase_done stays true if the
    // background pre-erase already wiped the partition — sector_was_erased
    // shortcuts on it. The write path falls through to lazy erase as soon
    // as we touch any sector, which is what we want for retries after a
    // partial UF2 stream too.
    memset(sector_erased, 0, sizeof(sector_erased));
    ESP_LOGI(TAG, "UF2 target: %s @ 0x%" PRIx32 " (running: %s @ 0x%" PRIx32 "), preerased=%d",
             uf2_inactive->label, uf2_inactive->address,
             uf2_active->label, uf2_active->address, (int)preerase_done);
    return ESP_OK;
}

esp_err_t dfu_core_internal_uf2_write(uint32_t target_addr,
                                      const void *payload, size_t len,
                                      bool *out_written)
{
    if (out_written) *out_written = false;
    if (!uf2_inactive || !uf2_active) return ESP_ERR_INVALID_STATE;

    // Translate the UF2 target address into an offset inside the inactive
    // partition. Two cases we accept; everything else is silently skipped:
    //   1. uf2-app: target lands inside the *running* (active) partition →
    //      mirror that offset into the inactive slot.
    //   2. someone constructed a UF2 already aimed at the inactive slot.
    uint32_t off;
    if (target_addr >= uf2_active->address &&
        target_addr + len <= uf2_active->address + uf2_active->size) {
        off = target_addr - uf2_active->address;
    } else if (target_addr >= uf2_inactive->address &&
               target_addr + len <= uf2_inactive->address + uf2_inactive->size) {
        off = target_addr - uf2_inactive->address;
    } else {
        // Bootloader / partition table / otadata blocks from a combined
        // uf2.bin land here. Not an error — out_written stays false so the
        // UF2 parser doesn't count this block toward app-image completion.
        return ESP_OK;
    }

    // Lazy per-sector erase. Mark every touched sector regardless of why
    // the erase was skipped — if we only mark when we actually erase, a
    // later write into the same sector (very common: payload is 256 B but
    // sectors are 4 KB, so 16 blocks share one sector) would think the
    // sector still needs erasing on a fresh bitmap and wipe our earlier
    // writes.
    uint32_t first_sector = off / FLASH_SECTOR_BYTES;
    uint32_t last_sector  = (off + len - 1) / FLASH_SECTOR_BYTES;
    for (uint32_t s = first_sector; s <= last_sector; s++) {
        if (!sector_was_erased(s)) {
            esp_err_t err = esp_partition_erase_range(
                uf2_inactive, s * FLASH_SECTOR_BYTES, FLASH_SECTOR_BYTES);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "erase sector %" PRIu32 ": %s", s, esp_err_to_name(err));
                return err;
            }
        }
        mark_sector_erased(s);
    }

    esp_err_t err = esp_partition_write(uf2_inactive, off, payload, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "partition_write @0x%" PRIx32 " (%zu B): %s",
                 off, len, esp_err_to_name(err));
        return err;
    }
    if (out_written) *out_written = true;
    return ESP_OK;
}

esp_err_t dfu_core_internal_uf2_finish(void)
{
    if (!uf2_inactive) return ESP_ERR_INVALID_STATE;
    esp_err_t err = esp_ota_set_boot_partition(uf2_inactive);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_boot_partition: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "image staged on %s, awaiting reboot", uf2_inactive->label);
    uf2_inactive = NULL;
    uf2_active = NULL;
    // After the partition flip on reboot, the *new* inactive (formerly
    // active) slot is dirty. The next post-boot pre-erase will clean it.
    preerase_done = false;
    return ESP_OK;
}

void dfu_core_internal_uf2_abort(void)
{
    uf2_inactive = NULL;
    uf2_active = NULL;
    // The inactive partition may be partially written. Clear both the
    // "fully erased" optimization flag AND the per-sector bitmap so the
    // next uf2_start re-erases each sector it actually touches — otherwise
    // we'd write onto dirty flash and the resulting image fails validation.
    preerase_done = false;
    memset(sector_erased, 0, sizeof(sector_erased));
    ESP_LOGW(TAG, "UF2 stream aborted");
}

// ---------------------------------------------------------------------------
// Background pre-erase (worker context)

static void preerase(void)
{
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        ESP_LOGW(TAG, "preerase: no inactive partition");
        return;
    }
    ESP_LOGI(TAG, "preerasing %s (0x%" PRIx32 " B) in background",
             part->label, part->size);
    esp_err_t err = esp_partition_erase_range(part, 0, part->size);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "preerase: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "preerase done");
    preerase_done = true;
}

// ---------------------------------------------------------------------------
// Timers: reboot, mark-valid

static void reboot_cb(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "rebooting into new firmware");
    esp_restart();
}

static void arm_reboot(uint32_t delay_ms)
{
    if (delay_ms == 0) return;
    const esp_timer_create_args_t args = {
        .callback = &reboot_cb,
        .name     = "ota_reboot",
    };
    esp_timer_handle_t t;
    if (esp_timer_create(&args, &t) == ESP_OK) {
        esp_timer_start_once(t, (uint64_t)delay_ms * 1000);
    }
}

static void mark_valid_timer_cb(void *arg)
{
    (void)arg;
    if (!queue) return;
    msg_t msg = { .op = MSG_MARK_VALID };
    xQueueSend(queue, &msg, 0);
}

// ---------------------------------------------------------------------------
// Periodic status log (bench debugging — independent of the wire protocol's
// PROGRESS frames). Runs on the esp_timer task; snapshots state under the
// dfu_core mutex briefly so a few u32s read consistently.

#if CONFIG_DFU_CORE_STATUS_LOG_PERIOD_MS > 0
static const char *src_name(dfu_source_t src)
{
    switch (src) {
    case DFU_SRC_USB_DFU: return "usb_dfu";
    case DFU_SRC_USB_CDC: return "cdc";
    case DFU_SRC_BLE:     return "ble";
    case DFU_SRC_MSC:     return "msc";
    case DFU_SRC_USB_HID: return "hid";
    case DFU_SRC_UDP:     return "udp";
    default:              return "none";
    }
}

static void status_log_cb(void *arg)
{
    (void)arg;
    if (!state_mtx) return;
    xSemaphoreTake(state_mtx, portMAX_DELAY);
    dfu_source_t src   = active_src;
    uint32_t     done  = bytes_written;
    uint32_t     total = image_total;
    const char  *label = ota_part ? ota_part->label
                       : (uf2_inactive ? uf2_inactive->label : "?");
    xSemaphoreGive(state_mtx);

    if (src == DFU_SRC_NONE) return;
    if (total > 0) {
        unsigned pct = (unsigned)((uint64_t)done * 100 / total);
        ESP_LOGI(TAG, "[%s] %" PRIu32 " / %" PRIu32 " B, %u%%, slot=%s",
                 src_name(src), done, total, pct, label);
    } else {
        ESP_LOGI(TAG, "[%s] %" PRIu32 " B written, slot=%s",
                 src_name(src), done, label);
    }
}
#endif

// ---------------------------------------------------------------------------
// Internal helpers: deliver and reset_session

static void deliver(dfu_event_t evt, esp_err_t err)
{
    if (active_cb) active_cb(active_src, evt, err, active_user);
}

static void reset_session(void)
{
    active_src = DFU_SRC_NONE;
    active_cb = NULL;
    active_user = NULL;
    mode = MODE_UNKNOWN;
    memset(&uf2_state, 0, sizeof(uf2_state));
    auto_completed = false;
    bytes_written = 0;
    image_total = 0;
#if CONFIG_DFU_CORE_DELTA_ENABLE
    // delta_handle itself is released on the abort/complete paths
    // (worker context); only the header cursor is reset here.
    delta_hdr_have = 0;
#endif
}
