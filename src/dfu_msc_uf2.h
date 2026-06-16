#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* MSC UF2 transport adapter for dfu_core.
 *
 * Extracts the UF2 sniff-and-feed logic from the USB-MSC write callback.
 * Has NO TinyUSB dependency — it only takes raw 512-byte sector data and
 * forwards UF2 blocks into dfu_core.
 *
 * Integration:
 *   1. Call dfu_msc_uf2_register() once from app_main after dfu_core_init().
 *   2. In tud_msc_write10_cb, loop over 512-byte sectors calling
 *      dfu_msc_uf2_write(sector, 512).
 *   3. In tud_mount_cb, call dfu_msc_uf2_reset() to clear the done latch
 *      so a fresh drag-drop can retry after a failed OTA.
 */

/* Initialise internal state. Call once after dfu_core_init(). */
void dfu_msc_uf2_register(void);

/* Process one 512-byte sector from the USB-MSC write10 callback.
 * Sniffs "UF2\n" magic, acquires dfu_core on the first UF2 sector,
 * feeds subsequent blocks, and handles the terminal done latch.
 * Non-UF2 sectors (FAT metadata, directory entries) are silently ignored. */
void dfu_msc_uf2_write(const uint8_t *sector, size_t len);

/* Clear the done latch. Call on USB mount so a new drag-drop can be
 * attempted after a previous OTA success or failure. */
void dfu_msc_uf2_reset(void);

#ifdef __cplusplus
}
#endif
