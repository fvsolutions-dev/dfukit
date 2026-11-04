// dfukit 2's server (dfukit_server.h)

#include "dfukit_server.h"

#include <stddef.h>
#include <string.h>

#include "dfukit_crc32.h"
#include "dfukit_wire.h"

#define DEFAULT_ACK_BYTES 8192
#define DEFAULT_ACK_MS    50
#define DEFAULT_IDLE_MS   10000
#define DEFAULT_RESUME_MS 60000
// A board's name and version in INFO, at most
#define TLV_TEXT_MAX      48

static void answer_info(dfukit_server_t *server, const dfukit_peer_t *to);
static void answer_status(dfukit_server_t *server, const dfukit_peer_t *to);
static void on_start(dfukit_server_t *server, const dfukit_peer_t *from, const uint8_t *frame,
		     size_t len, uint32_t timestamp);
static void on_data(dfukit_server_t *server, const dfukit_peer_t *from, const uint8_t *frame,
		    size_t len, uint32_t timestamp);
static void on_finish(dfukit_server_t *server, const dfukit_peer_t *from, const uint8_t *frame,
		      size_t len, uint32_t timestamp);
static void on_activate(dfukit_server_t *server, const dfukit_peer_t *from, const uint8_t *frame,
			size_t len);
static void on_abort(dfukit_server_t *server, const dfukit_peer_t *from, const uint8_t *frame,
		     size_t len);
static dfukit_status_t take(dfukit_server_t *server, const uint8_t *data, size_t len);
static dfukit_status_t to_backend(void *context, const uint8_t *data, size_t len);
static void send_ack(dfukit_server_t *server, dfukit_status_t status, uint32_t timestamp);
static bool window_opened(dfukit_server_t *server);
static void send_short(dfukit_server_t *server, const dfukit_peer_t *to, uint8_t op,
		       dfukit_status_t status);
static void send_start_answer(dfukit_server_t *server, const dfukit_peer_t *to,
			      dfukit_status_t status, uint32_t resume);
static void end_session(dfukit_server_t *server, bool abort_backend);
static size_t text_length(const char *text, size_t max);
static void report(dfukit_server_t *server);
static dfukit_backend_t *find_backend(dfukit_server_t *server, uint32_t board_id);
static bool same_peer(const dfukit_peer_t *first, const dfukit_peer_t *second);
static bool owned_by_other(const dfukit_server_t *server, const dfukit_peer_t *from);

/** @brief Sets the server up from its configuration: what is left 0 gets its default. */
void dfukit_server_init(dfukit_server_t *server, const dfukit_server_config_t *config)
{
	memset(server, 0, sizeof(*server));
	server->config = *config;
	if (server->config.ack_bytes == 0) {
		server->config.ack_bytes = DEFAULT_ACK_BYTES;
	}
	if (server->config.ack_ms == 0) {
		server->config.ack_ms = DEFAULT_ACK_MS;
	}
	if (server->config.idle_ms == 0) {
		server->config.idle_ms = DEFAULT_IDLE_MS;
	}
	if (server->config.resume_ms == 0) {
		server->config.resume_ms = DEFAULT_RESUME_MS;
	}
	server->next_session = 1;
}

/** @brief Adds a backend: its board ID is now one this server takes. */
void dfukit_server_register(dfukit_server_t *server, dfukit_backend_t *backend)
{
	backend->next = server->backends;
	server->backends = backend;
}

/** @brief Where the session stands. */
dfukit_session_state_t dfukit_server_state(const dfukit_server_t *server)
{
	return server->state;
}

/**
 * @brief Sends a frame to what handles it: the server's own, a session passing through, or by
 *        opcode.
 */
static void feed(dfukit_server_t *server, const dfukit_peer_t *from, const uint8_t *frame,
		 size_t len, uint32_t timestamp);

/** @brief A frame from a peer: handled, and what changed reported. */
void dfukit_server_feed(dfukit_server_t *server, const dfukit_peer_t *from, const uint8_t *frame,
			size_t len, uint32_t timestamp)
{
	// A peer longer than its bytes: not one this server can tell apart, dropped
	if (from->len > DFUKIT_PEER_MAX) {
		return;
	}
	feed(server, from, frame, len, timestamp);
	report(server);
}

