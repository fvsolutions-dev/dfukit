// dfukit 2's client (dfukit_client.h)

#include "dfukit_client.h"

#include <string.h>

#include "dfukit_wire.h"

#define DEFAULT_RTO_MS   500
#define DEFAULT_TRIES    8
#define DEFAULT_STALL_MS (5 * 60 * 1000)

static void send_start(dfukit_client_t *client, uint32_t timestamp);
static void send_session_op(dfukit_client_t *client, uint8_t op, uint32_t timestamp);
static void send_data(dfukit_client_t *client);
static void on_start_answer(dfukit_client_t *client, const uint8_t *frame, size_t len,
			    uint32_t timestamp);
static void on_ack(dfukit_client_t *client, const uint8_t *frame, size_t len, uint32_t timestamp);
static void finish_if_done(dfukit_client_t *client, uint32_t timestamp);
static void fail(dfukit_client_t *client, dfukit_status_t status);

/** @brief Sets the client up from its configuration: what is left 0 gets its default */
void dfukit_client_init(dfukit_client_t *client, const dfukit_client_config_t *config)
{
	memset(client, 0, sizeof(*client));
	client->config = *config;
	if (client->config.rto_ms == 0) {
		client->config.rto_ms = DEFAULT_RTO_MS;
	}
	if (client->config.tries == 0) {
		client->config.tries = DEFAULT_TRIES;
	}
	if (client->config.stall_ms == 0) {
		client->config.stall_ms = DEFAULT_STALL_MS;
	}
	// The DATA frames are built in a buffer of DFUKIT_MAX_FRAME
	if (client->config.max_frame == 0 || client->config.max_frame > DFUKIT_MAX_FRAME) {
		client->config.max_frame = DFUKIT_MAX_FRAME;
	}
}
/**
 * @brief Starts an update: forgets the last one, and sends START. The rest follows from the
 *        answers (dfukit_client_feed()) and from time (dfukit_client_poll())
 */
void dfukit_client_begin(dfukit_client_t *client, const dfukit_start_t *start, bool activate,
			 uint8_t activate_flags, uint16_t activate_delay_ms, uint32_t timestamp)
{
	client->start = *start;
	client->activate = activate;
	client->activate_flags = activate_flags;
	client->activate_delay = activate_delay_ms;
	client->status = DFUKIT_OK;
	client->acked = 0;
	client->next = 0;
	client->window = 0;
	client->resent = 0;
	client->tries_left = client->config.tries;
	client->state = DFUKIT_CLIENT_STARTING;
	client->moved_time = timestamp;
	// No GAP seen yet, also not at offset 0
	client->rewound_to = UINT32_MAX;
	send_start(client, timestamp);
}

/**
 * @brief Goes on after a lost link, on the same or a new one: START again, with the resume flag.
 *        The device answers where it stands, or that it has nothing to resume from. Ignored
 *        unless an update is running, or failed by a timeout
 */
void dfukit_client_resume(dfukit_client_t *client, uint32_t timestamp)
{
	if (client->state != DFUKIT_CLIENT_SENDING && client->state != DFUKIT_CLIENT_STARTING &&
	    client->state != DFUKIT_CLIENT_FINISHING && client->state != DFUKIT_CLIENT_ACTIVATING &&
	    !(client->state == DFUKIT_CLIENT_FAILED && client->status == DFUKIT_TIMEOUT)) {
		return;
	}
	client->status = DFUKIT_OK;
	client->moved_time = timestamp;
	client->start.flags |= DFUKIT_START_RESUME;
	client->tries_left = client->config.tries;
	client->state = DFUKIT_CLIENT_STARTING;
	send_start(client, timestamp);
}

/** @brief Where the update stands */
dfukit_client_state_t dfukit_client_state(const dfukit_client_t *client)
{
	return client->state;
}

/** @brief Why it failed */
dfukit_status_t dfukit_client_status(const dfukit_client_t *client)
{
	return client->status;
}

