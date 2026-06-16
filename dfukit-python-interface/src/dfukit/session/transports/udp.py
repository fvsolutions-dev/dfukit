"""UDP transport for the DFU Session protocol.

UDP is lossy and unordered — which is exactly what the session layer
already tolerates: windowed PROGRESS acks, OFFSET_GAP rewind, and
suspend/resume mean a dropped or reordered datagram costs a retransmit,
not the transfer. Each datagram carries exactly one protocol frame.

Host → device: sendto(device). Device → host: the device replies to the
source address of the last datagram it received, so the host just
recvfrom() on the same socket.
"""
from __future__ import annotations

import asyncio
import socket

from ..transport import LinkError, NotifyCallback, Transport

# One frame per datagram. 1024 keeps us well under the typical 1500-byte
# Ethernet / 1280-byte IPv6-minimum MTU, so no IP fragmentation; the
# device advertises its own per-DATA payload in the START response.
MAX_DATAGRAM = 1024


class UdpTransport(Transport):
    """Transport over a connected UDP socket to a device host:port."""

    def __init__(self, sock: socket.socket, pace_s: float):
        self._sock = sock
        self._pace_s = pace_s
        self._cb: NotifyCallback | None = None
        self._loop = asyncio.get_running_loop()
        self._alive = True
        self._reader = asyncio.ensure_future(self._read_loop())

    # NOTE: UDP transfers currently reach ~88% then can wedge if a
    # PROGRESS (device->host) datagram is lost: the host waits on a
    # window it thinks is full while the device waits for the next
    # offset, and the GET_STATE recovery doesn't reliably bridge the
    # deadlock over a lossy link. Pacing reduces loss but doesn't cure
    # it — the fix is more robust lost-PROGRESS recovery (e.g. the host
    # polling GET_STATE harder once a window stalls, or a smaller UDP
    # window). Tracked as a known limitation.
    @classmethod
    async def open(cls, host: str, port: int = 3333,
                   pace_s: float = 0.0008) -> "UdpTransport":
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            sock.connect((host, port))   # fixes the peer; enables send()/recv()
            sock.setblocking(False)
        except OSError as e:
            raise LinkError(f"udp open {host}:{port}: {e}") from e
        return cls(sock, pace_s)

    @property
    def max_write_size(self) -> int:
        return MAX_DATAGRAM

    async def send(self, frame: bytes) -> None:
        if len(frame) > MAX_DATAGRAM:
            raise LinkError(f"frame {len(frame)} > {MAX_DATAGRAM}")
        try:
            await self._loop.sock_sendall(self._sock, frame)
        except OSError as e:
            raise LinkError(f"udp send: {e}") from e
        # UDP has no transport flow control: a burst within the session
        # window can outrun the device's single recv-task + flash-write
        # and overflow its socket buffer (lost datagrams → stall). Pace
        # sends so the device keeps up; the session window still bounds
        # how far ahead we get before a PROGRESS ack.
        if self._pace_s:
            await asyncio.sleep(self._pace_s)

    def set_notify_callback(self, cb: NotifyCallback) -> None:
        self._cb = cb

    async def _read_loop(self) -> None:
        while self._alive:
            try:
                data = await self._loop.sock_recv(self._sock, MAX_DATAGRAM)
            except (OSError, asyncio.CancelledError):
                break
            if data and self._cb is not None:
                self._cb(bytes(data))

    async def close(self) -> None:
        self._alive = False
        self._reader.cancel()
        try:
            self._sock.close()
        except OSError:
            pass
