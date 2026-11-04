"""Deflated images, decoded on the device as they come in."""

from __future__ import annotations

import struct
import zlib

from simulator import BOARD, OP_ANSWER, OP_INFO, STRANGER, Device, Link

import dfukit

DEFLATE = 1
WINDOW_LOG2 = 12


def compress(data: bytes) -> bytes:
    compressor = zlib.compressobj(9, zlib.DEFLATED, -WINDOW_LOG2)
    return compressor.compress(data) + compressor.flush()


def text_image() -> bytes:
    return b"".join(b"line %05u value %u\n" % (line, line % 97) for line in range(4000))


def with_inflater() -> Device:
    device = Device()
    device.set_inflater(dfukit.Inflate(WINDOW_LOG2))
    return device


def test_image_arrives_decoded() -> None:
    device = with_inflater()
    ram = device.add_board()
    data = text_image()
    stream = compress(data)
    assert len(stream) < len(data) / 3

    link = Link(device, data, encoding=DEFLATE, stream=stream).run()

    assert link.client.state == dfukit.ClientState.DONE and ram.activated and ram.image == data


def test_lossy_link() -> None:
    device = with_inflater()
    ram = device.add_board()
    ram.behave(window=4096)
    data = text_image()

    for seed in range(1, 6):
        link = Link(
            device, data, encoding=DEFLATE, stream=compress(data), seed=seed, drop=5, delay=10
        ).run()
        assert link.client.state == dfukit.ClientState.DONE and ram.image == data


def test_buffer_the_exact_size_of_the_image() -> None:
    # The whole image is decoded before the stream's last bytes have come in
    data = text_image()

    # Each frame size splits the stream's end differently
    for max_frame in range(40, 140):
        device = with_inflater()
        ram = device.add_board(size=len(data))
        link = Link(device, data, encoding=DEFLATE, stream=compress(data), max_frame=max_frame)

        assert link.run().client.state == dfukit.ClientState.DONE, max_frame
        assert ram.image == data


def test_resume_goes_on_decoding() -> None:
    device = with_inflater()
    ram = device.add_board()
    ram.behave(window=4096)
    data = text_image()
    link = Link(device, data, encoding=DEFLATE, stream=compress(data))
    link.run_until(lambda: link.client.acked > 4000)

    link.lose_link()
    link.resume()

    assert link.run().client.state == dfukit.ClientState.DONE
    assert ram.begins == 1 and ram.image == data


def test_a_second_image_starts_a_new_stream() -> None:
    device = with_inflater()
    ram = device.add_board()
    first, second = text_image(), text_image()[::-1]

    Link(device, first, encoding=DEFLATE, stream=compress(first)).run()
    Link(device, second, encoding=DEFLATE, stream=compress(second)).run()

    assert ram.image == second


def test_corrupted_stream_never_activates() -> None:
    device = with_inflater()
    ram = device.add_board()
    data = text_image()
    stream = bytearray(compress(data))
    stream[len(stream) // 2] ^= 0x10

    link = Link(device, data, encoding=DEFLATE, stream=bytes(stream)).run()

    assert link.client.status in (dfukit.Status.BAD_STREAM, dfukit.Status.CRC_MISMATCH)
    assert not ram.activated and device.state == dfukit.SessionState.IDLE


def test_stream_cut_short_never_activates() -> None:
    device = with_inflater()
    ram = device.add_board()
    data = text_image()

    link = Link(device, data, encoding=DEFLATE, stream=compress(data)[:-20]).run()

    assert link.client.status == dfukit.Status.BAD_STREAM and not ram.activated


def test_bytes_after_the_stream_are_refused() -> None:
    device = with_inflater()
    ram = device.add_board()
    data = text_image()

    link = Link(device, data, encoding=DEFLATE, stream=compress(data) + b"more").run()

    assert link.client.status == dfukit.Status.BAD_STREAM and not ram.activated


def test_image_larger_than_it_said_is_refused() -> None:
    device = with_inflater()
    ram = device.add_board()
    # A small stream that decodes to far more than the image it says it is
    stream = compress(bytes(100_000))

    link = Link(device, bytes(1000), encoding=DEFLATE, stream=stream).run()

    assert link.client.status == dfukit.Status.BAD_STREAM and not ram.activated


def test_stream_far_larger_than_its_image_is_refused() -> None:
    device = with_inflater()
    device.add_board()

    link = Link(device, bytes(1000), encoding=DEFLATE, stream=bytes(100_000)).run(5000)

    assert link.client.status == dfukit.Status.TOO_LARGE


def test_server_without_an_inflater_takes_raw_only() -> None:
    device = Device()
    device.add_board()
    data = text_image()

    link = Link(device, data, encoding=DEFLATE, stream=compress(data)).run()

    assert link.client.status == dfukit.Status.BAD_FRAME


def info_encodings_and_window(device: Device) -> tuple[int, int]:
    device.feed(STRANGER, bytes([OP_INFO]), 0)
    answer = device.stranger_heard[-1]
    assert answer[0] == OP_ANSWER | OP_INFO
    # Op, version, the name's length and the name, the board count, then ID, kind and largest image
    at = 3 + answer[2] + 1 + 9
    return struct.unpack_from("<BB", answer, at)


def test_info_announces_deflate_only_with_an_inflater() -> None:
    without = Device()
    without.add_board(BOARD)
    assert info_encodings_and_window(without) == (0b01, 0)

    device = with_inflater()
    device.add_board(BOARD)
    assert info_encodings_and_window(device) == (0b11, 15)
