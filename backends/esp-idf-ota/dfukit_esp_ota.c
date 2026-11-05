// The esp-idf-ota backend (dfukit_esp_ota.h)

#include "dfukit_esp_ota.h"

#include <stdio.h>
#include <string.h>

#include "dfukit_crc32.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_system.h"

#define DEFAULT_WINDOW (16 * 1024)
// The eraser's step: a flash block, erased at once (much faster per byte than its sectors)
#define BLOCK          (64 * 1024)
// The eraser runs below everything else; between blocks it lets the rest of the system run
#define ERASER_STACK   3072
#define ERASER_PRIO    1
#define READ_CHUNK     1024

static const char *TAG = "dfukit_esp_ota";

static dfukit_status_t ota_begin(void *ctx, const dfukit_start_t *start);
static dfukit_status_t ota_write(void *ctx, uint32_t offset, const uint8_t *data, size_t len);
static dfukit_status_t ota_finish(void *ctx, char *reason, size_t reason_size);
static dfukit_status_t ota_activate(void *ctx, uint8_t flags, uint16_t delay_ms);
static void ota_abort(void *ctx);
static uint32_t ota_window(void *ctx);
static void restart(void *arg);
static bool running_confirmed(void);
static esp_err_t flush(dfukit_esp_ota_t *ota);
static bool read_back_matches(dfukit_esp_ota_t *ota);
static esp_err_t erase_next(dfukit_esp_ota_t *ota);
static void eraser_task(void *arg);

/** @brief Fills in the backend for the next OTA slot. */
esp_err_t dfukit_esp_ota_init(dfukit_esp_ota_t *ota, dfukit_backend_t *backend, uint32_t board_id,
			      uint32_t window, uint8_t window_log2)
{
	const esp_partition_t *slot = esp_ota_get_next_update_partition(NULL);

	if (slot == NULL) {
		return ESP_ERR_NOT_FOUND;
	}
	memset(ota, 0, sizeof(*ota));
	ota->lock = xSemaphoreCreateMutex();
	if (ota->lock == NULL) {
		return ESP_ERR_NO_MEM;
	}
	ota->slot = slot;
	ota->window = window != 0 ? window : DEFAULT_WINDOW;
	memset(backend, 0, sizeof(*backend));
	backend->board_id = board_id;
	backend->kind = DFUKIT_KIND_IMAGE;
	backend->name = "esp-idf-ota";
	backend->max_size = slot->size;
	backend->encodings = DFUKIT_ENCODING_BIT(DFUKIT_ENCODING_RAW);
	if (window_log2 != 0) {
		backend->encodings |= DFUKIT_ENCODING_BIT(DFUKIT_ENCODING_DEFLATE);
	}
	backend->window_log2 = window_log2;
	backend->begin = ota_begin;
	backend->write = ota_write;
	backend->finish = ota_finish;
	backend->activate = ota_activate;
	backend->abort = ota_abort;
	backend->window = ota_window;
	backend->ctx = ota;
	return ESP_OK;
}

/** @brief Starts the eraser, or leaves it for the next update if one is writing now. */
esp_err_t dfukit_esp_ota_preerase(dfukit_esp_ota_t *ota)
{
	if (!running_confirmed()) {
		return ESP_ERR_INVALID_STATE;
	}
	if (ota->eraser == NULL && xTaskCreate(eraser_task, "dfukit_erase", ERASER_STACK, ota,
					       ERASER_PRIO, &ota->eraser) != pdPASS) {
		return ESP_ERR_NO_MEM;
	}
	xSemaphoreTake(ota->lock, portMAX_DELAY);
	if (ota->open || ota->holds_image) {
		// The slot is an update's now, also once FINISH closed it: its image must not go.
		// Pre-erased from the next update on
		ota->preerase_asked = true;
	} else {
		ota->preerase = true;
		xTaskNotifyGive(ota->eraser);
	}
	xSemaphoreGive(ota->lock);
	return ESP_OK;
}

