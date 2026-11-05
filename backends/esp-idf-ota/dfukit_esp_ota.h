// The esp-idf-ota backend: the image into the next OTA slot. FINISH checks it (the image's format,
// and its signature with signed app images); ACTIVATE makes it the boot image and restarts. With
// rollback on, the new image confirms itself once it is up
// (esp_ota_mark_app_valid_cancel_rollback()), else the bootloader goes back.
//
// The slot is erased as it is written, a sector at a time; or, after dfukit_esp_ota_preerase(),
// ahead of time: once the running image is confirmed the other slot holds only the image before
// it, which no rollback needs, and it is erased in the background. An update then writes without
// waiting for erases (any block not erased yet is erased as it is reached).

#ifndef DFUKIT_ESP_OTA_H
#define DFUKIT_ESP_OTA_H

#include <stdbool.h>
#include <stdint.h>

#include "dfukit_server.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	const esp_partition_t *slot;
	esp_ota_handle_t handle;
	bool open;
	// FINISH closed the handle with the image good: the slot holds it, ACTIVATE is to come
	bool holds_image;
	// The window it announces: what the transport buffers while a write erases
	uint32_t window;
	esp_timer_handle_t restart;
	// ACTIVATE done: the slot is the next boot's, until a restart; no update may begin on it
	bool activated;
	// Pre-erased: the slot is blank up to erased (a block at a time, from the start); the
	// eraser and the writes take turns by the lock. Asked for while the slot is an update's,
	// written the sequential way: from the next update on
	bool preerase;
	bool preerase_asked;
	volatile uint32_t erased;
	uint32_t written;
	// The CRC of what was written, to read the slot back against at FINISH
	uint32_t crc;
	// Pre-erased: the image gathered into whole 4 KiB pieces (16 flash pages), each written at
	// once; written is what is in the flash. A write of a frame's few hundred bytes costs
	// nearly as much as one of 4 KiB
	uint8_t buffer[4096];
	size_t buffered;
	SemaphoreHandle_t lock;
	TaskHandle_t eraser;
} dfukit_esp_ota_t;

/**
 * @brief Sets up a backend over the next OTA slot
 * @param ota The backend's state, which the application owns
 * @param backend What goes to dfukit_server_register()
 * @param board_id The board ID it takes
 * @param window The window it announces; 0 for 16 KiB
 * @param window_log2 The deflate window the application's inflater takes, for INFO; 0 for raw only
 */
esp_err_t dfukit_esp_ota_init(dfukit_esp_ota_t *ota, dfukit_backend_t *backend, uint32_t board_id,
			      uint32_t window, uint8_t window_log2);

/**
 * @brief Erases the other slot in the background; from then on updates write it without erasing.
 *        Only once the running image is confirmed: until then that slot is the rollback's. From
 *        any task
 */
esp_err_t dfukit_esp_ota_preerase(dfukit_esp_ota_t *ota);

#ifdef __cplusplus
}
#endif

#endif // DFUKIT_ESP_OTA_H
