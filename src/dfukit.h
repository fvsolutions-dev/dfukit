// dfukit 2: the types both ends share (DESIGN.md)

#ifndef DFUKIT_H
#define DFUKIT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DFUKIT_PROTOCOL_VERSION 2

// A frame's sender or receiver, as the transport knows it: an opaque address (a socket, a BLE
// connection, a bus address). Two peers are the same when their bytes are
#define DFUKIT_PEER_MAX 16

typedef struct {
	uint8_t len;
	uint8_t bytes[DFUKIT_PEER_MAX];
} dfukit_peer_t;

typedef enum {
	DFUKIT_OK = 0x00,
	// A DATA frame past the expected offset; the ACK's offset is where to go on
	DFUKIT_GAP = 0x01,
	// Another peer owns the session
	DFUKIT_BUSY = 0x02,
	// No backend registered for START's board ID
	DFUKIT_UNKNOWN_BOARD = 0x03,
	DFUKIT_TOO_LARGE = 0x04,
	// No state to resume from
	DFUKIT_NO_RESUME = 0x05,
	// The stream is not the size START said
	DFUKIT_BAD_STREAM = 0x06,
	DFUKIT_CRC_MISMATCH = 0x07,
	// The backend refused; the reason has its text
	DFUKIT_BACKEND = 0x08,
	// A session number nobody knows
	DFUKIT_NO_SESSION = 0x09,
	// A request that is too short, or an unknown encoding
	DFUKIT_BAD_FRAME = 0x0a,
	// FINISH's answer while the backend is still at work: ask again
	DFUKIT_PENDING = 0x0b,
	// The client's own, never on the wire: no answer after all its tries
	DFUKIT_TIMEOUT = 0x7f,
} dfukit_status_t;

// The largest frame either end builds (a buffer on the stack); a transport may carry less
#ifndef DFUKIT_MAX_FRAME
#define DFUKIT_MAX_FRAME 1024
#endif

typedef enum {
	DFUKIT_ENCODING_RAW = 0,
} dfukit_encoding_t;

// What a backend does with an image: writes it, or passes the session on
#define DFUKIT_KIND_IMAGE       0
#define DFUKIT_KIND_PASSTHROUGH 1

// Encodings as INFO's bitmask
#define DFUKIT_ENCODING_BIT(encoding) (1u << (encoding))

// START's flags
#define DFUKIT_START_RESUME 0x01

// ACTIVATE's flags
#define DFUKIT_ACTIVATE_AT_NEXT_BOOT 0x01

// What a START asks for
typedef struct {
	uint32_t board_id;
	dfukit_encoding_t encoding;
	uint8_t flags;
	// The image
	uint32_t image_size;
	// The stream that carries it: the image itself for raw
	uint32_t stream_size;
	// CRC-32 of the image (dfukit_crc32.h)
	uint32_t crc32;
} dfukit_start_t;

/** @brief A status's name, for logs and messages. */
const char *dfukit_status_name(dfukit_status_t status);

#ifdef __cplusplus
}
#endif

#endif // DFUKIT_H
