// The mcuboot-recovery backend: an update inside MCUboot (Zephyr), into its application slot.
// START's first block is erased with a window of 0; then the image is written as it comes, the
// rest erased ahead of it, a block per dfukit_mcuboot_step() between frames, or by the write
// that gets there first. An erase stops the CPU's interrupts, never the window's bytes: the
// receive buffers must take a window each (recovery.c). FINISH runs MCUboot's check of the image;
// a bad one leaves the device in recovery for another try.

#ifndef DFUKIT_MCUBOOT_H
#define DFUKIT_MCUBOOT_H

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/storage/flash_map.h>

#include "dfukit_server.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	const struct flash_area *area;
	// Erased up to erased, to be up to erase_to; written up to written
	uint32_t erase_to;
	uint32_t erased;
	uint32_t written;
	uint32_t window;
	// The image gathered into whole write units of the flash (flash_area_align()), written
	// when full and at FINISH: written is what is in the flash
	uint8_t buffer[1024];
	size_t buffered;
	uint32_t align;
	// ACTIVATE's restart, due at restart_at (dfukit_mcuboot_step())
	bool restart;
	int64_t restart_at;
} dfukit_mcuboot_t;

/**
 * @brief Sets up a backend over a flash area
 * @param mcuboot The backend's state, which the application owns
 * @param backend What goes to dfukit_server_register()
 * @param board_id The board ID it takes
 * @param area_id The flash area: MCUboot's slot 0
 * @param window The window it announces
 * @param window_log2 The deflate window the application's inflater takes, for INFO; 0 for raw only
 */
int dfukit_mcuboot_init(dfukit_mcuboot_t *mcuboot, dfukit_backend_t *backend, uint32_t board_id,
			uint8_t area_id, uint32_t window, uint8_t window_log2);

/**
 * @brief Between frames, from the recovery's loop: a block of the erase if one is due, and
 *        ACTIVATE's restart once its delay is over
 */
void dfukit_mcuboot_step(dfukit_mcuboot_t *mcuboot);

#ifdef __cplusplus
}
#endif

#endif // DFUKIT_MCUBOOT_H
