"""DFU Session Protocol v1.1 — wire constants and frame helpers.

This module mirrors dfu_session_wire.h field-for-field.  That C header is
the canonical source of truth; if the two diverge the size asserts at the
bottom of this file will catch it at import time.

All multi-byte integers are little-endian on the wire (ESP32 native order).
"""
from __future__ import annotations

import struct
from enum import IntEnum


# --------------------------------------------------------------------------
# Opcodes
# --------------------------------------------------------------------------

class Op(IntEnum):
    START      = 0x01   # host → device: begin session
    FINISH     = 0x02   # host → device: all bytes sent, verify+stage
    APPLY      = 0x03   # host → device: reboot into staged image
    CONFIRM    = 0x04   # host → device: commit running slot
    ABORT      = 0x05   # host → device: tear down session
    GET_STATE  = 0x06   # host → device: query current state
    DATA       = 0x10   # host → device: one data chunk
    RSP_FLAG   = 0x80   # device → host: response = opcode | RSP_FLAG
    PROGRESS   = 0x90   # device → host: async progress notify


# --------------------------------------------------------------------------
# Status codes
# --------------------------------------------------------------------------

class Status(IntEnum):
    OK             = 0
    INVALID_STATE  = 1
    HW_MISMATCH    = 2
    TOO_LARGE      = 3
    OFFSET_GAP     = 4
    FLASH_ERR      = 5
    SHA_MISMATCH   = 6
    SIG_INVALID    = 7
    TIMEOUT        = 8
    BUSY           = 9
    DOWNGRADE      = 10
    RESUME_INVALID = 11


# --------------------------------------------------------------------------
# Session states (carried in PROGRESS.state and GET_STATE response)
# --------------------------------------------------------------------------

class State(IntEnum):
    IDLE      = 0
    TRANSFER  = 1
    FINISHING = 2   # FINISH received, draining/verifying
    READY     = 3   # image staged + verified, await APPLY
    SUSPENDED = 4   # peer gone mid-transfer; held for resume


# --------------------------------------------------------------------------
# Struct definitions (all little-endian)
#
# Field order and sizes MUST match the packed C structs in dfu_session_wire.h.
# The _Static_asserts in that header enforce the C side; the assertions at the
# bottom of this file enforce the Python side.
# --------------------------------------------------------------------------

# START request payload (after opcode byte): 48 bytes.
# proto u8, reserved u8, size u32, sha256 u8[32], fw_ver u32, hw_id u16, min_fw u32
START_REQ = struct.Struct("<BB I 32s I H I")
assert START_REQ.size == 48, f"START_REQ must be 48 bytes, got {START_REQ.size}"

# START response extra (after opcode + status): 10 bytes.
# resume u32, max_payload u16, window u32
START_RSP = struct.Struct("<I H I")
assert START_RSP.size == 10, f"START_RSP must be 10 bytes, got {START_RSP.size}"

# PROGRESS body (after opcode byte): 10 bytes.
# status u8, state u8, acked u32, crc u32
PROGRESS_BODY = struct.Struct("<BB II")
assert PROGRESS_BODY.size == 10, f"PROGRESS_BODY must be 10 bytes, got {PROGRESS_BODY.size}"

# GET_STATE response extra (after opcode + status): 9 bytes.
# state u8, acked u32, image_size u32
GET_STATE_RSP = struct.Struct("<B II")
assert GET_STATE_RSP.size == 9, f"GET_STATE_RSP must be 9 bytes, got {GET_STATE_RSP.size}"

# DATA frame header (before payload): 5 bytes.
# op u8, offset u32
DATA_HDR = struct.Struct("<BI")
assert DATA_HDR.size == 5, f"DATA_HDR must be 5 bytes, got {DATA_HDR.size}"

PROTO_VER = 1


# --------------------------------------------------------------------------
# Frame build/parse helpers
# --------------------------------------------------------------------------

def build_start(size: int, sha256: bytes, fw_ver: int,
                hw_id: int, min_fw: int) -> bytes:
    """Build a full START frame (opcode + 48-byte payload)."""
    assert len(sha256) == 32
    payload = START_REQ.pack(PROTO_VER, 0, size, sha256, fw_ver, hw_id, min_fw)
    return bytes([Op.START]) + payload


def build_data(offset: int, payload: bytes) -> bytes:
    """Build a DATA frame."""
    return DATA_HDR.pack(Op.DATA, offset) + payload


def build_single_op(opcode: Op) -> bytes:
    """Build a no-payload command frame (FINISH, GET_STATE, ABORT, CONFIRM)."""
    return bytes([opcode])


def build_apply(delay_ms: int = 500) -> bytes:
    """Build an APPLY frame."""
    delay_ms = max(delay_ms, 200)
    return bytes([Op.APPLY]) + struct.pack("<H", delay_ms)


def parse_rsp(data: bytes) -> tuple[int, int, bytes]:
    """Parse a RSP frame.  Returns (req_opcode, status, extra_bytes)."""
    if len(data) < 2:
        raise ValueError(f"RSP too short: {data.hex()}")
    op = data[0]
    if not (op & Op.RSP_FLAG):
        raise ValueError(f"Not a RSP frame: opcode=0x{op:02x}")
    req_op = op & ~Op.RSP_FLAG
    status = data[1]
    extra  = bytes(data[2:])
    return req_op, status, extra


def parse_progress(data: bytes) -> tuple[Status, State, int, int]:
    """Parse a PROGRESS frame.  Returns (status, state, acked, crc)."""
    if len(data) < 1 + PROGRESS_BODY.size:
        raise ValueError(f"PROGRESS too short: {data.hex()}")
    status_raw, state_raw, acked, crc = PROGRESS_BODY.unpack_from(data, 1)
    return Status(status_raw), State(state_raw), acked, crc


def parse_start_rsp_extra(extra: bytes) -> tuple[int, int, int]:
    """Parse START response extra.  Returns (resume_off, max_payload, window)."""
    if len(extra) < START_RSP.size:
        raise ValueError(f"START RSP extra too short: {extra.hex()}")
    return START_RSP.unpack_from(extra)


def parse_get_state_rsp_extra(extra: bytes) -> tuple[State, int, int]:
    """Parse GET_STATE response extra.  Returns (state, acked, image_size)."""
    if len(extra) < GET_STATE_RSP.size:
        raise ValueError(f"GET_STATE RSP extra too short: {extra.hex()}")
    state_raw, acked, image_size = GET_STATE_RSP.unpack_from(extra)
    return State(state_raw), acked, image_size


def status_name(s: int) -> str:
    try:
        return Status(s).name
    except ValueError:
        return f"UNKNOWN({s})"


def state_name(s: int) -> str:
    try:
        return State(s).name
    except ValueError:
        return f"UNKNOWN({s})"