/** @brief START: gets the next OTA slot ready, sequentially or pre-erased. */
static dfukit_status_t ota_begin(void *ctx, const dfukit_start_t *start)
{
	dfukit_esp_ota_t *ota = ctx;

	(void)start;
	// The slot holds the image activated a moment ago: the next update slot is still that one
	// (the running image has not changed), and a begin would erase what is to boot
	if (ota->activated) {
		ESP_LOGW(TAG, "an image is activated: no update before the restart");
		return DFUKIT_BUSY;
	}
	// An update broken off, and a pre-erase asked for during it
	xSemaphoreTake(ota->lock, portMAX_DELAY);
	if (ota->open) {
		esp_ota_abort(ota->handle);
		ota->open = false;
	}
	ota->holds_image = false;
	if (ota->preerase_asked) {
		ota->preerase = true;
		ota->preerase_asked = false;
	}
	xSemaphoreGive(ota->lock);
	ota->slot = esp_ota_get_next_update_partition(NULL);
	if (ota->slot == NULL) {
		return DFUKIT_BACKEND;
	}
	if (ota->preerase) {
		// As esp_ota_begin(): not over the image a rollback needs
		if (!running_confirmed()) {
			ESP_LOGE(TAG, "the running image is not confirmed yet");
			return DFUKIT_BACKEND;
		}
		// What the last update left (broken off) is checked again from the start: blank
		// blocks are skipped
		xSemaphoreTake(ota->lock, portMAX_DELAY);
		ota->erased = 0;
		ota->written = 0;
		ota->buffered = 0;
		ota->crc = 0;
		xSemaphoreGive(ota->lock);
		xTaskNotifyGive(ota->eraser);
		return DFUKIT_OK;
	}
	xSemaphoreTake(ota->lock, portMAX_DELAY);
	esp_err_t err = esp_ota_begin(ota->slot, OTA_WITH_SEQUENTIAL_WRITES, &ota->handle);

	ota->open = err == ESP_OK;
	xSemaphoreGive(ota->lock);
	// Says why, which BACKEND does not: a running image not confirmed yet, for one
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
		return DFUKIT_BACKEND;
	}
	return DFUKIT_OK;
}

/** @brief The image, in order: through ESP-IDF's OTA, or gathered and written pre-erased. */
static dfukit_status_t ota_write(void *ctx, uint32_t offset, const uint8_t *data, size_t len)
{
	dfukit_esp_ota_t *ota = ctx;

	if (!ota->preerase) {
		return esp_ota_write(ota->handle, data, len) == ESP_OK ? DFUKIT_OK : DFUKIT_BACKEND;
	}
	// As esp_ota_write(): an app image starts with its magic byte
	if (offset == 0 && len > 0 && data[0] != ESP_IMAGE_HEADER_MAGIC) {
		ESP_LOGE(TAG, "not an app image (0x%02x)", data[0]);
		return DFUKIT_BACKEND;
	}
	if (offset + len > ota->slot->size || offset != ota->written + ota->buffered) {
		return DFUKIT_BACKEND;
	}
	ota->crc = dfukit_crc32(ota->crc, data, len);
	while (len > 0) {
		size_t room = sizeof(ota->buffer) - ota->buffered;
		size_t piece = room < len ? room : len;

		memcpy(ota->buffer + ota->buffered, data, piece);
		ota->buffered += piece;
		data += piece;
		len -= piece;
		if (ota->buffered == sizeof(ota->buffer) && flush(ota) != ESP_OK) {
			return DFUKIT_BACKEND;
		}
	}
	return DFUKIT_OK;
}

/** @brief What is gathered, to the flash at written; blocks the eraser has not reached go first. */
static esp_err_t flush(dfukit_esp_ota_t *ota)
{
	uint32_t end = ota->written + ota->buffered;

	while (end > ota->erased) {
		xSemaphoreTake(ota->lock, portMAX_DELAY);
		esp_err_t err = end > ota->erased ? erase_next(ota) : ESP_OK;

		xSemaphoreGive(ota->lock);
		if (err != ESP_OK) {
			return err;
		}
	}
	esp_err_t err = esp_partition_write(ota->slot, ota->written, ota->buffer, ota->buffered);

	if (err == ESP_OK) {
		ota->written = end;
		ota->buffered = 0;
	}
	return err;
}

/** @brief FINISH: ESP-IDF's check of the image, and pre-erased the slot read back. */
static dfukit_status_t ota_finish(void *ctx, char *reason, size_t reason_size)
{
	dfukit_esp_ota_t *ota = ctx;
	esp_err_t err;

	if (ota->preerase) {
		// What esp_ota_end() checks: the image, its hash, its signature
		const esp_partition_pos_t position = {.offset = ota->slot->address,
						      .size = ota->slot->size};
		esp_image_metadata_t metadata;

		// The image's last piece first
		err = ota->buffered > 0 ? flush(ota) : ESP_OK;
		if (err == ESP_OK) {
			err = esp_image_verify(ESP_IMAGE_VERIFY, &position, &metadata);
		}
		if (err == ESP_OK && !read_back_matches(ota)) {
			snprintf(reason, reason_size, "the slot reads back wrong");
			return DFUKIT_BACKEND;
		}
	} else {
		xSemaphoreTake(ota->lock, portMAX_DELAY);
		err = esp_ota_end(ota->handle);
		ota->open = false;
		ota->holds_image = err == ESP_OK;
		xSemaphoreGive(ota->lock);
	}
	if (err != ESP_OK) {
		snprintf(reason, reason_size, "image check: %s", esp_err_to_name(err));
		return DFUKIT_BACKEND;
	}
	return DFUKIT_OK;
}

