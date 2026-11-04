// dfukit 2's server: the device's end (DESIGN.md). Driven by the application: frames in with
// dfukit_server_feed(), frames out through the send callback, time in with dfukit_server_poll().
// No threads, no heap: all state is in dfukit_server_t, which the application owns. Not
// reentrant: one task calls into a server at a time.

#ifndef DFUKIT_SERVER_H
#define DFUKIT_SERVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dfukit.h"

#ifdef __cplusplus
extern "C" {
#endif

// The decoded image's bytes, in order, to wherever they go: what an inflater hands on
typedef dfukit_status_t (*dfukit_sink_fn)(void *ctx, const uint8_t *data, size_t len);

// A decoder for one encoding (deflate): fed the stream as it comes, in pieces of any size
typedef struct {
	// A new stream begins
	void (*reset)(void *ctx);
	// As much of input as decodes, to sink. DFUKIT_OK, DFUKIT_BAD_STREAM, or what sink returned
	dfukit_status_t (*feed)(void *ctx, const uint8_t *input, size_t len, dfukit_sink_fn sink,
				void *sink_ctx);
	// The stream has ended (its last block decoded)
	bool (*done)(void *ctx);
	void *ctx;
} dfukit_inflater_t;

typedef struct dfukit_backend dfukit_backend_t;

// What puts an image in place, for the board IDs it registers for (one backend, one ID: register
// a copy per ID). An image backend gets the decoded image in order; a pass-through backend gets
// the session's frames and hands the downstream device's back with dfukit_server_downstream()
struct dfukit_backend {
	uint32_t board_id;
	uint8_t kind; // DFUKIT_KIND_IMAGE or DFUKIT_KIND_PASSTHROUGH
	const char *name;
	// The running image's version, for INFO (optional)
	const char *version;
	uint32_t max_size;
	// What it takes, for INFO: encodings (DFUKIT_ENCODING_BIT), the largest deflate window
	// (log2)
	uint8_t encodings;
	uint8_t window_log2;

	// An image backend's. begin may take long (an erase): the session's first window comes
	// after it. write gets the image at offset, in order
	dfukit_status_t (*begin)(void *ctx, const dfukit_start_t *start);
	dfukit_status_t (*write)(void *ctx, uint32_t offset, const uint8_t *data, size_t len);
	// All of it written and its CRC checked: the backend's own checks. reason: room for a
	// text. DFUKIT_PENDING while it is still at work (writing what it buffered): it is called
	// again on the client's next FINISH
	dfukit_status_t (*finish)(void *ctx, char *reason, size_t reason_size);
	dfukit_status_t (*activate)(void *ctx, uint8_t flags, uint16_t delay_ms);
	// An image backend's session broken off; a pass-through's session over, however it ended
	void (*abort)(void *ctx);
	// How many more stream bytes it can take now: the window
	uint32_t (*window)(void *ctx);

	// A pass-through backend's: one of the session's frames, to the downstream device as it is
	int (*forward)(void *ctx, const uint8_t *frame, size_t len);

	void *ctx;
	// The server's list, linked at dfukit_server_register()
	dfukit_backend_t *next;
};

typedef enum {
	DFUKIT_SESSION_IDLE,
	DFUKIT_SESSION_TRANSFER,
	DFUKIT_SESSION_SUSPENDED,
	DFUKIT_SESSION_READY,
	DFUKIT_SESSION_PASSTHROUGH,
} dfukit_session_state_t;

// What a status callback gets (a light, a display): the session's state, and its progress
typedef struct {
	dfukit_session_state_t state;
	// The stream's bytes acked, of all of it (passed through: 0, that is the downstream
	// device's)
	uint32_t done;
	uint32_t total;
	// After an image session (state IDLE): whether it ended activated
	bool activated;
} dfukit_server_status_t;

typedef struct {
	// A frame to a peer: 0 when sent (or queued), negative when not
	int (*send)(void *ctx, const dfukit_peer_t *to, const uint8_t *frame, size_t len);
	void *ctx;
	// The device's name, for INFO
	const char *name;
	// The largest frame the transport carries
	uint16_t max_frame;
	// For DFUKIT_ENCODING_DEFLATE; NULL: raw only
	const dfukit_inflater_t *inflater;
	// An ACK after this many stream bytes taken, or this long after the first not acked
	// (0: 8192 bytes, 50 ms)
	uint32_t ack_bytes;
	uint32_t ack_ms;
	// A session with no frame for idle_ms is suspended; a suspended one is dropped after
	// resume_ms, as is a finished one never activated (0: 10 s, 60 s)
	uint32_t idle_ms;
	uint32_t resume_ms;
	// Optional: the status, on every change of state and every ACK (a few times a second while
	// a stream comes in); from inside the server's calls, so quick
	void (*on_status)(void *ctx, const dfukit_server_status_t *status);
} dfukit_server_config_t;

// The server. Its fields are dfukit's: read them with the functions below
typedef struct {
	dfukit_server_config_t config;
	dfukit_backend_t *backends;
	dfukit_session_state_t state;
	dfukit_backend_t *backend;
	dfukit_peer_t owner;
	uint16_t session;
	uint16_t next_session;
	// The last session activated, to answer a resent ACTIVATE
	uint16_t activated;
	// The session of the last ACTIVATE passed through to a downstream device
	uint16_t forwarded_activate;
	// For on_status: the last one reported; whether the last session ended activated
	dfukit_server_status_t reported;
	bool ended_activated;
	dfukit_start_t start;
	// The stream: taken up to expected; the image: written up to written
	uint32_t expected;
	uint32_t written;
	uint32_t crc;
	// When the last ACK went, and up to where; when the last frame came; when suspended
	uint32_t acked;
	uint32_t ack_time;
	bool ack_due;
	// The window in the last ACK
	uint32_t window_sent;
	uint32_t last_frame;
	uint32_t suspended_at;
	// A GAP is sent once per expected offset, and again after ack_ms
	uint32_t gap_for;
	uint32_t gap_time;
	bool gap_sent;
} dfukit_server_t;

void dfukit_server_init(dfukit_server_t *server, const dfukit_server_config_t *config);

/**
 * @brief Adds a backend for its board ID. The application owns the backend, and it must outlive
 *        the server
 */
void dfukit_server_register(dfukit_server_t *server, dfukit_backend_t *backend);

/** @brief A frame from a peer */
void dfukit_server_feed(dfukit_server_t *server, const dfukit_peer_t *from, const uint8_t *frame,
			size_t len, uint32_t timestamp);

/**
 * @brief The timers: ACKs on their cadence, idle sessions suspended and dropped. Often enough for
 *        ack_ms (every 10 ms, say), and after each feed
 */
void dfukit_server_poll(dfukit_server_t *server, uint32_t timestamp);

/** @brief A pass-through backend's: a frame from its downstream device, to the session's owner */
void dfukit_server_downstream(dfukit_server_t *server, const uint8_t *frame, size_t len,
			      uint32_t timestamp);

/**
 * @brief A pass-through backend's: this frame from the downstream device says it activated its
 *        image
 */
bool dfukit_frame_activated(const uint8_t *frame, size_t len);

/**
 * @brief A pass-through backend's: the session is over (the downstream device says it is done).
 *        Else it ends with the owner's ABORT, or after idle_ms without a frame either way. Its
 *        abort is called
 */
void dfukit_server_passthrough_end(dfukit_server_t *server);

dfukit_session_state_t dfukit_server_state(const dfukit_server_t *server);

#ifdef __cplusplus
}
#endif

#endif // DFUKIT_SERVER_H