/**
 * @brief Sends a frame to what handles it: the server's own, a session passing through, or by
 *        opcode.
 */
static void feed(dfukit_server_t *server, const dfukit_peer_t *from, const uint8_t *frame,
		 size_t len, uint32_t timestamp)
{
	if (len == 0) {
		return;
	}
	// INFO and STATUS are the server's own, to anyone
	if (frame[0] == DFUKIT_OP_INFO) {
		answer_info(server, from);
		return;
	}
	if (frame[0] == DFUKIT_OP_STATUS) {
		answer_status(server, from);
		return;
	}
	// The owner moving on to another of this device's boards: the session passing through ends
	if (server->state == DFUKIT_SESSION_PASSTHROUGH && same_peer(from, &server->owner) &&
	    frame[0] == DFUKIT_OP_START && len >= sizeof(dfukit_start_frame_t)) {
		const dfukit_start_frame_t *start = (const dfukit_start_frame_t *)frame;
		dfukit_backend_t *target = find_backend(server, start->board_id);

		if (target != NULL && target != server->backend) {
			end_session(server, false);
		}
	}
	// A session that passes through: the owner's frames on as they are, what is in them is the
	// downstream device's; anyone else's are refused like any busy session's. It ends with the
	// owner's ABORT, on idle_ms, or by dfukit_server_passthrough_end(). Only ACTIVATE's session
	// is noted: the answer to a resend, once the session is over
	if (server->state == DFUKIT_SESSION_PASSTHROUGH && same_peer(from, &server->owner)) {
		server->last_frame = timestamp;
		if (frame[0] == DFUKIT_OP_ACTIVATE && len >= sizeof(dfukit_session_frame_t)) {
			server->forwarded_activate =
				((const dfukit_session_frame_t *)frame)->session;
		}
		server->backend->forward(server->backend->ctx, frame, len);
		if (frame[0] == DFUKIT_OP_ABORT) {
			end_session(server, false);
			send_short(server, from, DFUKIT_OP_ANSWER | DFUKIT_OP_ABORT, DFUKIT_OK);
		}
		return;
	}
	switch (frame[0]) {
	// Opens a session, or takes one over on a resume
	case DFUKIT_OP_START:
		on_start(server, from, frame, len, timestamp);
		break;
	// A piece of the stream
	case DFUKIT_OP_DATA:
		on_data(server, from, frame, len, timestamp);
		break;
	// The whole stream is sent: the checks
	case DFUKIT_OP_FINISH:
		on_finish(server, from, frame, len, timestamp);
		break;
	// Make the image the one that runs
	case DFUKIT_OP_ACTIVATE:
		on_activate(server, from, frame, len);
		break;
	// The host gives up
	case DFUKIT_OP_ABORT:
		on_abort(server, from, frame, len);
		break;
	default:
		break;
	}
}

/** @brief Time passes: ACKs on their cadence, idle sessions suspended and dropped. */
void dfukit_server_poll(dfukit_server_t *server, uint32_t timestamp)
{
	switch (server->state) {
	case DFUKIT_SESSION_TRANSFER:
		// ACKs on their cadence: what was taken since the last one. And as soon as a window
		// that was too small for the next frame has opened (the backend was busy)
		if ((server->ack_due && timestamp - server->ack_time >= server->config.ack_ms) ||
		    window_opened(server)) {
			send_ack(server, DFUKIT_OK, timestamp);
		}
		if (timestamp - server->last_frame >= server->config.idle_ms) {
			server->state = DFUKIT_SESSION_SUSPENDED;
			server->suspended_at = timestamp;
		}
		break;
	case DFUKIT_SESSION_SUSPENDED:
		if (timestamp - server->suspended_at >= server->config.resume_ms) {
			end_session(server, true);
		}
		break;
	case DFUKIT_SESSION_READY:
		// Finished, but never activated (the host went away): not held for ever
		if (timestamp - server->last_frame >= server->config.resume_ms) {
			end_session(server, true);
		}
		break;
	case DFUKIT_SESSION_PASSTHROUGH:
		if (timestamp - server->last_frame >= server->config.idle_ms) {
			end_session(server, false);
		}
		break;
	default:
		break;
	}
	report(server);
}

