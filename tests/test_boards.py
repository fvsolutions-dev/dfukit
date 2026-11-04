"""Two boards on one device, and how INFO describes them."""

import struct

from simulator import BOARD, OP_ANSWER, OP_INFO, OTHER, STRANGER, Device, Link, image

import dfukit


def test_each_image_goes_to_its_own_backend() -> None:
    device = Device()
    first, second = device.add_board(BOARD), device.add_board(OTHER)

    assert Link(device, image(5000), OTHER).run().client.state == dfukit.ClientState.DONE
    assert first.begins == 0 and second.written == 5000

    assert Link(device, image(7000), BOARD).run().client.state == dfukit.ClientState.DONE
    assert first.written == 7000 and second.begins == 1


def test_info_describes_the_boards() -> None:
    device = Device()
    device.add_board(BOARD, version="1.2.3")
    device.add_board(OTHER, size=1000)

    device.feed(STRANGER, bytes([OP_INFO]), 0)

    info = device.stranger_heard[-1]
    assert info[0] == OP_ANSWER | OP_INFO and info[1] == 2
    assert info[2:7] == b"\x04test"
    assert info[7] == 2

    # The last one registered comes first: its ID, kind, largest image, encodings
    board, kind, largest, encodings = struct.unpack_from("<IBIB", info, 8)
    assert (board, kind, largest, encodings) == (OTHER, 0, 1000, 1)
    assert info[19:21] == struct.pack("<H", 5) and info[21:23] == b"\x01\x03"

    second = 8 + 13 + 5
    assert struct.unpack_from("<I", info, second)[0] == BOARD
    tail = info[second + 13 :]
    assert tail[:2] == b"\x01\x03"
    assert tail[5:7] == b"\x02\x05" and tail[7:12] == b"1.2.3"
    assert len(info) == second + 13 + 12


def test_info_stays_within_the_largest_frame() -> None:
    heard: list[bytes] = []
    server = dfukit.Server(lambda _peer, frame: heard.append(frame), name="x" * 64, max_frame=23)
    server.add(dfukit.RamBackend(BOARD, 1000))

    server.feed(STRANGER, bytes([OP_INFO]), 0)

    assert len(heard[-1]) == 23
