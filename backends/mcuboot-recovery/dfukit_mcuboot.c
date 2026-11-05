// The mcuboot-recovery backend (dfukit_mcuboot.h)

#include "dfukit_mcuboot.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/drivers/flash.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>

#include "bootutil/fault_injection_hardening.h"
#include "bootutil/image.h"

// A NOR flash's block: erased at once much faster than its sectors one by one
#define BLOCK (64 * 1024)

static dfukit_status_t mcuboot_begin(void *ctx, const dfukit_start_t *start);
static dfukit_status_t mcuboot_write(void *ctx, uint32_t offset, const uint8_t *data, size_t len);
static dfukit_status_t mcuboot_finish(void *ctx, char *reason, size_t reason_size);
static dfukit_status_t mcuboot_activate(void *ctx, uint8_t flags, uint16_t delay_ms);
static uint32_t mcuboot_window(void *ctx);
static uint32_t sector_at(dfukit_mcuboot_t *mcuboot, uint32_t offset);
static int flush(dfukit_mcuboot_t *mcuboot, size_t len);

/** @brief Fills in the backend for a flash area. */
int dfukit_mcuboot_init(dfukit_mcuboot_t *mcuboot, dfukit_backend_t *backend, uint32_t board_id,
			uint8_t area_id, uint32_t window, uint8_t window_log2)
{
	int err = flash_area_open(area_id, &mcuboot->area);

	if (err != 0) {
		return err;
	}
	mcuboot->erase_to = 0;
	mcuboot->erased = 0;
	mcuboot->written = 0;
	mcuboot->window = window;
	mcuboot->restart = false;
	mcuboot->buffered = 0;
	// The flash's write unit; the buffer is a whole number of them
	mcuboot->align = flash_area_align(mcuboot->area);
	if (mcuboot->align == 0 || sizeof(mcuboot->buffer) % mcuboot->align != 0) {
		return -EINVAL;
	}
	memset(backend, 0, sizeof(*backend));
	backend->board_id = board_id;
	backend->kind = DFUKIT_KIND_IMAGE;
	backend->name = "mcuboot";
	backend->max_size = (uint32_t)mcuboot->area->fa_size;
	backend->encodings = DFUKIT_ENCODING_BIT(DFUKIT_ENCODING_RAW);
	if (window_log2 != 0) {
		backend->encodings |= DFUKIT_ENCODING_BIT(DFUKIT_ENCODING_DEFLATE);
	}
	backend->window_log2 = window_log2;
	backend->begin = mcuboot_begin;
	backend->write = mcuboot_write;
	backend->finish = mcuboot_finish;
	backend->activate = mcuboot_activate;
	backend->window = mcuboot_window;
	backend->ctx = mcuboot;
	return 0;
}

/** @brief A block of the erase if one is due, and ACTIVATE's restart once its delay is over. */
void dfukit_mcuboot_step(dfukit_mcuboot_t *mcuboot)
{
	if (mcuboot->restart && k_uptime_get() >= mcuboot->restart_at) {
		sys_reboot(SYS_REBOOT_COLD);
	}
	if (mcuboot->erased >= mcuboot->erase_to) {
		return;
	}
	// A block where one fits whole (the driver erases it at once), else a sector
	uint32_t address = mcuboot->area->fa_off + mcuboot->erased;
	uint32_t size = address % BLOCK == 0 && mcuboot->erase_to - mcuboot->erased >= BLOCK
				? BLOCK
				: sector_at(mcuboot, mcuboot->erased);

	if (flash_area_erase(mcuboot->area, mcuboot->erased, size) != 0) {
		// Not erased: the writes there fail, and the session with them
		mcuboot->erase_to = mcuboot->erased;
		return;
	}
	mcuboot->erased += size;
}

/** @brief START: how far to erase, up to the end of the sector the image ends in. */
static dfukit_status_t mcuboot_begin(void *ctx, const dfukit_start_t *start)
{
	dfukit_mcuboot_t *mcuboot = ctx;
	uint32_t end = 0;

	while (end < start->image_size) {
		end += sector_at(mcuboot, end);
	}
	mcuboot->erase_to = end;
	mcuboot->erased = 0;
	mcuboot->written = 0;
	mcuboot->buffered = 0;
	return DFUKIT_OK;
}