/** @brief A frame from the device behind a pass-through, on to the session's owner. */
void dfukit_server_downstream(dfukit_server_t *server, const uint8_t *frame, size_t len,
			      uint32_t timestamp)
{
	if (server->state != DFUKIT_SESSION_PASSTHROUGH || len == 0) {
		return;
	}
	server->last_frame = timestamp;
	server->config.send(server->config.ctx, &server->owner, frame, len);
	// The device behind has it: a resent ACTIVATE gets this answer once the session is over
	if (dfukit_frame_activated(frame, len)) {
		server->activated = server->forwarded_activate;
		server->session = server->forwarded_activate;
	}
}

/** @brief Whether this frame is the answer that an image was activated. */
bool dfukit_frame_activated(const uint8_t *frame, size_t len)
{
	const dfukit_answer_frame_t *answer = (const dfukit_answer_frame_t *)frame;

	return len >= sizeof(*answer) && answer->op == (DFUKIT_OP_ANSWER | DFUKIT_OP_ACTIVATE) &&
	       answer->status == DFUKIT_OK;
}

/** @brief Ends a session passing through. */
void dfukit_server_passthrough_end(dfukit_server_t *server)
{
	if (server->state == DFUKIT_SESSION_PASSTHROUGH) {
		end_session(server, false);
		report(server);
	}
}

/**
 * @brief START: the board ID selects the backend; a resume goes on where a suspended session
 *        stopped
 */
static void on_start(dfukit_server_t *server, const dfukit_peer_t *from, const uint8_t *frame,
		     size_t len, uint32_t timestamp)
{
	dfukit_start_t start;
	dfukit_backend_t *backend;

	const dfukit_start_frame_t *request = (const dfukit_start_frame_t *)frame;

	if (len < sizeof(*request)) {
		send_start_answer(server, from, DFUKIT_BAD_FRAME, 0);
		return;
	}
	start.board_id = request->board_id;
	start.encoding = (dfukit_encoding_t)request->encoding;
	start.flags = request->flags;
	start.image_size = request->image_size;
	start.stream_size = request->stream_size;
	start.crc32 = request->crc32;

	// Resume: the same image (its CRC and sizes name it), while its state is kept, from any
	// peer: the owner may come back as another one (a new connection, a new port), also while
	// the session still waits for it, or once the image is in (READY: FINISH and ACTIVATE
	// follow). The owner's START again for the session's image is its retry (the answer was
	// lost or late): answered the same way, so the session stays the one in flight
	bool same_image = server->start.board_id == start.board_id &&
			  server->start.encoding == start.encoding &&
			  server->start.image_size == start.image_size &&
			  server->start.stream_size == start.stream_size &&
			  server->start.crc32 == start.crc32;
	bool image_session = server->state == DFUKIT_SESSION_TRANSFER ||
			     server->state == DFUKIT_SESSION_SUSPENDED ||
			     server->state == DFUKIT_SESSION_READY;
	bool own_retry =
		server->state == DFUKIT_SESSION_TRANSFER && same_peer(from, &server->owner);

	if (same_image && image_session && (own_retry || (start.flags & DFUKIT_START_RESUME))) {
		server->owner = *from;
		if (server->state != DFUKIT_SESSION_READY) {
			server->state = DFUKIT_SESSION_TRANSFER;
		}
		server->last_frame = timestamp;
		server->gap_sent = false;
		send_start_answer(server, from, DFUKIT_OK, server->expected);
		send_ack(server, DFUKIT_OK, timestamp);
		return;
	}
	// A resume of the image already activated: it is all in, FINISH and ACTIVATE get their
	// answers again. Never a new session, whose begin would erase it
	if ((start.flags & DFUKIT_START_RESUME) && same_image &&
	    server->state == DFUKIT_SESSION_IDLE && server->activated != 0 &&
	    server->activated == server->session) {
		send_start_answer(server, from, DFUKIT_OK, server->start.stream_size);
		return;
	}
	if (start.flags & DFUKIT_START_RESUME) {
		if (!owned_by_other(server, from)) {
			send_start_answer(server, from, DFUKIT_NO_RESUME, 0);
			return;
		}
	}
	// Another peer's session: busy, unless it is suspended (a new START takes it over)
	if (owned_by_other(server, from) && server->state != DFUKIT_SESSION_SUSPENDED) {
		send_start_answer(server, from, DFUKIT_BUSY, 0);
		return;
	}
	backend = find_backend(server, start.board_id);
	if (backend == NULL) {
		send_start_answer(server, from, DFUKIT_UNKNOWN_BOARD, 0);
		return;
	}
	// An image backend's checks, before an old session goes for it: the encoding, the sizes
	if (backend->kind == DFUKIT_KIND_IMAGE) {
		dfukit_status_t refused = DFUKIT_OK;

		if (start.encoding != DFUKIT_ENCODING_RAW) {
			refused = DFUKIT_BAD_FRAME;
		} else if (start.image_size > backend->max_size) {
			refused = DFUKIT_TOO_LARGE;
		} else if (start.stream_size != start.image_size) {
			refused = DFUKIT_BAD_FRAME;
		}
		if (refused != DFUKIT_OK) {
			send_start_answer(server, from, refused, 0);
			return;
		}
	}
	// The owner starting over: the old session goes
	if (server->state != DFUKIT_SESSION_IDLE) {
		end_session(server, true);
	}
	// A new session: its number may be the one activated before
	server->activated = 0;
	if (backend->kind == DFUKIT_KIND_PASSTHROUGH) {
		server->backend = backend;
		server->owner = *from;
		server->state = DFUKIT_SESSION_PASSTHROUGH;
		// Unread, but it names the image: a resume once it is activated is not a new
		// session
		server->start = start;
		server->acked = 0;
		server->ended_activated = false;
		server->last_frame = timestamp;
		backend->forward(backend->ctx, frame, len);
		return;
	}
	dfukit_status_t status = backend->begin(backend->ctx, &start);

	if (status != DFUKIT_OK) {
		send_start_answer(server, from, status, 0);
		return;
	}
	server->backend = backend;
	server->owner = *from;
	server->start = start;
	server->session = server->next_session++;
	if (server->next_session == 0) {
		server->next_session = 1;
	}
	server->expected = 0;
	server->written = 0;
	server->acked = 0;
	server->ended_activated = false;
	server->crc = 0;
	server->state = DFUKIT_SESSION_TRANSFER;
	server->last_frame = timestamp;
	server->gap_sent = false;
	// Window 0 in the answer; the first window comes with the first ACK, now that the backend
	// is ready
	send_start_answer(server, from, DFUKIT_OK, 0);
	send_ack(server, DFUKIT_OK, timestamp);
}

