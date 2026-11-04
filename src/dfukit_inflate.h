// The deflate decoder for the server (dfukit_server_config_t.inflater): raw deflate (RFC 1951),
// no zlib or gzip header, decoded with miniz's tinfl as the stream comes

#ifndef DFUKIT_INFLATE_H
#define DFUKIT_INFLATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dfukit_server.h"
#include "miniz_tinfl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	tinfl_decompressor decompressor;
	// The window: the last bytes decoded, which later ones refer back to
	uint8_t *dict;
	size_t dict_size;
	size_t dict_used;
	bool done;
} dfukit_inflate_t;

/**
 * @brief Sets up a decoder, and the inflater that gives the server access to it
 * @param inflate The decoder's state, which the application owns
 * @param inflater What goes in the server's config
 * @param dict The window: a power of two, at least the stream's window. 32 KiB takes any deflate
 *             stream; a host compresses with a smaller window for a smaller one, as the backend's
 *             window_log2 announces
 * @param dict_size The size of dict in bytes
 */
void dfukit_inflate_init(dfukit_inflate_t *inflate, dfukit_inflater_t *inflater, uint8_t *dict,
			 size_t dict_size);

#ifdef __cplusplus
}
#endif

#endif // DFUKIT_INFLATE_H
