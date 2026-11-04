"""A device and a host in one process: the library's Server and Client, and a link between them that
drops, duplicates and delays frames, on a clock of its own. The frames below are written out in
bytes on purpose, apart from the C code's."""

from __future__ import annotations

import random
from collections.abc import Callable
from dataclasses import dataclass

import dfukit

BOARD, OTHER = 0x0000A001, 0x0000B001
IMAGE_MAX = 256 * 1024
HOST, HOST_ELSEWHERE, STRANGER = b"\x01", b"\x01\x09", b"\x02"

# Frames (dfukit_wire.h)
OP_INFO, OP_START, OP_DATA, OP_FINISH, OP_ACTIVATE, OP_ABORT = 0x01, 0x02, 0x03, 0x04, 0x05, 0x06
OP_ANSWER = 0x80
OP_ACK = OP_ANSWER | OP_DATA
START_RESUME = 0x01


def image(size: int, seed: int = 1) -> bytes:
    return random.Random(seed).randbytes(size)


@dataclass
class StatusReport:
    state: dfukit.SessionState
    done: int
    total: int
    activated: bool


class Device(dfukit.Server):
    """A server that remembers what it reported. What it says to the host goes to on_host; to
    anyone else, to stranger_heard."""

    def __init__(self, max_frame: int = 512) -> None:
        super().__init__(self._send, self._on_status, "test", max_frame)
        self.statuses: list[StatusReport] = []
        self.stranger_heard: list[bytes] = []
        self.on_host: Callable[[bytes, bytes], None] | None = None

    def _send(self, peer: bytes, frame: bytes) -> None:
        if peer == STRANGER or self.on_host is None:
            self.stranger_heard.append(frame)
        else:
            self.on_host(peer, frame)

    def _on_status(
        self, state: dfukit.SessionState, done: int, total: int, activated: bool
    ) -> None:
        self.statuses.append(StatusReport(state, done, total, activated))

    def add_board(
        self, board: int = BOARD, size: int = IMAGE_MAX, version: str | None = None
    ) -> dfukit.RamBackend:
        ram = dfukit.RamBackend(board, size, version)
        self.add(ram)
        return ram


class Link:
    """A client on a link to a device, updating it. Options the client takes go to its begin()."""

    def __init__(
        self,
        device: Device,
        data: bytes,
        board: int = BOARD,
        *,
        seed: int = 1,
        drop: int = 0,
        duplicate: int = 0,
        delay: int = 0,
        stream: bytes | None = None,
        max_frame: int = 512,
        stall_ms: int = 0,
        now: int = 0,
        **begin_options: object,
    ) -> None:
        self.device, self.now = device, now
        self.stream = data if stream is None else stream
        self.random = random.Random(seed)
        self.drop, self.duplicate, self.delay = drop, duplicate, delay
        self.peer, self.cut = HOST, False
        self.queue: list[tuple[int, bool, bytes]] = []
        self.last_due = {True: 0, False: 0}
        self.dropped, self.limit = 0, 0
        # Set to lose one frame on purpose: the first that it returns True for
        self.lose: Callable[[bool, bytes], bool] | None = None
        device.on_host = self._from_device
        self.client = dfukit.Client(self._send, self._read, max_frame, stall_ms=stall_ms)
        self.client.begin(
            board, data, stream_size=len(self.stream), timestamp=self.now, **begin_options
        )

    def _send(self, frame: bytes) -> None:
        if frame[0] == OP_DATA:
            end = int.from_bytes(frame[3:7], "little") + len(frame) - 7
            assert end <= self.limit, "DATA went past the window the device announced"
        self._enqueue(True, frame)

    def _read(self, offset: int, length: int) -> bytes:
        return self.stream[offset : offset + length]

    def _from_device(self, to: bytes, frame: bytes) -> None:
        if to == self.peer:
            self._enqueue(False, frame)
        else:
            self.device.stranger_heard.append(frame)

    def _enqueue(self, to_device: bool, frame: bytes) -> None:
        if self.cut:
            return
        if self.lose is not None and self.lose(to_device, frame):
            self.lose = None
            return
        if self.random.randrange(100) < self.drop:
            self.dropped += 1
            return
        for _ in range(2 if self.random.randrange(100) < self.duplicate else 1):
            # Jitter, not reordering: a frame never overtakes the one sent before it
            due = self.now + 1 + (self.random.randrange(self.delay) if self.delay else 0)
            due = max(due, self.last_due[to_device])
            self.last_due[to_device] = due
            self.queue.append((due, to_device, frame))

    def deliver(self) -> None:
        due = [m for m in self.queue if m[0] <= self.now]
        self.queue = [m for m in self.queue if m[0] > self.now]
        for _, to_device, frame in due:
            if to_device:
                self.device.feed(self.peer, frame, self.now)
            else:
                if frame[0] == OP_ACK and frame[3] <= 1:
                    offset = int.from_bytes(frame[4:8], "little")
                    self.limit = max(self.limit, offset + int.from_bytes(frame[8:12], "little"))
                self.client.feed(frame, self.now)

    def step(self) -> None:
        self.deliver()
        self.device.poll(self.now)
        self.client.poll(self.now)
        self.now += 1

    def run(self, ms: int = 600_000) -> Link:
        """Until the client is done or has failed, or for ms at most"""
        for _ in range(ms):
            self.step()
            if self.client.state in (dfukit.ClientState.DONE, dfukit.ClientState.FAILED):
                break
        return self

    def run_until(self, condition: Callable[[], bool], ms: int = 60_000) -> None:
        for _ in range(ms):
            if condition():
                return
            self.step()
        raise AssertionError("not within the time")

    def idle_time(self, ms: int) -> None:
        """Time passes on the device, with nothing from the link"""
        self.now += ms
        self.device.poll(self.now)

    def new_address(self) -> None:
        """A reconnect: nothing on the old link arrives, and the device sees another peer"""
        self.queue.clear()
        self.peer = HOST_ELSEWHERE

    def lose_link(self) -> None:
        self.queue.clear()
        self.cut = True

    def resume(self) -> None:
        self.cut = False
        self.client.resume(self.now)


def answer_to_activate(to_device: bool, frame: bytes) -> bool:
    """For Link.lose: the device's answer to ACTIVATE"""
    return not to_device and frame[0] == OP_ANSWER | OP_ACTIVATE


def first_data(to_device: bool, frame: bytes) -> bool:
    """For Link.lose: the first DATA frame"""
    return to_device and frame[0] == OP_DATA
