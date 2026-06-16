#pragma once

/* DFU Session Protocol v1.1 — on-wire constants and packed frame layouts.
 *
 * THIS IS THE CANONICAL SOURCE OF TRUTH for the wire format. All numeric
 * values here are fixed protocol constants; changing them is a breaking
 * change that requires a version bump.
 *
 * The Python mirror is pydfu/src/dfukit/session/wire.py — keep the two in sync.
 *
 * See DFU-session.md for the full protocol spec. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------- */
/* Opcodes                                                                */
/* --------------------------------------------------------------------- */

typedef enum {
    OP_START     = 0x01,   /* host → device: begin session               */
    OP_FINISH    = 0x02,   /* host → device: all bytes sent, verify+stage */
    OP_APPLY     = 0x03,   /* host → device: reboot into staged image     */
    OP_CONFIRM   = 0x04,   /* host → device: commit running slot          */
    OP_ABORT     = 0x05,   /* host → device: tear down session            */
    OP_GET_STATE = 0x06,   /* host → device: query current state          */
    OP_DATA      = 0x10,   /* host → device: one data chunk               */
    OP_RSP_FLAG  = 0x80,   /* device → host: response = opcode | OP_RSP_FLAG */
    OP_PROGRESS  = 0x90,   /* device → host: async progress notify        */
} dfu_wire_op_t;

/* --------------------------------------------------------------------- */
/* Status codes                                                           */
/* --------------------------------------------------------------------- */

typedef enum {
    STS_OK             = 0,
    STS_INVALID_STATE  = 1,
    STS_HW_MISMATCH    = 2,
    STS_TOO_LARGE      = 3,
    STS_OFFSET_GAP     = 4,
    STS_FLASH_ERR      = 5,
    STS_SHA_MISMATCH   = 6,
    STS_SIG_INVALID    = 7,
    STS_TIMEOUT        = 8,
    STS_BUSY           = 9,
    STS_DOWNGRADE      = 10,
    STS_RESUME_INVALID = 11,
} dfu_wire_status_t;

/* --------------------------------------------------------------------- */
/* Session states (also carried in PROGRESS.state and GET_STATE.state)   */
/* --------------------------------------------------------------------- */

typedef enum {
    DFU_WIRE_ST_IDLE       = 0,
    DFU_WIRE_ST_TRANSFER   = 1,
    DFU_WIRE_ST_FINISHING  = 2,   /* FINISH received, draining/verifying */
    DFU_WIRE_ST_READY      = 3,   /* image staged + verified, await APPLY */
    DFU_WIRE_ST_SUSPENDED  = 4,   /* peer gone mid-transfer; held for resume */
} dfu_wire_state_t;

/* --------------------------------------------------------------------- */
/* Packed frame / payload structs                                         */
/* --------------------------------------------------------------------- */

/* START request payload (after the opcode byte): 48 bytes total.
 * proto u8, reserved u8, size u32, sha256 u8[32], fw_ver u32,
 * hw_id u16, min_fw u32. */
typedef struct __attribute__((packed)) {
    uint8_t  proto;          /* must equal PROTO_VER (1)  */
    uint8_t  reserved;       /* must be 0                  */
    uint32_t size;           /* total image bytes          */
    uint8_t  sha256[32];     /* SHA-256 of the full image  */
    uint32_t fw_ver;         /* sender's firmware version  */
    uint16_t hw_id;          /* hardware-ID gate           */
    uint32_t min_fw;         /* minimum-firmware gate      */
} dfu_wire_start_req_t;

_Static_assert(sizeof(dfu_wire_start_req_t) == 48,
               "dfu_wire_start_req_t must be exactly 48 bytes");

/* START response extra payload (after opcode + status): 10 bytes. */
typedef struct __attribute__((packed)) {
    uint32_t resume;         /* last flush boundary; 0 on a fresh session */
    uint16_t max_payload;    /* per-DATA payload cap (DATA header excl.)  */
    uint32_t window;         /* sliding-window byte budget                */
} dfu_wire_start_rsp_t;

_Static_assert(sizeof(dfu_wire_start_rsp_t) == 10,
               "dfu_wire_start_rsp_t must be exactly 10 bytes");

/* PROGRESS frame body (after the opcode byte): 10 bytes.
 * Full frame = [OP_PROGRESS | status u8 | state u8 | acked u32 | crc u32]
 * = 11 bytes including the leading opcode. */
typedef struct __attribute__((packed)) {
    uint8_t  status;         /* dfu_wire_status_t                         */
    uint8_t  state;          /* dfu_wire_state_t                          */
    uint32_t acked;          /* bytes confirmed written by dfu_core       */
    uint32_t crc;            /* CRC32 over acked bytes                    */
} dfu_wire_progress_body_t;

_Static_assert(sizeof(dfu_wire_progress_body_t) == 10,
               "dfu_wire_progress_body_t must be exactly 10 bytes");

/* GET_STATE response extra payload (after opcode + status): 9 bytes. */
typedef struct __attribute__((packed)) {
    uint8_t  state;          /* dfu_wire_state_t                          */
    uint32_t acked;          /* bytes confirmed written                   */
    uint32_t image_size;     /* total declared in START                   */
} dfu_wire_get_state_rsp_t;

_Static_assert(sizeof(dfu_wire_get_state_rsp_t) == 9,
               "dfu_wire_get_state_rsp_t must be exactly 9 bytes");

/* DATA frame header (before the payload): 5 bytes.
 * Full frame = [OP_DATA | offset u32 | payload…]. */
typedef struct __attribute__((packed)) {
    uint8_t  op;             /* must equal OP_DATA (0x10)                 */
    uint32_t offset;         /* wire offset; must equal session's next_off */
} dfu_wire_data_hdr_t;

_Static_assert(sizeof(dfu_wire_data_hdr_t) == 5,
               "dfu_wire_data_hdr_t must be exactly 5 bytes");

#ifdef __cplusplus
}
#endif