/** @brief ACTIVATE: the slot is the boot image; a restart follows, unless at the next boot. */
static dfukit_status_t ota_activate(void *ctx, uint8_t flags, uint16_t delay_ms)
{
	dfukit_esp_ota_t *ota = ctx;

	if (esp_ota_set_boot_partition(ota->slot) != ESP_OK) {
		return DFUKIT_BACKEND;
	}
	ota->activated = true;
	if (flags & DFUKIT_ACTIVATE_AT_NEXT_BOOT) {
		return DFUKIT_OK;
	}
	// Later, from a timer: ACTIVATE's answer goes out first
	if (ota->restart == NULL) {
		const esp_timer_create_args_t args = {.callback = restart,
						      .name = "dfukit restart"};

		if (esp_timer_create(&args, &ota->restart) != ESP_OK) {
			esp_restart();
		}
	}
	esp_timer_start_once(ota->restart, (uint64_t)(delay_ms > 0 ? delay_ms : 100) * 1000);
	return DFUKIT_OK;
}

/** @brief The timer of a restart that ACTIVATE asked for. */
static void restart(void *arg)
{
	(void)arg;
	esp_restart();
}

/** @brief The session is broken off: an open OTA handle is closed. */
static void ota_abort(void *ctx)
{
	dfukit_esp_ota_t *ota = ctx;

	xSemaphoreTake(ota->lock, portMAX_DELAY);
	if (ota->open) {
		esp_ota_abort(ota->handle);
		ota->open = false;
	}
	ota->holds_image = false;
	xSemaphoreGive(ota->lock);
}

/** @brief What it can take: always the window it was set up with. */
static uint32_t ota_window(void *ctx)
{
	dfukit_esp_ota_t *ota = ctx;

	return ota->window;
}

/** @brief Whether the running image is not waiting for its confirmation. */
static bool running_confirmed(void)
{
	esp_ota_img_states_t state;

	return esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) != ESP_OK ||
	       state != ESP_OTA_IMG_PENDING_VERIFY;
}

/** @brief The slot as the flash holds it (not through the cache), against what was written. */
static bool read_back_matches(dfukit_esp_ota_t *ota)
{
	static uint8_t chunk[READ_CHUNK];
	uint32_t crc = 0;

	for (uint32_t offset = 0; offset < ota->written; offset += READ_CHUNK) {
		uint32_t left = ota->written - offset;
		uint32_t length = left < READ_CHUNK ? left : READ_CHUNK;

		if (esp_partition_read(ota->slot, offset, chunk, length) != ESP_OK) {
			return false;
		}
		crc = dfukit_crc32(crc, chunk, length);
	}
	if (crc != ota->crc) {
		ESP_LOGE(TAG, "the slot reads back wrong (written %u, erased %u)",
			 (unsigned int)ota->written, (unsigned int)ota->erased);
		return false;
	}
	return true;
}

/** @brief The block at erased, unless it is blank already. Under the lock. */
static esp_err_t erase_next(dfukit_esp_ota_t *ota)
{
	static uint32_t words[READ_CHUNK / 4];
	uint32_t block = ota->erased;

	// Never under what this update has written
	if (block < ota->written) {
		ESP_LOGE(TAG, "erase at 0x%x, under the written 0x%x", (unsigned int)block,
			 (unsigned int)ota->written);
		return ESP_ERR_INVALID_STATE;
	}
	uint32_t size = ota->slot->size - block < BLOCK ? ota->slot->size - block : BLOCK;
	bool blank = true;

	// A blank block is left alone: reading it is much faster than erasing it
	for (uint32_t offset = 0; offset < size && blank; offset += READ_CHUNK) {
		if (esp_partition_read(ota->slot, block + offset, words, READ_CHUNK) != ESP_OK) {
			blank = false;
			break;
		}
		for (size_t word = 0; word < READ_CHUNK / 4 && blank; word++) {
			blank = words[word] == 0xffffffff;
		}
	}
	if (!blank) {
		esp_err_t err = esp_partition_erase_range(ota->slot, block, size);

		if (err != ESP_OK) {
			ESP_LOGE(TAG, "erase at 0x%x: %s", (unsigned int)block,
				 esp_err_to_name(err));
			return err;
		}
	}
	ota->erased = block + size;
	return ESP_OK;
}

/** @brief The slot, a block at a time, until blank; again from the start when an update begins. */
static void eraser_task(void *arg)
{
	dfukit_esp_ota_t *ota = arg;

	for (;;) {
		ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
		int64_t start = esp_timer_get_time();
		esp_err_t err = ESP_OK;

		while (err == ESP_OK && ota->erased < ota->slot->size) {
			xSemaphoreTake(ota->lock, portMAX_DELAY);
			err = ota->erased < ota->slot->size ? erase_next(ota) : ESP_OK;
			xSemaphoreGive(ota->lock);
			vTaskDelay(1);
		}
		// A failed erase is logged where it failed, and writes erase what they reach
		if (err == ESP_OK) {
			ESP_LOGI(TAG, "the other slot is blank (%u ms)",
				 (unsigned int)((esp_timer_get_time() - start) / 1000));
		}
	}
}
