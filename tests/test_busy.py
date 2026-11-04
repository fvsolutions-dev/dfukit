"""A backend that is slow: the host waits as long as the device answers."""

import struct
import zlib

from simulator import BOARD, HOST, OP_DATA, OP_FINISH, OP_START, Device, Link, image

import dfukit


def test_waits_longer_than_its_tries_and_goes_on_at_once() -> None:
    device = Device()
    ram = device.add_board()
    ram.behave(window=4096, busy=True)
    data = image(50_000)
    link = Link(device, data)

    link.run(20_000)  # 8 tries of 500 ms are long over
    assert link.client.state == dfukit.ClientState.SENDING and link.client.acked == 0

    ram.behave(busy=False)
    freed = link.now

    assert link.run().client.state == dfukit.ClientState.DONE
    assert ram.image == data
    assert link.now - freed < 100, "the device says so itself, no waiting for a probe"


def test_busy_now_and_then_costs_no_resend_timeout() -> None:
    device = Device()
    ram = device.add_board()
    ram.behave(window=4096)
    data = image(50_000)
    link = Link(device, data)

    for _ in range(20_000):
        ram.behave(busy=link.now % 100 < 30)
        link.step()
        if link.client.state in (dfukit.ClientState.DONE, dfukit.ClientState.FAILED):
            break

    assert link.client.state == dfukit.ClientState.DONE and ram.image == data
    assert link.now < 300


def test_still_writing_at_finish() -> None:
    device = Device()
    ram = device.add_board()
    ram.behave(pending=20)

    assert Link(device, image(20_000)).run().client.state == dfukit.ClientState.DONE
    assert ram.pending == 0 and ram.finished


def test_still_writing_at_finish_on_a_lossy_link() -> None:
    device = Device()
    ram = device.add_board()
    ram.behave(pending=20)

    assert (
        Link(device, image(20_000), seed=3, drop=10, delay=5).run().client.state
        == dfukit.ClientState.DONE
    )


def test_a_window_near_the_limit_does_not_wrap() -> None:
    device = Device()
    ram = device.add_board()
    ram.behave(window=0xFFFFFFFF)

    assert Link(device, image(50_000)).run().client.state == dfukit.ClientState.DONE


def test_gives_up_when_stuck_at_window_0() -> None:
    device = Device()
    ram = device.add_board()
    ram.behave(window=4096, busy=True)

    link = Link(device, image(20_000), stall_ms=3000).run(10_000)

    assert link.client.status == dfukit.Status.TIMEOUT
    assert link.now < 3100


def test_finish_still_pending_keeps_a_suspended_session() -> None:
    device = Device()
    ram = device.add_board()
    data = image(100)
    device.feed(HOST, struct.pack("<BIBBIII", OP_START, BOARD, 0, 0, 100, 100, zlib.crc32(data)), 0)
    device.feed(HOST, struct.pack("<BHI", OP_DATA, device.session, 0) + data, 0)
    now = 11_000
    device.poll(now)
    assert device.state == dfukit.SessionState.SUSPENDED
    ram.behave(pending=1000)

    # The host asks every 500 ms, far past resume_ms
    for _ in range(140):
        device.feed(HOST, struct.pack("<BH", OP_FINISH, device.session), now)
        now += 500
        device.poll(now)

    assert ram.aborts == 0
    assert device.state == dfukit.SessionState.TRANSFER