/** @brief How much of the stream the device has taken */
uint32_t dfukit_client_acked(const dfukit_client_t *client)
{
	return client->acked;
}

/**
 * @brief A frame from the device: moves the update on by the answer it carries. Frames too short
 *        to hold an opcode and a status, and answers to something the client is not waiting for
 *        (late or duplicated), are ignored
 */
void dfukit_client_feed(dfukit_client_t *client, const uint8_t *frame, size_t len,
			uint32_t timestamp)
{
	const dfukit_answer_frame_t *answer = (const dfukit_answer_frame_t *)frame;

	if (len < sizeof(*answer)) {
		return;
	}
	switch (answer->op) {
	// The session is open, or refused: on to the data
	case DFUKIT_OP_ANSWER | DFUKIT_OP_START:
		on_start_answer(client, frame, len, timestamp);
		break;
	// How far the device is, and how much more it can take: on with the data, or back after a
	// gap
	case DFUKIT_OP_ACK:
		on_ack(client, frame, len, timestamp);
		break;
	// The whole stream was checked: on to ACTIVATE, or done when the host does not activate
	case DFUKIT_OP_ANSWER | DFUKIT_OP_FINISH:
		if (client->state != DFUKIT_CLIENT_FINISHING) {
			break;
		}
		if (answer->status == DFUKIT_PENDING) {
			// The device is alive and at work: FINISH again after the RTO
			client->tries_left = client->config.tries;
			client->request_time = timestamp;
		} else if (answer->status != DFUKIT_OK) {
			fail(client, (dfukit_status_t)answer->status);
		} else if (client->activate) {
			client->state = DFUKIT_CLIENT_ACTIVATING;
			client->moved_time = timestamp;
			client->tries_left = client->config.tries;
			send_session_op(client, DFUKIT_OP_ACTIVATE, timestamp);
		} else {
			client->state = DFUKIT_CLIENT_DONE;
		}
		break;
	// The image will run: the update is over
	case DFUKIT_OP_ANSWER | DFUKIT_OP_ACTIVATE:
		if (client->state != DFUKIT_CLIENT_ACTIVATING) {
			break;
		}
		if (answer->status != DFUKIT_OK) {
			fail(client, (dfukit_status_t)answer->status);
		} else {
			client->state = DFUKIT_CLIENT_DONE;
		}
		break;
	// Nothing the client is waiting for
	default:
		break;
	}
}

/**
 * @brief Time passes: resends what went unanswered, sends what the window allows, and gives up
 *        when nothing has moved for too long. Often enough for the RTO, every few milliseconds
 */
void dfukit_client_poll(dfukit_client_t *client, uint32_t timestamp)
{
	// Stuck, though the device may answer: given up
	if (client->state > DFUKIT_CLIENT_IDLE && client->state < DFUKIT_CLIENT_DONE &&
	    timestamp - client->moved_time >= client->config.stall_ms) {
		fail(client, DFUKIT_TIMEOUT);
		return;
	}
	switch (client->state) {
	// Waiting for the answer to a single request
	case DFUKIT_CLIENT_STARTING:
	case DFUKIT_CLIENT_FINISHING:
	case DFUKIT_CLIENT_ACTIVATING:
		// A request without an answer: again, until the tries are used up
		if (timestamp - client->request_time < client->config.rto_ms) {
			break;
		}
		if (client->tries_left == 0) {
			fail(client, DFUKIT_TIMEOUT);
			break;
		}
		if (client->state == DFUKIT_CLIENT_STARTING) {
			send_start(client, timestamp);
		} else {
			send_session_op(client,
					client->state == DFUKIT_CLIENT_FINISHING
						? DFUKIT_OP_FINISH
						: DFUKIT_OP_ACTIVATE,
					timestamp);
		}
		break;
	// In the stream: frames go out as the window allows
	case DFUKIT_CLIENT_SENDING:
		// No progress for the RTO: what is in flight is lost, or its ACKs are; from the
		// last acked offset again. With nothing in flight, a probe asks where the device is
		if (timestamp - client->progress_time >= client->config.rto_ms) {
			if (client->tries_left == 0) {
				fail(client, DFUKIT_TIMEOUT);
				break;
			}
			client->tries_left--;
			client->progress_time = timestamp;
			if (client->next > client->acked) {
				client->resent += client->next - client->acked;
				client->next = client->acked;
			} else {
				dfukit_data_frame_t probe = {
					.op = DFUKIT_OP_DATA,
					.session = client->session,
					.offset = client->acked,
				};

				client->config.send(client->config.ctx, (const uint8_t *)&probe,
						    sizeof(probe));
			}
		}
		send_data(client);
		break;
	// Not started, or over: nothing to do
	default:
		break;
	}
}

