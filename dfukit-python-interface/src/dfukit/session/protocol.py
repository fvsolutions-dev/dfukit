"""DFU Session Protocol v1.1 — transport-agnostic upload orchestration.

All logic here is lifted from scripts/ble_dfu_client.py and made transport-
agnostic.  The caller provides:

  * a Transport instance (see transport.py)
  * or an async factory Callable[[], Awaitable[Transport]] for the reconnect loop

Progress is reported via an optional callback: progress_cb(bytes_done, total).

Usage (single connection, no reconnect):
    async with BleTransport(...) as tr:
        session = Session(tr)
        await session.upload(image, sha256, hw_id=0x0001)
        await session.apply()

Usage (with reconnect):
    await upload_with_reconnect(transport_factory, image, sha256, hw_id=0x0001)
"""
from __future__ import annotations

import asyncio
import struct
from typing import Awaitable, Callable

from .transport import LinkError, ProtocolError, Transport
from .wire import (
    Op, State, Status,
    build_apply, build_data, build_single_op, build_start,
    parse_get_state_rsp_extra, parse_progress, parse_rsp, parse_start_rsp_extra,
    status_name, state_name,
)

ProgressCallback = Callable[[int, int], None]   # (bytes_done, total)


# --------------------------------------------------------------------------
# Reply routing
# --------------------------------------------------------------------------

class _ReplyRouter:
    """Routes incoming notifications to per-opcode futures + PROGRESS."""

    def __init__(self) -> None:
        self._waiters: dict[int, asyncio.Future] = {}
        self.last_progress: tuple[State, Status, int, int] | None = None
        self._progress_event = asyncio.Event()

    def on_notify(self, data: bytes) -> None:
        if len(data) < 1:
            return
        op = data[0]
        if op == Op.PROGRESS:
            try:
                status, state, acked, crc = parse_progress(data)
                self.last_progress = (state, status, acked, crc)
                self._progress_event.set()
            except ValueError:
                pass
            return
        if op & Op.RSP_FLAG:
            try:
                req_op, status, extra = parse_rsp(data)
            except ValueError:
                return
            fut = self._waiters.pop(req_op, None)
            if fut is not None and not fut.done():
                fut.set_result((status, extra))

    async def request(self, transport: Transport, frame: bytes,
                      expect_op: int, timeout: float) -> tuple[int, bytes]:
        """Send frame, await matching RSP.  Returns (status, extra)."""
        loop = asyncio.get_running_loop()
        fut: asyncio.Future = loop.create_future()
        old = self._waiters.pop(expect_op, None)
        if old is not None and not old.done():
            old.cancel()
        self._waiters[expect_op] = fut
        await transport.send(frame)
        return await asyncio.wait_for(fut, timeout=timeout)

    async def wait_progress(self, timeout: float) -> bool:
        """Wait for any PROGRESS notify.  Returns True if received in time."""
        self._progress_event.clear()
        try:
            await asyncio.wait_for(self._progress_event.wait(), timeout=timeout)
            return True
        except asyncio.TimeoutError:
            return False


# --------------------------------------------------------------------------
# Session
# --------------------------------------------------------------------------

