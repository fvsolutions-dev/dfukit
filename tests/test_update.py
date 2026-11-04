"""A normal link, from START to ACTIVATE."""

import pytest
from simulator import Device, Link, image

import dfukit


def test_image_arrives_and_activates() -> None:
    device = Device()
    ram = device.add_board()
    data = image(100_000)

    link = Link(device, data).run()

    assert link.client.state == dfukit.ClientState.DONE
    assert link.client.resent == 0
    assert ram.image == data and ram.activated
    assert (ram.begins, ram.aborts) == (1, 0)
    assert device.state == dfukit.SessionState.IDLE


def test_status_reports_progress_and_the_end() -> None:
    device = Device()
    device.add_board()

    Link(device, image(100_000)).run()

    states = [s.state for s in device.statuses]
    assert states.count(dfukit.SessionState.TRANSFER) >= 100_000 // 8192
    assert len(device.statuses) < 100
    done = [s.done for s in device.statuses if s.state == dfukit.SessionState.TRANSFER]
    assert done == sorted(done), "progress never goes back"
    last = device.statuses[-1]
    assert last.state == dfukit.SessionState.IDLE and last.activated and last.total == 100_000


# Empty, one byte, one frame exactly (512 less the DATA header)
@pytest.mark.parametrize("size", [0, 1, 505])
def test_sizes_at_the_edges(size: int) -> None:
    device = Device()
    ram = device.add_board()
    data = image(size)

    assert Link(device, data).run().client.state == dfukit.ClientState.DONE
    assert ram.activated and ram.image == data


def test_client_stays_within_its_own_largest_frame() -> None:
    device = Device(max_frame=4000)
    ram = device.add_board()
    data = image(100_000)

    link = Link(device, data, max_frame=0).run()

    assert link.client.state == dfukit.ClientState.DONE and ram.image == data
    assert link.client.max_data == 1024 - 7