/**
 * @brief DATA: taken only at exactly the expected offset. Past it a frame was lost: a GAP, at
 *        once. Before it a duplicate: acked again
 */
static void on_data(dfukit_server_t *server, const dfukit_peer_t *from, const uint8_t *frame,
		    size_t len, uint32_t timestamp)
{
	const dfukit_data_frame_t *request = (const dfukit_data_frame_t *)frame;

	if (len < sizeof(*request)) {
		return;
	}
	if (owned_by_other(server, from)) {
		dfukit_ack_frame_t ack = {
			.op = DFUKIT_OP_ACK,
			.session = request->session,
			.status = DFUKIT_BUSY,
		};

		server->config.send(server->config.ctx, from, (const uint8_t *)&ack, sizeof(ack));
		return;
	}
	if ((server->state != DFUKIT_SESSION_TRANSFER &&
	     server->state != DFUKIT_SESSION_SUSPENDED) ||
	    request->session != server->session) {
		dfukit_ack_frame_t ack = {
			.op = DFUKIT_OP_ACK,
			.session = request->session,
			.status = DFUKIT_NO_SESSION,
		};

		server->config.send(server->config.ctx, from, (const uint8_t *)&ack, sizeof(ack));
		return;
	}
	uint32_t offset = request->offset;
	const uint8_t *data = request->data;
	size_t data_len = len - sizeof(*request);

	server->state = DFUKIT_SESSION_TRANSFER;
	server->last_frame = timestamp;
	if (offset > server->expected) {
		if (!server->gap_sent || server->gap_for != server->expected ||
		    timestamp - server->gap_time >= server->config.ack_ms) {
			server->gap_sent = true;
			server->gap_for = server->expected;
			server->gap_time = timestamp;
			send_ack(server, DFUKIT_GAP, timestamp);
		}
		return;
	}
	if (offset < server->expected || data_len == 0) {
		// A duplicate, or a probe: where we are, now
		if (offset + data_len <= server->expected) {
			send_ack(server, DFUKIT_OK, timestamp);
			return;
		}
		// Overlapping: the part not taken yet
		data += server->expected - offset;
		data_len -= server->expected - offset;
	}
	if (data_len > server->start.stream_size - server->expected) {
		send_ack(server, DFUKIT_BAD_STREAM, timestamp);
		end_session(server, true);
		return;
	}
	dfukit_status_t status = take(server, data, data_len);

	if (status != DFUKIT_OK) {
		send_ack(server, status, timestamp);
		end_session(server, true);
		return;
	}
	server->expected += (uint32_t)data_len;
	server->gap_sent = false;
	if (!server->ack_due) {
		server->ack_due = true;
		server->ack_time = timestamp;
	}
	// An ACK every ack_bytes, or once half the window is used, so the client never runs dry
	uint32_t ack_at = server->window_sent / 2 < server->config.ack_bytes
				  ? server->window_sent / 2
				  : server->config.ack_bytes;

	if (server->expected - server->acked >= ack_at ||
	    server->expected == server->start.stream_size) {
		send_ack(server, DFUKIT_OK, timestamp);
	}
}

