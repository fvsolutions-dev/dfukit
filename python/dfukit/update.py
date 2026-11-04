"""An update through a transport, sent by the C client."""

import contextlib
import queue
import struct
import time
import zlib
from collections.abc import Callable
from dataclasses import dataclass, field

from _dfukit import Client, ClientState, Status, status_name

from dfukit.transport import LinkError, Transport

RAW, DEFLATE = 0, 1
KIND_IMAGE, KIND_PASSTHROUGH = 0, 1
ACTIVATE_AT_NEXT_BOOT = 0x01

# The largest frame an update sends, on a link that carries more
MAX_FRAME = 1024
# How long reconnecting goes on without progress: a device keeps a session this long by default,
# 10 s until it is suspended and 60 s after that
GIVE_UP_S = 70.0

_OP_INFO, _ANSWER = 0x01, 0x80
_TLV_NAME, _TLV_VERSION = 1, 2


class UpdateError(Exception):
    def __init__(self, status: int) -> None:
        self.status = status
        super().__init__(status_name(int(status)))


@dataclass
class Board:
    board_id: int
    kind: int
    max_size: int
    encodings: int
    window_log2: int
    name: str = ""
    # The running image's version, where the device gives it
    version: str = ""

    def takes(self, encoding: int) -> bool:
        return bool(self.encodings & (1 << encoding))


@dataclass
class Info:
    version: int
    name: str
    boards: list[Board] = field(default_factory=list)

    def board(self, board_id: int) -> Board | None:
        return next((board for board in self.boards if board.board_id == board_id), None)


class ActivationNotConfirmed(UpdateError):
    """The device has the whole image and was told to activate it, but the answer never came. It
    may well be running the new image: an update is not sent again, which would flash it a second
    time."""

    def __str__(self) -> str:
        return f"the image was sent, but its activation is not confirmed ({super().__str__()})"


def parse_info(frame: bytes) -> Info:
    """INFO's answer; ValueError when it is not one, or cut short."""
    try:
        return _parse_info(frame)
    except (IndexError, struct.error) as error:
        raise ValueError(f"a malformed INFO answer: {error}") from error


def _parse_info(frame: bytes) -> Info:
    if len(frame) < 4 or frame[0] != _ANSWER | _OP_INFO:
        raise ValueError("not an INFO answer")
    name_len = frame[2]
    info = Info(frame[1], frame[3 : 3 + name_len].decode(errors="replace"))
    at = 3 + name_len
    count = frame[at]
    at += 1
    for _ in range(count):
        board_id, kind, max_size, encodings, window_log2, tlv_len = struct.unpack_from(
            "<IBIBBH", frame, at
        )
        at += 13
        board = Board(board_id, kind, max_size, encodings, window_log2)
        tlv = frame[at : at + tlv_len]
        at += tlv_len
        while len(tlv) >= 2:
            tag, length = tlv[0], tlv[1]
            if tag == _TLV_NAME:
                board.name = tlv[2 : 2 + length].decode(errors="replace")
            elif tag == _TLV_VERSION:
                board.version = tlv[2 : 2 + length].decode(errors="replace")
            tlv = tlv[2 + length :]
        info.boards.append(board)
    return info


def info(transport: Transport, timeout: float = 1.0, tries: int = 3) -> Info:
    """The device's INFO: its boards, what each takes."""
    answers: queue.Queue[bytes] = queue.Queue()
    transport.on_frame = answers.put
    for _ in range(tries):
        transport.send(bytes([_OP_INFO]))
        end = time.monotonic() + timeout
        while (left := end - time.monotonic()) > 0:
            try:
                frame = answers.get(timeout=left)
            except queue.Empty:
                break
            if frame and frame[0] == _ANSWER | _OP_INFO:
                return parse_info(frame)
    raise UpdateError(Status.TIMEOUT)


def deflate(image: bytes, window_log2: int = 15) -> bytes:
    """Raw deflate (no header) with the device's window, 2^9 to 2^15 (zlib's range)."""
    compressor = zlib.compressobj(9, zlib.DEFLATED, -max(9, min(15, window_log2)))
    return compressor.compress(image) + compressor.flush()


def _now() -> int:
    return int(time.monotonic() * 1000) & 0xFFFFFFFF


class Update:
    """One image to one board ID. run() sends it and returns when the device has it (and has
    activated it, with activate), or raises UpdateError."""

    def __init__(
        self,
        transport: Transport,
        image: bytes,
        board_id: int,
        *,
        encoding: int = RAW,
        window_log2: int = 15,
        activate: bool = True,
        activate_flags: int = 0,
        activate_delay_ms: int = 0,
        rto_ms: int = 0,
        tries: int = 0,
    ) -> None:
        self.transport = transport
        self.image = image
        self.board_id = board_id
        self.encoding = encoding
        self.stream = deflate(image, window_log2) if encoding == DEFLATE else image
        self.activate = activate
        self.activate_flags = activate_flags
        self.activate_delay_ms = activate_delay_ms
        self._frames: queue.Queue[bytes] = queue.Queue()
        self._link_error: LinkError | None = None
        self._client = Client(
            self._send,
            self._read,
            max_frame=min(transport.max_frame, MAX_FRAME),
            rto_ms=rto_ms,
            tries=tries,
        )

    @property
    def acked(self) -> int:
        """The stream's bytes the device has taken."""
        return int(self._client.acked)

    @property
    def resent(self) -> int:
        return int(self._client.resent)

    def run(
        self,
        progress: Callable[[int, int], None] | None = None,
        reconnect: Callable[[], Transport] | None = None,
    ) -> None:
        """reconnect: called when the link is lost, or the device stopped answering; it returns
        an open transport, on which the update resumes. It is given up on after GIVE_UP_S without
        progress. Without it, a lost link fails the update. Once ACTIVATE is sent the link is not
        reconnected: a device that restarted would start over from nothing, and flash the image
        again. ActivationNotConfirmed says so instead."""
        self.transport.on_frame = self._frames.put
        self._client.begin(
            self.board_id,
            self.image,
            _now(),
            stream_size=len(self.stream),
            encoding=self.encoding,
            activate=self.activate,
            activate_flags=self.activate_flags,
            activate_delay_ms=self.activate_delay_ms,
        )
        last_acked = -1
        moved = time.monotonic()
        activating = False
        while True:
            with contextlib.suppress(queue.Empty):
                self._client.feed(self._frames.get(timeout=0.002), _now())
            self._client.poll(_now())
            state = self._client.state
            if self.acked != last_acked:
                last_acked = self.acked
                moved = time.monotonic()
                if progress is not None:
                    progress(last_acked, len(self.stream))
            if state == ClientState.DONE:
                return
            activating = activating or state == ClientState.ACTIVATING
            lost = self._link_error is not None or (
                state == ClientState.FAILED and self._client.status == Status.TIMEOUT
            )
            if lost and activating:
                raise ActivationNotConfirmed(Status.TIMEOUT)
            if lost and reconnect is not None and time.monotonic() - moved < GIVE_UP_S:
                self._link_error = None
                self.transport = reconnect()
                self.transport.on_frame = self._frames.put
                self._client.resume(_now())
                continue
            if self._link_error is not None:
                raise self._link_error
            if state == ClientState.FAILED:
                raise UpdateError(self._client.status)

    def _send(self, frame: bytes) -> int:
        try:
            self.transport.send(frame)
        except LinkError as error:
            self._link_error = error
            return -1
        return 0

    def _read(self, offset: int, length: int) -> bytes | None:
        if offset + length > len(self.stream):
            return None
        return self.stream[offset : offset + length]