class Session:
    """One upload session over a connected Transport.

    Instantiate fresh for each reconnect attempt (the router clears
    automatically on construction).
    """

    def __init__(self, transport: Transport,
                 progress_cb: ProgressCallback | None = None) -> None:
        self._tr = transport
        self._cb = progress_cb
        self._router = _ReplyRouter()
        transport.set_notify_callback(self._router.on_notify)

    # ---- Recovery probe -----------------------------------------------

    async def get_state(self, retries: int = 4) -> tuple[State, int, int]:
        """Poll GET_STATE.  Returns (state, acked, image_size).

        Retries up to `retries` times with a 3 s per-attempt timeout.
        Both the write and the reply notify ride a lossy link, so the
        recovery path must be more persistent than the thing it recovers
        from (a lost PROGRESS).  Raises LinkError if all attempts time out.
        """
        last_exc: Exception | None = None
        for attempt in range(retries):
            try:
                frame = build_single_op(Op.GET_STATE)
                status, extra = await self._router.request(
                    self._tr, frame, int(Op.GET_STATE), timeout=3.0)
                if status != Status.OK or len(extra) < 9:
                    raise ProtocolError(
                        f"GET_STATE failed: {status_name(status)}")
                state, acked, image_size = parse_get_state_rsp_extra(extra)
                return state, acked, image_size
            except (asyncio.TimeoutError, TimeoutError) as e:
                last_exc = e
        raise LinkError(
            f"GET_STATE timed out after {retries} attempts") from last_exc

    # ---- Upload orchestration -----------------------------------------

    async def upload(self, image: bytes, sha256: bytes,
                     hw_id: int = 0x0001,
                     fw_ver: int = 0,
                     min_fw: int = 0,
                     chunk_minus: int = 9,
                     apply_delay_ms: int = 500) -> None:
        """Upload image and leave device in READY state.

        chunk_minus: subtract from device-advertised max_payload.  Default 9
        leaves a single LL PDU (243 - 5 DATA hdr - 9 = 229 B) which is
        measured fastest on BlueZ because it avoids LL fragmentation.

        Raises ProtocolError on device-side rejection (wrong hw_id, etc.).
        Raises LinkError on transport failure (let the caller reconnect).
        """
        size = len(image)

        # Recovery probe before START: a previous leg may have left the image
        # fully staged (READY) or still finalizing (FINISHING).  A fresh
        # device answers IDLE and we fall through to the normal START.
        try:
            st0, _ack0, img0 = await self.get_state(retries=2)
        except (LinkError, asyncio.TimeoutError, TimeoutError):
            st0, img0 = None, None  # type: ignore[assignment]

        if img0 == size and st0 == State.FINISHING:
            # Finalisation is in progress — wait up to 10 s for READY.
            for _ in range(10):
                await asyncio.sleep(1.0)
                try:
                    st0, _ack0, img0 = await self.get_state(retries=2)
                except (LinkError, asyncio.TimeoutError, TimeoutError):
                    continue
                if st0 != State.FINISHING:
                    break

        if img0 == size and st0 == State.READY:
            # Image already staged — skip transfer.
            if self._cb:
                self._cb(size, size)
            return

        # START
        start_frame = build_start(size, sha256, fw_ver, hw_id, min_fw)
        status, extra = await self._router.request(
            self._tr, start_frame, int(Op.START), timeout=10.0)
        if status != Status.OK:
            raise ProtocolError(f"START rejected: {status_name(status)}")
        resume, max_payload, window = parse_start_rsp_extra(extra)

        chunk = max(1, max_payload - chunk_minus)
        outstanding: int = 0
        offset: int = resume

        if self._cb and resume:
            self._cb(resume, size)

        while offset < size:
            # Window flow control: wait for a PROGRESS if we'd exceed window.
            if outstanding + chunk > window:
                saw = await self._router.wait_progress(timeout=5.0)
                if not saw:
                    # Lost PROGRESS path — poll authoritative state.
                    try:
                        _, acked_r, _ = await self.get_state()
                    except (asyncio.TimeoutError, TimeoutError, LinkError):
                        raise LinkError(
                            "stalled: no PROGRESS or GET_STATE reply in 15 s")
                    outstanding = offset - acked_r
                else:
                    prog = self._router.last_progress
                    if prog is not None:
                        _state, _st, acked_p, _ = prog
                        outstanding = offset - acked_p
                continue

            take = min(chunk, size - offset)
            frame = build_data(offset, image[offset:offset + take])
            await self._tr.send(frame)
            offset += take
            outstanding += take

            # Drain any queued PROGRESS so outstanding tracks reality.
            while self._router.last_progress is not None:
                state_p, status_p, acked_p, _ = self._router.last_progress
                self._router.last_progress = None
                if status_p == Status.OFFSET_GAP:
                    # Device rewinds; mirror.
                    offset = acked_p
                    outstanding = 0
                    break
                if status_p != Status.OK:
                    raise ProtocolError(
                        f"transfer aborted: {status_name(status_p)}"
                        f" (state={state_name(state_p)})")
                outstanding = offset - acked_p

            if self._cb:
                self._cb(offset, size)

        # FINISH
        status, extra = await self._router.request(
            self._tr, build_single_op(Op.FINISH), int(Op.FINISH), timeout=30.0)
        if status != Status.OK:
            raise ProtocolError(f"FINISH failed: {status_name(status)}")
        if self._cb:
            self._cb(size, size)

    async def apply(self, delay_ms: int = 500) -> None:
        """Send APPLY — device will reboot after delay_ms."""
        status, _extra = await self._router.request(
            self._tr, build_apply(delay_ms), int(Op.APPLY), timeout=5.0)
        if status != Status.OK:
            raise ProtocolError(f"APPLY failed: {status_name(status)}")

    async def confirm(self) -> None:
        """Commit the running slot (post-flash boot, after verifying the new fw)."""
        status, _extra = await self._router.request(
            self._tr, build_single_op(Op.CONFIRM), int(Op.CONFIRM), timeout=5.0)
        if status != Status.OK:
            raise ProtocolError(f"CONFIRM failed: {status_name(status)}")


# --------------------------------------------------------------------------
# Reconnect loop (outer orchestration)
# --------------------------------------------------------------------------

async def upload_with_reconnect(
    transport_factory: Callable[[], Awaitable[Transport]],
    image: bytes,
    sha256: bytes,
    hw_id: int = 0x0001,
    fw_ver: int = 0,
    min_fw: int = 0,
    do_apply: bool = True,
    apply_delay_ms: int = 500,
    chunk_minus: int = 9,
    max_reconnects: int = 8,
    reconnect_pause_s: float = 2.0,
    progress_cb: ProgressCallback | None = None,
) -> None:
    """Full upload with reconnect-and-resume loop.

    On LinkError (transport-level failure) the loop recreates the transport
    via transport_factory and resumes from the device's last flush boundary.
    The device holds a suspended session for CONFIG_DFU_SESSION_RESUME_GRACE_MS
    (default 60 s) after a link drop, so any link failure costs one reconnect
    rather than a full re-transfer.

    Raises ProtocolError immediately (fatal).
    Raises the last LinkError if max_reconnects is exhausted.
    """
    attempt = 0
    last_link_err: Exception | None = None
    while True:
        transport = await transport_factory()
        try:
            session = Session(transport, progress_cb=progress_cb)
            await session.upload(image, sha256, hw_id=hw_id, fw_ver=fw_ver,
                                 min_fw=min_fw, chunk_minus=chunk_minus,
                                 apply_delay_ms=apply_delay_ms)
            if do_apply:
                await session.apply(delay_ms=apply_delay_ms)
            return
        except ProtocolError:
            raise
        except (LinkError, OSError, asyncio.TimeoutError, TimeoutError) as e:
            last_link_err = e
            attempt += 1
            if attempt > max_reconnects:
                raise LinkError(
                    f"giving up after {max_reconnects} reconnects: {e}") from e
        finally:
            try:
                await transport.close()
            except Exception:
                pass

        await asyncio.sleep(reconnect_pause_s)