/** @brief The stream's bytes to the backend and the CRC */
static dfukit_status_t take(dfukit_server_t *server, const uint8_t *data, size_t len)
{
	return to_backend(server, data, len);
}

/** @brief The image's bytes: written by the backend, and counted in the CRC. */
static dfukit_status_t to_backend(void *context, const uint8_t *data, size_t len)
{
	dfukit_server_t *server = context;

	if (server->written + len > server->start.image_size) {
		return DFUKIT_BAD_STREAM;
	}
	dfukit_status_t status =
		server->backend->write(server->backend->ctx, server->written, data, len);

	if (status != DFUKIT_OK) {
		return status;
	}
	server->crc = dfukit_crc32(server->crc, data, len);
	server->written += (uint32_t)len;
	return DFUKIT_OK;
}

/** @brief FINISH: all of the stream, all of the image, the CRC, then the backend's own checks */
static void on_finish(dfukit_server_t *server, const dfukit_peer_t *from, const uint8_t *frame_in,
		      size_t len, uint32_t timestamp)
{
	const dfukit_session_frame_t *request = (const dfukit_session_frame_t *)frame_in;
	dfukit_finish_answer_t answer = {.op = DFUKIT_OP_ANSWER | DFUKIT_OP_FINISH};
	dfukit_status_t status = DFUKIT_OK;

	if (len < sizeof(*request)) {
		send_short(server, from, DFUKIT_OP_ANSWER | DFUKIT_OP_FINISH, DFUKIT_BAD_FRAME);
		return;
	}
	if (owned_by_other(server, from)) {
		send_short(server, from, DFUKIT_OP_ANSWER | DFUKIT_OP_FINISH, DFUKIT_BUSY);
		return;
	}
	// A FINISH again for the image already activated: the same answer
	if (server->activated != 0 && request->session == server->activated) {
		send_short(server, from, DFUKIT_OP_ANSWER | DFUKIT_OP_FINISH, DFUKIT_OK);
		return;
	}
	if (server->state == DFUKIT_SESSION_IDLE || request->session != server->session) {
		send_short(server, from, DFUKIT_OP_ANSWER | DFUKIT_OP_FINISH, DFUKIT_NO_SESSION);
		return;
	}
	// A FINISH again, its answer lost: the same answer
	if (server->state == DFUKIT_SESSION_READY) {
		send_short(server, from, DFUKIT_OP_ANSWER | DFUKIT_OP_FINISH, DFUKIT_OK);
		return;
	}
	if (server->state != DFUKIT_SESSION_TRANSFER && server->state != DFUKIT_SESSION_SUSPENDED) {
		send_short(server, from, DFUKIT_OP_ANSWER | DFUKIT_OP_FINISH, DFUKIT_NO_SESSION);
		return;
	}
	if (server->expected != server->start.stream_size ||
	    server->written != server->start.image_size) {
		status = DFUKIT_BAD_STREAM;
	} else {
		if (server->crc != server->start.crc32) {
			status = DFUKIT_CRC_MISMATCH;
		} else {
			status = server->backend->finish(server->backend->ctx, answer.reason,
							 sizeof(answer.reason));
		}
	}
	answer.status = status;
	answer.reason_len = (uint8_t)text_length(answer.reason, sizeof(answer.reason));
	server->config.send(server->config.ctx, from, (const uint8_t *)&answer,
			    offsetof(dfukit_finish_answer_t, reason) + answer.reason_len);
	if (status == DFUKIT_OK) {
		server->state = DFUKIT_SESSION_READY;
		server->last_frame = timestamp;
	} else if (status == DFUKIT_PENDING) {
		// Still writing: the client asks again, and the session is alive
		server->state = DFUKIT_SESSION_TRANSFER;
		server->last_frame = timestamp;
	} else {
		end_session(server, true);
	}
}

