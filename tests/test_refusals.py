"""What the device refuses, and that a refusal leaves it in order."""

import struct

from simulator import (
    BOARD,
    HOST,
    OP_ABORT,
    OP_ACK,
    OP_ANSWER,
    OP_DATA,
    OP_INFO,
    OP_START,
    OTHER,
    STRANGER,
    Device,
    Link,
    image,
)

import dfukit


def start_frame(encoding: int = 0) -> bytes:
    return struct.pack("<BIBBIII", OP_START, BOARD, encoding, 0, 10, 10, 0)


def test_unknown_board_never_reaches_a_backend() -> None:
    device = Device()
    ram = device.add_board()

    assert Link(device, image(1000), OTHER).run().client.status == dfukit.Status.UNKNOWN_BOARD
    assert ram.begins == 0


def test_too_large_is_no_session() -> None:
    device = Device()
    ram = device.add_board(size=50_000)

    assert Link(device, image(60_000)).run().client.status == dfukit.Status.TOO_LARGE
    assert ram.begins == 0
    assert device.statuses == [], "a refused START reports nothing"


def test_stream_size_must_match_a_raw_image() -> None:
    device = Device()
    device.add_board()

    link = Link(device, image(1000), stream=image(1001)).run()

    assert link.client.status == dfukit.Status.BAD_FRAME


def test_wrong_crc_refused_at_finish() -> None:
    device = Device()
    ram = device.add_board()
    data = image(20_000)

    link = Link(device, data, crc32=1).run()

    assert (
        link.client.state == dfukit.ClientState.FAILED
        and link.client.status == dfukit.Status.CRC_MISMATCH
    )
    assert ram.aborts == 1 and not ram.activated
    assert device.state == dfukit.SessionState.IDLE
    assert not device.statuses[-1].activated


def test_finished_but_never_activated_is_dropped() -> None:
    device = Device()
    ram = device.add_board()

    link = Link(device, image(20_000), activate=False).run()

    assert (
        link.client.state == dfukit.ClientState.DONE and device.state == dfukit.SessionState.READY
    )
    link.idle_time(61_000)
    assert device.state == dfukit.SessionState.IDLE and ram.aborts == 1


def test_activate_refused_keeps_the_session() -> None:
    device = Device()
    ram = device.add_board()
    ram.behave(refuse_activate=True)

    link = Link(device, image(5000)).run()

    assert link.client.status == dfukit.Status.BACKEND
    assert device.state == dfukit.SessionState.READY


def test_another_peer_gets_busy_and_the_owner_goes_on() -> None:
    device = Device()
    ram = device.add_board()
    data = image(20_000)
    link = Link(device, data)
    link.run(5)
    assert device.state == dfukit.SessionState.TRANSFER

    device.feed(STRANGER, start_frame(), link.now)
    assert device.stranger_heard[-1][:2] == bytes([OP_ANSWER | OP_START, dfukit.Status.BUSY])

    device.feed(STRANGER, struct.pack("<BHI", OP_DATA, device.session, 0) + b"x", link.now)
    answer = device.stranger_heard[-1]
    assert answer[0] == OP_ACK and answer[3] == dfukit.Status.BUSY

    assert link.run().client.state == dfukit.ClientState.DONE
    assert ram.activated and ram.image == data


def test_stray_abort_and_bad_start_leave_the_session() -> None:
    device = Device()
    ram = device.add_board()
    link = Link(device, image(20_000))
    link.run(5)

    device.feed(HOST, struct.pack("<BH", OP_ABORT, device.session + 1), link.now)
    device.feed(HOST, start_frame(encoding=7), link.now)

    assert device.state == dfukit.SessionState.TRANSFER
    assert link.run().client.state == dfukit.ClientState.DONE and ram.aborts == 0


def test_abort_without_a_session_is_answered() -> None:
    device = Device()
    device.add_board()

    device.feed(STRANGER, struct.pack("<BH", OP_ABORT, 1), 0)

    assert device.stranger_heard[-1][:2] == bytes([OP_ANSWER | OP_ABORT, 0])


def test_frames_without_a_session_are_answered_so() -> None:
    device = Device()
    device.add_board()

    device.feed(STRANGER, struct.pack("<BH", 0x04, 1), 0)  # FINISH
    assert device.stranger_heard[-1][1] == dfukit.Status.NO_SESSION


def test_short_and_unknown_frames_are_refused() -> None:
    device = Device()
    device.add_board()

    for frame in [bytes([OP_START]), bytes([0x04]), bytes([0x05]), bytes([OP_ABORT])]:
        device.feed(STRANGER, frame, 0)
        assert device.stranger_heard[-1][1] == dfukit.Status.BAD_FRAME
    device.feed(STRANGER, bytes([OP_DATA]), 0)
    assert device.state == dfukit.SessionState.IDLE


def test_peer_longer_than_16_bytes_is_dropped() -> None:
    device = Device()
    device.add_board()

    device.feed(bytes(17), bytes([OP_INFO]), 0)

    assert device.stranger_heard == []


def test_owner_starting_over_replaces_the_session() -> None:
    device = Device()
    ram = device.add_board()
    link = Link(device, image(20_000))
    link.run(5)

    # Another image from the same host
    device.feed(HOST, start_frame(), link.now)

    assert ram.begins == 2 and ram.aborts == 1
    assert device.state == dfukit.SessionState.TRANSFER


def test_data_overlapping_what_was_taken_is_cut_to_the_new_part() -> None:
    device = Device()
    ram = device.add_board()
    device.feed(HOST, start_frame(), 0)

    device.feed(HOST, struct.pack("<BHI", OP_DATA, device.session, 0) + b"abcd", 0)
    device.feed(HOST, struct.pack("<BHI", OP_DATA, device.session, 2) + b"cdef", 0)

    assert ram.image == b"abcdef"


def test_peer_of_256_bytes_is_dropped_not_wrapped_to_empty() -> None:
    device = Device()
    device.add_board()

    device.feed(bytes(256), bytes([OP_INFO]), 0)

    assert device.stranger_heard == []


def test_unknown_opcode_gets_no_answer() -> None:
    device = Device()
    device.add_board()

    device.feed(STRANGER, bytes([0x40, 0, 0, 0]), 0)

    assert device.stranger_heard == []