/** @brief Sends the stream from next on, frame by frame, as far as the window allows */
static void send_data(dfukit_client_t *client)
{
	uint8_t buffer[DFUKIT_MAX_FRAME];
	dfukit_data_frame_t *frame = (dfukit_data_frame_t *)buffer;
	uint16_t sent = 0;
	// Allowed up to acked + window, saturated: a window near 2^32 means no limit, not a wrap
	uint32_t limit = client->window > UINT32_MAX - client->acked
				 ? UINT32_MAX
				 : client->acked + client->window;

	while (client->next < client->start.stream_size && client->next < limit &&
	       (client->config.burst == 0 || sent < client->config.burst)) {
		uint32_t length = client->max_data;

		if (length > client->start.stream_size - client->next) {
			length = client->start.stream_size - client->next;
		}
		if (length > limit - client->next) {
			length = limit - client->next;
		}
		frame->op = DFUKIT_OP_DATA;
		frame->session = client->session;
		frame->offset = client->next;
		if (client->config.read(client->config.ctx, client->next, frame->data, length) <
		    0) {
			fail(client, DFUKIT_BAD_STREAM);
			return;
		}
		if (client->config.send(client->config.ctx, buffer, sizeof(*frame) + length) < 0) {
			// The transport is full: again on the next poll
			return;
		}
		client->next += length;
		sent++;
	}
}

/**
 * @brief The answer to START: takes the session, how large a frame may be and where to start
 *        from. A resume the device has no state for starts over; any other refusal ends the
 *        update
 */
static void on_start_answer(dfukit_client_t *client, const uint8_t *frame, size_t len,
			    uint32_t timestamp)
{
	const dfukit_start_answer_t *answer = (const dfukit_start_answer_t *)frame;

	if (client->state != DFUKIT_CLIENT_STARTING || len < sizeof(*answer)) {
		return;
	}
	dfukit_status_t status = (dfukit_status_t)answer->status;

	// Nothing to resume from: from the start
	if (status == DFUKIT_NO_RESUME && (client->start.flags & DFUKIT_START_RESUME)) {
		client->start.flags &= (uint8_t)~DFUKIT_START_RESUME;
		client->tries_left = client->config.tries;
		send_start(client, timestamp);
		return;
	}
	if (status != DFUKIT_OK) {
		fail(client, status);
		return;
	}
	size_t own = client->config.max_frame > sizeof(dfukit_data_frame_t)
			     ? client->config.max_frame - sizeof(dfukit_data_frame_t)
			     : 0;

	client->session = answer->session;
	client->max_data = answer->max_data < own ? answer->max_data : (uint16_t)own;
	if (client->max_data == 0) {
		fail(client, DFUKIT_BAD_FRAME);
		return;
	}
	client->acked = answer->resume;
	client->next = client->acked;
	client->window = answer->window;
	client->progress_time = timestamp;
	client->moved_time = timestamp;
	client->tries_left = client->config.tries;
	client->state = DFUKIT_CLIENT_SENDING;
	finish_if_done(client, timestamp);
}

/**
 * @brief An ACK: moves acked and the window on, and goes back to the offset after a gap. Only the
 *        first GAP for an offset rewinds: the frames still in flight bring more of them
 */