/** @brief ACTIVATE: the backend switches to the image; the session is over */
static void on_activate(dfukit_server_t *server, const dfukit_peer_t *from, const uint8_t *frame,
			size_t len)
{
	const dfukit_activate_frame_t *request = (const dfukit_activate_frame_t *)frame;

	if (len < sizeof(*request)) {
		send_short(server, from, DFUKIT_OP_ANSWER | DFUKIT_OP_ACTIVATE, DFUKIT_BAD_FRAME);
		return;
	}
	uint16_t session = request->session;

	// An ACTIVATE again, its answer lost: the session is over, the answer the same
	if (server->activated != 0 && session == server->activated) {
		send_short(server, from, DFUKIT_OP_ANSWER | DFUKIT_OP_ACTIVATE, DFUKIT_OK);
		return;
	}
	if (owned_by_other(server, from)) {
		send_short(server, from, DFUKIT_OP_ANSWER | DFUKIT_OP_ACTIVATE, DFUKIT_BUSY);
		return;
	}
	if (server->state != DFUKIT_SESSION_READY || session != server->session) {
		send_short(server, from, DFUKIT_OP_ANSWER | DFUKIT_OP_ACTIVATE, DFUKIT_NO_SESSION);
		return;
	}
	// The backend's answer: activate only schedules a restart (after delay), so it is sent
	dfukit_status_t status =
		server->backend->activate(server->backend->ctx, request->flags, request->delay_ms);

	send_short(server, from, DFUKIT_OP_ANSWER | DFUKIT_OP_ACTIVATE, status);
	if (status == DFUKIT_OK) {
		server->activated = server->session;
		server->ended_activated = true;
		end_session(server, false);
	}
}

/** @brief ABORT: the session ends, unless it is another peer's or another session's. */
static void on_abort(dfukit_server_t *server, const dfukit_peer_t *from, const uint8_t *frame,
		     size_t len)
{
	const dfukit_session_frame_t *request = (const dfukit_session_frame_t *)frame;

	if (len < sizeof(*request)) {
		send_short(server, from, DFUKIT_OP_ANSWER | DFUKIT_OP_ABORT, DFUKIT_BAD_FRAME);
		return;
	}
	if (owned_by_other(server, from)) {
		send_short(server, from, DFUKIT_OP_ANSWER | DFUKIT_OP_ABORT, DFUKIT_BUSY);
		return;
	}
	// Another session's ABORT (late, from an earlier run): not this one's
	if (server->state != DFUKIT_SESSION_IDLE && request->session != server->session) {
		send_short(server, from, DFUKIT_OP_ANSWER | DFUKIT_OP_ABORT, DFUKIT_NO_SESSION);
		return;
	}
	end_session(server, server->state != DFUKIT_SESSION_IDLE);
	send_short(server, from, DFUKIT_OP_ANSWER | DFUKIT_OP_ABORT, DFUKIT_OK);
}

