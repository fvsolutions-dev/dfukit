// The ram backend (dfukit_ram.h)

#include "dfukit_ram.h"

#include <string.h>

/** @brief An image begins: what was there is forgotten. */
static dfukit_status_t ram_begin(void *ctx, const dfukit_start_t *start)
{
	dfukit_ram_t *ram = ctx;

	(void)start;
	ram->written = 0;
	ram->finished = false;
	ram->activated = false;
	ram->begins++;
	return DFUKIT_OK;
}

/** @brief Copies the bytes in; they must come in order and fit. */
static dfukit_status_t ram_write(void *ctx, uint32_t offset, const uint8_t *data, size_t len)
{
	dfukit_ram_t *ram = ctx;

	if (offset != ram->written || len > ram->size - ram->written) {
		return DFUKIT_BACKEND;
	}
	memcpy(ram->buf + offset, data, len);
	ram->written += len;
	return DFUKIT_OK;
}

/** @brief The image is in: PENDING as often as the application asked for, then OK. */
static dfukit_status_t ram_finish(void *ctx, char *reason, size_t reason_size)
{
	dfukit_ram_t *ram = ctx;

	(void)reason;
	(void)reason_size;
	if (ram->pending > 0) {
		ram->pending--;
		return DFUKIT_PENDING;
	}
	ram->finished = true;
	return DFUKIT_OK;
}

/** @brief Notes the activation, unless the application asked for a refusal. */
static dfukit_status_t ram_activate(void *ctx, uint8_t flags, uint16_t delay_ms)
{
	dfukit_ram_t *ram = ctx;

	(void)delay_ms;
	if (ram->refuse_activate) {
		return DFUKIT_BACKEND;
	}
	ram->activated = true;
	ram->activate_flags = flags;
	return DFUKIT_OK;
}

/** @brief Counts the abort. */
static void ram_abort(void *ctx)
{
	dfukit_ram_t *ram = ctx;

	ram->aborts++;
}

/** @brief What it can take: 0 while told to be busy, then the window set, or all the room left. */
static uint32_t ram_window(void *ctx)
{
	dfukit_ram_t *ram = ctx;

	if (ram->busy) {
		return 0;
	}
	return ram->window != 0 ? ram->window : (uint32_t)(ram->size - ram->written);
}

/** @brief Fills in the backend for a buffer of the application's. */
void dfukit_ram_init(dfukit_ram_t *ram, dfukit_backend_t *backend, uint32_t board_id, uint8_t *buf,
		     size_t size)
{
	memset(ram, 0, sizeof(*ram));
	ram->buf = buf;
	ram->size = size;
	memset(backend, 0, sizeof(*backend));
	backend->board_id = board_id;
	backend->kind = DFUKIT_KIND_IMAGE;
	backend->name = "ram";
	backend->max_size = (uint32_t)size;
	backend->encodings = DFUKIT_ENCODING_BIT(DFUKIT_ENCODING_RAW) |
			     DFUKIT_ENCODING_BIT(DFUKIT_ENCODING_DEFLATE);
	backend->window_log2 = 15;
	backend->begin = ram_begin;
	backend->write = ram_write;
	backend->finish = ram_finish;
	backend->activate = ram_activate;
	backend->abort = ram_abort;
	backend->window = ram_window;
	backend->ctx = ram;
}
