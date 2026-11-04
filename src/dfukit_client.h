// dfukit 2's client: the host's end (DESIGN.md). Driven like the server: frames in with
// dfukit_client_feed(), frames out through the send callback, time in with dfukit_client_poll().
// It reads the stream through a callback, at any offset (a rewind reads again), so the image need
// not be in RAM. No threads, no heap.

#ifndef DFUKIT_CLIENT_H
#define DFUKIT_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dfukit.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	// A frame to the device: 0 when sent (or queued), negative when not
	int (*send)(void *ctx, const uint8_t *frame, size_t len);
	// len bytes of the stream at offset into buf: 0, or negative on an error
	int (*read)(void *ctx, uint32_t offset, uint8_t *buf, size_t len);
	void *ctx;
	// The largest frame the transport carries (the device may ask for less)
	uint16_t max_frame;
	// The resend timeout, at least (0: 500 ms); START, FINISH and ACTIVATE are sent this many
	// times before the client gives up (0: 8)
	uint32_t rto_ms;
	uint8_t tries;
	// Frames sent per poll at most (0: no limit): for a transport that takes few at a time
	uint16_t burst;
	// No progress (the stream acked further, or the next state) for this long: TIMEOUT, even
	// while the device answers (a window of 0, PENDING) (0: 5 minutes)
	uint32_t stall_ms;
} dfukit_client_config_t;

typedef enum {
	DFUKIT_CLIENT_IDLE,
	DFUKIT_CLIENT_STARTING,
	DFUKIT_CLIENT_SENDING,
	DFUKIT_CLIENT_FINISHING,
	DFUKIT_CLIENT_ACTIVATING,
	DFUKIT_CLIENT_DONE,
	DFUKIT_CLIENT_FAILED,
} dfukit_client_state_t;

// The client. Its fields are dfukit's: read them with the functions below
typedef struct {
	dfukit_client_config_t config;
	dfukit_client_state_t state;
	dfukit_status_t status;
	dfukit_start_t start;
	bool activate;
	uint8_t activate_flags;
	uint16_t activate_delay;
	uint16_t session;
	uint16_t max_data;
	// The stream: acked up to acked, sent up to next, allowed up to acked + window
	uint32_t acked;
	uint32_t next;
	uint32_t window;
	// The last progress (an ACK that moved acked), the last frame sent that wants an answer
	uint32_t progress_time;
	uint32_t request_time;
	// The last progress of any kind, for stall_ms
	uint32_t moved_time;
	uint8_t tries_left;
	// A GAP for this offset was handled at gap_time: the same one again within the RTO is one
	// of the frames already in flight
	uint32_t rewound_to;
	uint32_t rewound_time;
	// Bytes sent again, for statistics
	uint32_t resent;
} dfukit_client_t;

void dfukit_client_init(dfukit_client_t *client, const dfukit_client_config_t *config);

/**
 * @brief An update: start->flags may ask for a resume. With activate, ACTIVATE follows a good
 *        FINISH
 */
void dfukit_client_begin(dfukit_client_t *client, const dfukit_start_t *start, bool activate,
			 uint8_t activate_flags, uint16_t activate_delay_ms, uint32_t timestamp);

/** @brief A frame from the device */
void dfukit_client_feed(dfukit_client_t *client, const uint8_t *frame, size_t len,
			uint32_t timestamp);

/**
 * @brief Sends what the window allows, and resends after the RTO. Often (every few ms), and after
 *        each feed
 */
void dfukit_client_poll(dfukit_client_t *client, uint32_t timestamp);

dfukit_client_state_t dfukit_client_state(const dfukit_client_t *client);

/** @brief Why it failed (DFUKIT_CLIENT_FAILED); DFUKIT_OK otherwise */
dfukit_status_t dfukit_client_status(const dfukit_client_t *client);

/** @brief The stream's bytes the device has taken */
uint32_t dfukit_client_acked(const dfukit_client_t *client);

/**
 * @brief The link was lost (or the client timed out on it): on the next one,
 *        dfukit_client_resume() asks the device to go on where it stopped, or to start over when
 *        it has nothing to go on from. Also while finishing or activating: the device, with the
 *        whole image in, answers FINISH again
 */
void dfukit_client_resume(dfukit_client_t *client, uint32_t timestamp);

#ifdef __cplusplus
}
#endif

#endif // DFUKIT_CLIENT_H
