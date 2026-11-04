"""A bridge: a device whose backend passes a session on, unread, to a device behind it."""

from __future__ import annotations

import struct

from simulator import BOARD, HOST, OP_START, OTHER, Device, Link, answer_to_activate, image

import dfukit

BEHIND_PEER = b"\x09"


class Bridge:
    def __init__(self) -> None:
        self.device = Device()
        self.own = self.device.add_board(BOARD)
        self.behind = Device()
        self.behind_ram = self.behind.add_board(OTHER)
        self.behind_ram.behave(window=4096)
        self.ends = 0
        self.now = 0
        self.behind.on_host = self._from_behind
        self.device.add(dfukit.PassthroughBackend(OTHER, 256 * 1024, self._forward, self._end))

    def _forward(self, frame: bytes) -> None:
        self.behind.feed(BEHIND_PEER, frame, self.now)

    def _end(self) -> None:
        self.ends += 1

    def _from_behind(self, _to: bytes, frame: bytes) -> None:
        self.device.downstream(frame, self.now)
        # What the backend counts as done: the device behind it activated
        if dfukit.frame_activated(frame):
            self.device.passthrough_end()

    def link(self, data: bytes, **options) -> Link:  # type: ignore[no-untyped-def]
        link = Link(self.device, data, OTHER, now=self.now, **options)
        original = link.step

        def step() -> None:
            self.now = link.now
            original()

        link.step = step  # type: ignore[method-assign]
        return link


def test_session_goes_through_unread() -> None:
    bridge = Bridge()
    data = image(40_000)

    assert bridge.link(data).run().client.state == dfukit.ClientState.DONE

    assert bridge.behind_ram.image == data and bridge.behind_ram.activated
    assert bridge.own.begins == 0
    assert bridge.device.state == dfukit.SessionState.IDLE


def test_bridge_reports_only_its_state() -> None:
    bridge = Bridge()

    bridge.link(image(40_000)).run()

    assert all(s.done == 0 for s in bridge.device.statuses)
    assert bridge.device.statuses[-1].state == dfukit.SessionState.IDLE


def test_its_end_is_told_once() -> None:
    bridge = Bridge()

    bridge.link(image(40_000)).run()

    assert bridge.ends == 1


def test_lossy_link() -> None:
    bridge = Bridge()
    data = image(40_000)

    link = bridge.link(data, seed=3, drop=5, duplicate=2, delay=10).run()

    assert link.client.state == dfukit.ClientState.DONE and bridge.behind_ram.image == data


def test_failure_downstream_ends_the_session_at_once() -> None:
    bridge = Bridge()

    link = bridge.link(image(1000), image_size=257 * 1024).run(1000)
    assert link.client.state == dfukit.ClientState.FAILED

    link.run_until(lambda: not link.queue, 100)  # its ABORT on the way
    assert bridge.device.state == dfukit.SessionState.IDLE


def test_new_address_gets_busy_until_the_idle_time_then_starts_over() -> None:
    bridge = Bridge()
    data = image(40_000)
    link = bridge.link(data)
    link.run_until(lambda: link.client.acked >= 15_000)

    link.new_address()
    link.resume()
    link.run(1000)
    assert (
        link.client.state == dfukit.ClientState.FAILED and link.client.status == dfukit.Status.BUSY
    )

    ends = bridge.ends
    link.idle_time(11_000)
    assert bridge.device.state == dfukit.SessionState.IDLE and bridge.ends == ends + 1

    bridge.now = link.now
    assert bridge.link(data).run().client.state == dfukit.ClientState.DONE
    assert bridge.behind_ram.image == data


def test_the_bridges_own_image_goes_to_its_own_backend() -> None:
    bridge = Bridge()
    data = image(10_000)

    assert Link(bridge.device, data, BOARD).run().client.state == dfukit.ClientState.DONE
    assert bridge.own.image == data


def test_lost_answer_to_activate_is_not_a_failure() -> None:
    bridge = Bridge()
    data = image(40_000)
    link = bridge.link(data)
    link.lose = answer_to_activate

    assert link.run().client.state == dfukit.ClientState.DONE
    assert bridge.behind_ram.begins == 1 and bridge.behind_ram.image == data


def test_an_activate_passed_through_says_nothing_about_the_next_session() -> None:
    bridge = Bridge()
    bridge.link(image(40_000)).run()

    # The bridge's own session has the same number as the one behind it activated
    assert Link(bridge.device, image(5000), BOARD).run().client.state == dfukit.ClientState.DONE
    assert bridge.own.activated


def test_the_owner_moving_on_to_the_bridges_own_board_ends_the_session() -> None:
    bridge = Bridge()
    first = bridge.link(image(10_000), activate=False).run()
    assert (
        first.client.state == dfukit.ClientState.DONE
        and bridge.device.state == dfukit.SessionState.PASSTHROUGH
    )
    data = image(5000)

    link = Link(bridge.device, data, BOARD, now=first.now).run()

    assert link.client.state == dfukit.ClientState.DONE and bridge.own.image == data
    assert bridge.ends == 1


def test_a_backend_may_end_the_session_from_its_own_abort() -> None:
    device = Device()
    ends = []

    def end() -> None:
        ends.append(1)
        device.passthrough_end()

    device.add(dfukit.PassthroughBackend(OTHER, 1000, lambda _frame: None, end))
    device.feed(HOST, struct.pack("<BIBBIII", OP_START, OTHER, 0, 0, 10, 10, 0), 0)
    assert device.state == dfukit.SessionState.PASSTHROUGH

    device.passthrough_end()

    assert ends == [1] and device.state == dfukit.SessionState.IDLE


def test_resume_after_the_answer_to_activate_was_lost_does_not_start_over() -> None:
    bridge = Bridge()
    data = image(40_000)
    link = bridge.link(data)
    link.lose = answer_to_activate
    link.run_until(lambda: bridge.behind_ram.activated)

    link.resume()

    assert link.run().client.state == dfukit.ClientState.DONE
    assert bridge.behind_ram.begins == 1, "begin would erase the image just activated"
    assert bridge.behind_ram.image == data