/** @brief The image, in order: gathered, and written when a buffer is full. */
static dfukit_status_t mcuboot_write(void *ctx, uint32_t offset, const uint8_t *data, size_t len)
{
	dfukit_mcuboot_t *mcuboot = ctx;

	if (offset != mcuboot->written + mcuboot->buffered) {
		return DFUKIT_BACKEND;
	}
	while (len > 0) {
		size_t room = sizeof(mcuboot->buffer) - mcuboot->buffered;
		size_t piece = room < len ? room : len;

		memcpy(mcuboot->buffer + mcuboot->buffered, data, piece);
		mcuboot->buffered += piece;
		data += piece;
		len -= piece;
		if (mcuboot->buffered == sizeof(mcuboot->buffer) &&
		    flush(mcuboot, mcuboot->buffered) != 0) {
			return DFUKIT_BACKEND;
		}
	}
	return DFUKIT_OK;
}

/**
 * @brief len of what is gathered (whole write units) to the flash at written; erased first if the
 *        eraser is not there yet (a deflated frame decodes to much)
 */
static int flush(dfukit_mcuboot_t *mcuboot, size_t len)
{
	while (mcuboot->written + len > mcuboot->erased && mcuboot->erased < mcuboot->erase_to) {
		dfukit_mcuboot_step(mcuboot);
	}
	if (mcuboot->written + len > mcuboot->erased) {
		return -EIO;
	}
	int err = flash_area_write(mcuboot->area, mcuboot->written, mcuboot->buffer, len);

	if (err == 0) {
		// What is left (none after the padded last piece)
		size_t rest = mcuboot->buffered > len ? mcuboot->buffered - len : 0;

		memmove(mcuboot->buffer, mcuboot->buffer + len, rest);
		mcuboot->buffered = rest;
		mcuboot->written += (uint32_t)len;
	}
	return err;
}

/** @brief FINISH: the image's last piece, then MCUboot's own check of it. */
static dfukit_status_t mcuboot_finish(void *ctx, char *reason, size_t reason_size)
{
	dfukit_mcuboot_t *mcuboot = ctx;
	struct image_header header;
	static uint8_t scratch[256];

	// The image's last piece, padded to a whole write unit with the erased value. The buffer is
	// a whole number of them, so the padding always fits
	if (mcuboot->buffered > 0) {
		size_t len =
			(mcuboot->buffered + mcuboot->align - 1) / mcuboot->align * mcuboot->align;

		memset(mcuboot->buffer + mcuboot->buffered, flash_area_erased_val(mcuboot->area),
		       len - mcuboot->buffered);
		if (flush(mcuboot, len) != 0) {
			snprintf(reason, reason_size, "the image's end not written");
			return DFUKIT_BACKEND;
		}
	}
	if (flash_area_read(mcuboot->area, 0, &header, sizeof(header)) != 0 ||
	    header.ih_magic != IMAGE_MAGIC) {
		snprintf(reason, reason_size, "not an MCUboot image");
		return DFUKIT_BACKEND;
	}
	// What MCUboot checks at boot, signature included: the host hears now if it will not boot
	FIH_DECLARE(valid, FIH_FAILURE);
	FIH_CALL(bootutil_img_validate, valid, NULL, &header, mcuboot->area, scratch,
		 sizeof(scratch), NULL, 0, NULL);
	if (FIH_NOT_EQ(valid, FIH_SUCCESS)) {
		snprintf(reason, reason_size, "MCUboot would not boot it (hash or signature)");
		return DFUKIT_BACKEND;
	}
	return DFUKIT_OK;
}

/**
 * @brief ACTIVATE: a reset into MCUboot's normal boot, after delay_ms, from
 *        dfukit_mcuboot_step(), so the answer goes out first
 */
static dfukit_status_t mcuboot_activate(void *ctx, uint8_t flags, uint16_t delay_ms)
{
	dfukit_mcuboot_t *mcuboot = ctx;

	if (!(flags & DFUKIT_ACTIVATE_AT_NEXT_BOOT)) {
		mcuboot->restart = true;
		mcuboot->restart_at = k_uptime_get() + (delay_ms > 0 ? delay_ms : 10);
	}
	return DFUKIT_OK;
}

/** @brief Nothing until the first block is erased; then the window, the rest erased ahead. */
static uint32_t mcuboot_window(void *ctx)
{
	dfukit_mcuboot_t *mcuboot = ctx;

	return mcuboot->erased == 0 && mcuboot->erase_to > 0 ? 0 : mcuboot->window;
}

/** @brief The erase unit at offset (a sector of a NOR flash). */
static uint32_t sector_at(dfukit_mcuboot_t *mcuboot, uint32_t offset)
{
	struct flash_pages_info info;

	if (flash_get_page_info_by_offs(flash_area_get_device(mcuboot->area),
					mcuboot->area->fa_off + offset, &info) != 0) {
		return 4096;
	}
	return (uint32_t)info.size;
}