/** @brief INFO's answer: the device's name and the boards it takes, as many as fit a frame. */
static void answer_info(dfukit_server_t *server, const dfukit_peer_t *to)
{
	uint8_t frame[256];
	size_t name_len = server->config.name != NULL ? strlen(server->config.name) : 0;
	size_t len = 0;
	uint8_t count = 0;

	// Op, version, the name's length and the board count come first; the name is cut to what
	// fits
	size_t name_room = server->config.max_frame > 4 ? server->config.max_frame - 4u : 0;

	if (name_len > 64) {
		name_len = 64;
	}
	if (name_len > name_room) {
		name_len = name_room;
	}
	frame[len++] = DFUKIT_OP_ANSWER | DFUKIT_OP_INFO;
	frame[len++] = DFUKIT_PROTOCOL_VERSION;
	frame[len++] = (uint8_t)name_len;
	if (name_len > 0) {
		memcpy(frame + len, server->config.name, name_len);
	}
	len += name_len;
	size_t count_at = len++;

	for (dfukit_backend_t *backend = server->backends; backend != NULL;
	     backend = backend->next) {
		// Its name and its image's version, each cut to TLV_TEXT_MAX
		size_t name = backend->name != NULL ? text_length(backend->name, TLV_TEXT_MAX) : 0;
		size_t version =
			backend->version != NULL ? text_length(backend->version, TLV_TEXT_MAX) : 0;
		size_t tlv = (name > 0 ? 2 + name : 0) + (version > 0 ? 2 + version : 0);

		if (len + sizeof(dfukit_info_board_t) + tlv > sizeof(frame) ||
		    len + sizeof(dfukit_info_board_t) + tlv > server->config.max_frame) {
			break;
		}
		dfukit_info_board_t *board = (dfukit_info_board_t *)(frame + len);

		board->board_id = backend->board_id;
		board->kind = backend->kind;
		board->max_size = backend->max_size;
		// What the device takes (a pass-through announces the downstream device's); no
		// encoding with a window
		board->encodings = backend->encodings;
		board->window_log2 = 0;
		board->tlv_len = (uint16_t)tlv;
		len += sizeof(*board);
		if (name > 0) {
			frame[len++] = DFUKIT_TLV_NAME;
			frame[len++] = (uint8_t)name;
			memcpy(frame + len, backend->name, name);
			len += name;
		}
		if (version > 0) {
			frame[len++] = DFUKIT_TLV_VERSION;
			frame[len++] = (uint8_t)version;
			memcpy(frame + len, backend->version, version);
			len += version;
		}
		count++;
	}
	frame[count_at] = count;
	server->config.send(server->config.ctx, to, frame, len);
}

/** @brief STATUS's answer: where the session stands. */
static void answer_status(dfukit_server_t *server, const dfukit_peer_t *to)
{
	// Passed through: where the stream stands is the downstream device's
	bool passing = server->state == DFUKIT_SESSION_PASSTHROUGH;
	dfukit_status_answer_t frame = {
		.op = DFUKIT_OP_ANSWER | DFUKIT_OP_STATUS,
		.state = (uint8_t)server->state,
		.session = passing ? 0 : server->session,
		.offset = passing ? 0 : server->expected,
		.board_id = passing ? server->backend->board_id : server->start.board_id,
	};

	server->config.send(server->config.ctx, to, (const uint8_t *)&frame, sizeof(frame));
}

/** @brief An ACK to the owner: where the stream stands, and how much more it may send */
static void send_ack(dfukit_server_t *server, dfukit_status_t status, uint32_t timestamp)
{
	uint32_t window = status == DFUKIT_OK || status == DFUKIT_GAP
				  ? server->backend->window(server->backend->ctx)
				  : 0;
	dfukit_ack_frame_t frame = {
		.op = DFUKIT_OP_ACK,
		.session = server->session,
		.status = status,
		.offset = server->expected,
		.window = window,
	};

	server->config.send(server->config.ctx, &server->owner, (const uint8_t *)&frame,
			    sizeof(frame));
	server->window_sent = window;
	server->acked = server->expected;
	server->ack_time = timestamp;
	server->ack_due = false;
}

/** @brief The last ACK's window had no room for the next frame, and now there is */
static bool window_opened(dfukit_server_t *server)
{
	uint32_t left = server->start.stream_size - server->expected;
	uint32_t max_data =
		server->config.max_frame > sizeof(dfukit_data_frame_t)
			? (uint32_t)(server->config.max_frame - sizeof(dfukit_data_frame_t))
			: 1;
	uint32_t need = left < max_data ? left : max_data;

	return left > 0 && server->window_sent < need &&
	       server->backend->window(server->backend->ctx) >= need;
}

