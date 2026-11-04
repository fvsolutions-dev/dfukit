"""A link that drops, duplicates and delays frames: the image arrives whole, resends bounded."""

import pytest
from simulator import OP_ANSWER, Device, Link, first_data, image

import dfukit


@pytest.mark.parametrize("seed", range(1, 11))
def test_losses_cost_a_window_each_at_most(seed: int) -> None:
    device = Device()
    ram = device.add_board()
    ram.behave(window=8192)
    data = image(200_000, seed)

    link = Link(device, data, seed=seed, drop=5, duplicate=3, delay=20).run()

    assert link.client.state == dfukit.ClientState.DONE and ram.image == data and ram.activated
    assert link.client.resent <= link.dropped * (8192 + 512)


def test_heavy_losses_still_get_through() -> None:
    device = Device()
    ram = device.add_board()
    data = image(50_000)

    link = Link(device, data, seed=99, drop=30, duplicate=10, delay=50).run()

    assert link.client.state == dfukit.ClientState.DONE and ram.image == data


@pytest.mark.parametrize("seed, drop, delay", [(1, 0, 0), (7, 5, 10)])
def test_small_window(seed: int, drop: int, delay: int) -> None:
    device = Device()
    ram = device.add_board()
    ram.behave(window=1000)
    data = image(30_000)

    link = Link(device, data, seed=seed, drop=drop, delay=delay).run()

    assert link.client.state == dfukit.ClientState.DONE and ram.image == data


def test_lost_start_is_sent_again() -> None:
    device = Device()
    ram = device.add_board()
    data = image(5000)
    link = Link(device, data)
    link.queue.clear()

    assert link.run().client.state == dfukit.ClientState.DONE
    assert ram.begins == 1 and ram.image == data


def test_late_finish_answer_is_ignored() -> None:
    device = Device()
    device.add_board()
    link = Link(device, image(5000))
    link.run(1)

    # An answer to a FINISH from before: the client is still sending
    link.client.feed(bytes([OP_ANSWER | 0x04, 0, 0]), link.now)

    assert link.run().client.state == dfukit.ClientState.DONE


def test_gap_at_offset_0_on_a_clock_near_0_rewinds_at_once() -> None:
    device = Device()
    device.add_board()
    link = Link(device, image(5000), now=0)
    link.lose = first_data

    assert link.run().client.state == dfukit.ClientState.DONE
    assert link.now < 100, "not after a resend timeout"
