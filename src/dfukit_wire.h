// dfukit 2's wire format (DESIGN.md, Messages): the opcodes, and each frame as a packed struct

#ifndef DFUKIT_WIRE_H
#define DFUKIT_WIRE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Host to device; the device's answer to one has the high bit set
#define DFUKIT_OP_INFO     0x01
#define DFUKIT_OP_START    0x02
#define DFUKIT_OP_DATA     0x03
#define DFUKIT_OP_FINISH   0x04
#define DFUKIT_OP_ACTIVATE 0x05
#define DFUKIT_OP_ABORT    0x06
#define DFUKIT_OP_STATUS   0x07
#define DFUKIT_OP_ANSWER   0x80
// Device to host only: the answer to DATA, sent on the device's own cadence
#define DFUKIT_OP_ACK      (DFUKIT_OP_ANSWER | DFUKIT_OP_DATA)

// The protocol is little-endian, and so are these structs in memory
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "dfukit's frames are packed structs: a little-endian target only"
#endif

// A frame's layout, byte for byte. may_alias: a frame is read through these from a byte buffer,
// which only GCC and Clang need to be told
#if defined(__GNUC__)
#define DFUKIT_FRAME __attribute__((may_alias))
#else
#define DFUKIT_FRAME
#endif

#pragma pack(push, 1)

// START: which image, for which board
typedef struct DFUKIT_FRAME {
	uint8_t op;
	uint32_t board_id;
	uint8_t encoding;
	uint8_t flags;
	uint32_t image_size;
	uint32_t stream_size;
	uint32_t crc32;
} dfukit_start_frame_t;

// START's answer: the session, where to start from, and how large a frame's data may be
typedef struct DFUKIT_FRAME {
	uint8_t op;
	uint8_t status;
	uint16_t session;
	uint32_t resume;
	uint32_t window;
	uint16_t max_data;
} dfukit_start_answer_t;

// DATA: a piece of the stream. Without data, a probe: where does the device stand
typedef struct DFUKIT_FRAME {
	uint8_t op;
	uint16_t session;
	uint32_t offset;
	uint8_t data[];
} dfukit_data_frame_t;

// DATA's answer: taken up to offset, and how much more it may send
typedef struct DFUKIT_FRAME {
	uint8_t op;
	uint16_t session;
	uint8_t status;
	uint32_t offset;
	uint32_t window;
} dfukit_ack_frame_t;

// FINISH and ABORT
typedef struct DFUKIT_FRAME {
	uint8_t op;
	uint16_t session;
} dfukit_session_frame_t;

// ACTIVATE
typedef struct DFUKIT_FRAME {
	uint8_t op;
	uint16_t session;
	uint8_t flags;
	uint16_t delay_ms;
} dfukit_activate_frame_t;

// The answer to ACTIVATE and ABORT
typedef struct DFUKIT_FRAME {
	uint8_t op;
	uint8_t status;
} dfukit_answer_frame_t;

// FINISH's answer, with why it failed
#define DFUKIT_REASON_MAX 64
typedef struct DFUKIT_FRAME {
	uint8_t op;
	uint8_t status;
	uint8_t reason_len;
	char reason[DFUKIT_REASON_MAX];
} dfukit_finish_answer_t;

// STATUS's answer
typedef struct DFUKIT_FRAME {
	uint8_t op;
	uint8_t state;
	uint16_t session;
	uint32_t offset;
	uint32_t board_id;
} dfukit_status_answer_t;

// INFO's answer: op, version, name length, name, board count, then the boards, each this and its
// TLV entries
typedef struct DFUKIT_FRAME {
	uint32_t board_id;
	uint8_t kind;
	uint32_t max_size;
	uint8_t encodings;
	uint8_t window_log2; // for an encoding that has a window, else 0
	uint16_t tlv_len;
} dfukit_info_board_t;

#define DFUKIT_TLV_NAME    1
#define DFUKIT_TLV_VERSION 2

#pragma pack(pop)

// The sizes as documented (DESIGN.md, Messages): a compile error if a layout ever moves
#define DFUKIT_CHECK_SIZE(type, size) typedef char type##_size[sizeof(type) == (size) ? 1 : -1]
DFUKIT_CHECK_SIZE(dfukit_start_frame_t, 19);
DFUKIT_CHECK_SIZE(dfukit_start_answer_t, 14);
DFUKIT_CHECK_SIZE(dfukit_data_frame_t, 7);
DFUKIT_CHECK_SIZE(dfukit_ack_frame_t, 12);
DFUKIT_CHECK_SIZE(dfukit_session_frame_t, 3);
DFUKIT_CHECK_SIZE(dfukit_activate_frame_t, 6);
DFUKIT_CHECK_SIZE(dfukit_answer_frame_t, 2);
DFUKIT_CHECK_SIZE(dfukit_finish_answer_t, 3 + DFUKIT_REASON_MAX);
DFUKIT_CHECK_SIZE(dfukit_status_answer_t, 12);
DFUKIT_CHECK_SIZE(dfukit_info_board_t, 13);

#ifdef __cplusplus
}
#endif

#endif // DFUKIT_WIRE_H