/** @brief An answer with only a status. */
static void send_short(dfukit_server_t *server, const dfukit_peer_t *to, uint8_t op,
		       dfukit_status_t status)
{
	dfukit_finish_answer_t frame = {.op = op, .status = status};

	// Without a reason: FINISH's answer still says it has none
	server->config.send(server->config.ctx, to, (const uint8_t *)&frame,
			    op == (DFUKIT_OP_ANSWER | DFUKIT_OP_FINISH)
				    ? offsetof(dfukit_finish_answer_t, reason)
				    : sizeof(dfukit_answer_frame_t));
}

/** @brief START's answer: the session, where to start from, and how large a frame's data may be. */
static void send_start_answer(dfukit_server_t *server, const dfukit_peer_t *to,
			      dfukit_status_t status, uint32_t resume)
{
	dfukit_start_answer_t frame = {
		.op = DFUKIT_OP_ANSWER | DFUKIT_OP_START,
		.status = status,
		.session = status == DFUKIT_OK ? server->session : 0,
		.resume = resume,
		.window = 0,
		.max_data =
			server->config.max_frame > sizeof(dfukit_data_frame_t)
				? (uint16_t)(server->config.max_frame - sizeof(dfukit_data_frame_t))
				: 0,
	};

	server->config.send(server->config.ctx, to, (const uint8_t *)&frame, sizeof(frame));
}

/**
 * @brief An image backend's abort when abort_backend; a pass-through's at the end of its session,
 *        however it ends (its link to the downstream device can go)
 */
static void end_session(dfukit_server_t *server, bool abort_backend)
{
	dfukit_backend_t *backend = server->backend;
	bool was_open = server->state != DFUKIT_SESSION_IDLE;

	// Over before the backend hears of it: its abort may end the session too
	server->state = DFUKIT_SESSION_IDLE;
	server->backend = NULL;
	server->owner.len = 0;
	server->ack_due = false;
	if (backend != NULL && was_open && backend->abort != NULL &&
	    (abort_backend || backend->kind == DFUKIT_KIND_PASSTHROUGH)) {
		backend->abort(backend->ctx);
	}
}

/** @brief strnlen, which is not C99 */
static size_t text_length(const char *text, size_t max)
{
	size_t length = 0;

	while (length < max && text[length] != '\0') {
		length++;
	}
	return length;
}

/** @brief To on_status, when it changed */
static void report(dfukit_server_t *server)
{
	// The current session's, or after it the last one's
	dfukit_server_status_t current = {
		.state = server->state,
		.done = server->acked,
		.total = server->start.stream_size,
		.activated = server->ended_activated,
	};

	if (server->config.on_status == NULL ||
	    (current.state == server->reported.state && current.done == server->reported.done &&
	     current.total == server->reported.total &&
	     current.activated == server->reported.activated)) {
		return;
	}
	server->reported = current;
	server->config.on_status(server->config.ctx, &current);
}

/** @brief The backend for a board ID, or NULL. */
static dfukit_backend_t *find_backend(dfukit_server_t *server, uint32_t board_id)
{
	for (dfukit_backend_t *backend = server->backends; backend != NULL;
	     backend = backend->next) {
		if (backend->board_id == board_id) {
			return backend;
		}
	}
	return NULL;
}

/** @brief Whether two peers are the same address. */
static bool same_peer(const dfukit_peer_t *first, const dfukit_peer_t *second)
{
	return first->len == second->len && memcmp(first->bytes, second->bytes, first->len) == 0;
}

/**
 * @brief A session in progress that another peer owns. A suspended one is free to take over by
 *        its resume (on_start), not otherwise
 */
static bool owned_by_other(const dfukit_server_t *server, const dfukit_peer_t *from)
{
	return server->state != DFUKIT_SESSION_IDLE && !same_peer(from, &server->owner);
}

/** @brief A status's name, for logs and messages. */
const char *dfukit_status_name(dfukit_status_t status)
{
	static const char *const names[] = {
		"OK",         "GAP",          "BUSY",    "UNKNOWN_BOARD", "TOO_LARGE", "NO_RESUME",
		"BAD_STREAM", "CRC_MISMATCH", "BACKEND", "NO_SESSION",    "BAD_FRAME", "PENDING",
	};

	if (status == DFUKIT_TIMEOUT) {
		return "TIMEOUT";
	}
	return (unsigned)status < sizeof(names) / sizeof(names[0]) ? names[status] : "?";
}
