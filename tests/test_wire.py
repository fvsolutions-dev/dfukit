"""The frames byte by byte, as DESIGN.md documents them, checked apart from the C structs."""

import struct
import zlib

from simulator import BOARD, HOST, OP_ANSWER, OP_DATA, OP_FINISH, OP_START, STRANGER, Device, image

import dfukit


def client_frames(data: bytes) -> tuple[dfukit.Client, list[bytes]]:
    sent: list[bytes] = []
    client = dfukit.Client(sent.append, lambda offset, length: data[offset : offset + length])
    return client, sent


def test_start() -> None:
    data = image(1000)
    client, sent = client_frames(data)

    client.begin(BOARD, data, 0, flags=1)

    assert sent[-1] == bytes([OP_START]) + struct.pack(
        "<IBBIII", BOARD, 0, 1, 1000, 1000, zlib.crc32(data)
    )


def test_start_answer_and_ack() -> None:
    heard: list[bytes] = []
    device = Device()
    device.add_board()
    device.on_host = lambda _peer, frame: heard.append(frame)

    device.feed(HOST, struct.pack("<BIBBIII", OP_START, BOARD, 0, 0, 100, 100, 0), 0)

    assert heard[0] == struct.pack("<BBHIIH", OP_ANSWER | OP_START, 0, 1, 0, 0, 512 - 7)
    assert len(heard[1]) == 12
    assert struct.unpack("<BHBI", heard[1][:8]) == (OP_ANSWER | OP_DATA, 1, 0, 0)


def test_data_and_its_payload() -> None:
    data = bytes(range(100))
    device = Device()
    ram = device.add_board()
    device.feed(HOST, struct.pack("<BIBBIII", OP_START, BOARD, 0, 0, 100, 100, 0), 0)

    device.feed(HOST, struct.pack("<BHI", OP_DATA, device.session, 0) + data, 0)

    assert ram.image == data


def test_finish_answer_without_a_session_has_no_reason() -> None:
    device = Device()
    device.add_board()

    device.feed(STRANGER, struct.pack("<BH", OP_FINISH, 1), 0)

    assert device.stranger_heard[-1] == bytes([OP_ANSWER | OP_FINISH, 9, 0])


def test_status() -> None:
    device = Device()
    device.add_board()

    device.feed(STRANGER, bytes([0x07]), 0)

    assert device.stranger_heard[-1] == struct.pack("<BBHII", OP_ANSWER | 0x07, 0, 0, 0, 0)