static void on_ack(dfukit_client_t *client, const uint8_t *frame, size_t len, uint32_t timestamp)
{
	const dfukit_ack_frame_t *ack = (const dfukit_ack_frame_t *)frame;

	if (client->state != DFUKIT_CLIENT_SENDING || len < sizeof(*ack) ||
	    ack->session != client->session) {
		return;
	}
	dfukit_status_t status = (dfukit_status_t)ack->status;
	uint32_t offset = ack->offset;

	if (status != DFUKIT_OK && status != DFUKIT_GAP) {
		fail(client, status);
		return;
	}
	if (offset < client->acked) {
		// Older than what is known: an ACK overtaken by a later one
		return;
	}
	// The device answers: alive, though it may be busy (window 0, an erase say) and take
	// nothing
	client->tries_left = client->config.tries;
	if (offset > client->acked) {
		client->acked = offset;
		client->progress_time = timestamp;
		client->moved_time = timestamp;
	}
	client->window = ack->window;
	if (status == DFUKIT_GAP && !(offset == client->rewound_to &&
				      timestamp - client->rewound_time < client->config.rto_ms)) {
		// A frame was lost: everything after it again
		if (client->next > offset) {
			client->resent += client->next - offset;
		}
		client->next = offset;
		client->rewound_to = offset;
		client->rewound_time = timestamp;
		client->progress_time = timestamp;
	}
	if (client->next < client->acked) {
		client->next = client->acked;
	}
	finish_if_done(client, timestamp);
}

/** @brief The device has the whole stream: on to FINISH */
static void finish_if_done(dfukit_client_t *client, uint32_t timestamp)
{
	if (client->state == DFUKIT_CLIENT_SENDING && client->acked == client->start.stream_size) {
		client->state = DFUKIT_CLIENT_FINISHING;
		client->moved_time = timestamp;
		client->tries_left = client->config.tries;
		send_session_op(client, DFUKIT_OP_FINISH, timestamp);
	}
}

/** @brief Sends START, with what identifies the image; counts as a try */
static void send_start(dfukit_client_t *client, uint32_t timestamp)
{
	dfukit_start_frame_t frame = {
		.op = DFUKIT_OP_START,
		.board_id = client->start.board_id,
		.encoding = (uint8_t)client->start.encoding,
		.flags = client->start.flags,
		.image_size = client->start.image_size,
		.stream_size = client->start.stream_size,
		.crc32 = client->start.crc32,
	};

	client->config.send(client->config.ctx, (const uint8_t *)&frame, sizeof(frame));
	client->request_time = timestamp;
	client->tries_left--;
}

/** @brief Sends FINISH or ACTIVATE for the session; counts as a try */
static void send_session_op(dfukit_client_t *client, uint8_t op, uint32_t timestamp)
{
	dfukit_activate_frame_t frame = {
		.op = op,
		.session = client->session,
		.flags = client->activate_flags,
		.delay_ms = client->activate_delay,
	};

	// FINISH is the same frame without the last two fields
	client->config.send(client->config.ctx, (const uint8_t *)&frame,
			    op == DFUKIT_OP_ACTIVATE ? sizeof(frame)
						     : sizeof(dfukit_session_frame_t));
	client->request_time = timestamp;
	client->tries_left--;
}

/** @brief Ends the update with a status */
static void fail(dfukit_client_t *client, dfukit_status_t status)
{
	client->state = DFUKIT_CLIENT_FAILED;
	client->status = status;
	// Over: the device lets go at once, without its idle time. Not when the link may be back
	// for a resume, and a session that is someone else's stays theirs
	if (status != DFUKIT_TIMEOUT && status != DFUKIT_BUSY) {
		dfukit_session_frame_t frame = {.op = DFUKIT_OP_ABORT, .session = client->session};

		client->config.send(client->config.ctx, (const uint8_t *)&frame, sizeof(frame));
	}
}
