// The ram backend: takes the image into a buffer the application gives it. For tests, and for
// a device that handles the image itself once it is in

#ifndef DFUKIT_RAM_H
#define DFUKIT_RAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dfukit_server.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	uint8_t *buf;
	size_t size;
	// The window it announces (0: the room left in buf); busy: none, as a backend that erases
	uint32_t window;
	bool busy;
	// FINISH answered PENDING this many times first; ACTIVATE refused (as a failed switch)
	unsigned pending;
	bool refuse_activate;
	// What it holds: the image's bytes written; set by FINISH and ACTIVATE
	size_t written;
	bool finished;
	bool activated;
	uint8_t activate_flags;
	// Counts, for tests
	unsigned begins;
	unsigned aborts;
} dfukit_ram_t;

/** @brief A backend for board_id over buf; the application owns ram and backend, and they must outlive the server */
void dfukit_ram_init(dfukit_ram_t *ram, dfukit_backend_t *backend, uint32_t board_id, uint8_t *buf,
		     size_t size);

#ifdef __cplusplus
}
#endif

#endif // DFUKIT_RAM_H
