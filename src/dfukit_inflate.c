// The deflate decoder (dfukit_inflate.h)

#include "dfukit_inflate.h"

/** @brief A new stream begins: nothing decoded, an empty window. */
static void inflate_reset(void *context)
{
	dfukit_inflate_t *inflate = context;

	tinfl_init(&inflate->decompressor);
	inflate->dict_used = 0;
	inflate->done = false;
}

/** @brief Decodes all of input, handing what comes out to sink. */
static dfukit_status_t inflate_feed(void *context, const uint8_t *input, size_t len,
				    dfukit_sink_fn sink, void *sink_context)
{
	dfukit_inflate_t *inflate = context;

	for (;;) {
		// Bytes after the last block are not part of the stream
		if (inflate->done) {
			return len == 0 ? DFUKIT_OK : DFUKIT_BAD_STREAM;
		}
		size_t input_len = len;
		size_t output_len = inflate->dict_size - inflate->dict_used;
		tinfl_status status = tinfl_decompress(
			&inflate->decompressor, input, &input_len, inflate->dict,
			inflate->dict + inflate->dict_used, &output_len, TINFL_FLAG_HAS_MORE_INPUT);

		input += input_len;
		len -= input_len;
		// What came out goes on, and stays in the window for what refers back to it
		if (output_len > 0) {
			dfukit_status_t sunk =
				sink(sink_context, inflate->dict + inflate->dict_used, output_len);

			if (sunk != DFUKIT_OK) {
				return sunk;
			}
			inflate->dict_used =
				(inflate->dict_used + output_len) & (inflate->dict_size - 1);
		}
		if (status == TINFL_STATUS_DONE) {
			inflate->done = true;
		} else if (status < TINFL_STATUS_DONE) {
			return DFUKIT_BAD_STREAM;
		} else if (status == TINFL_STATUS_NEEDS_MORE_INPUT && len == 0) {
			return DFUKIT_OK;
		}
	}
}

/** @brief Whether the stream's last block has been decoded. */
static bool inflate_done(void *context)
{
	dfukit_inflate_t *inflate = context;

	return inflate->done;
}

void dfukit_inflate_init(dfukit_inflate_t *inflate, dfukit_inflater_t *inflater, uint8_t *dict,
			 size_t dict_size)
{
	inflate->dict = dict;
	inflate->dict_size = dict_size;
	inflate_reset(inflate);
	inflater->reset = inflate_reset;
	inflater->feed = inflate_feed;
	inflater->done = inflate_done;
	inflater->ctx = inflate;
}
