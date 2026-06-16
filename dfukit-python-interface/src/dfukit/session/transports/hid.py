"""USB-HID transport for dfu_session via Linux hidraw.

Pure-Python (no hidapi): finds the device's /dev/hidrawN node by USB
VID/PID in sysfs, then does fixed 64-byte report I/O. Each report is
framed [len][frame…][zero pad] to carry a variable-length session frame
(mirrors main/usb/usb_hid_dfu.c).

Needs read/write access to the hidraw node (root, or a udev rule). A
reader thread turns incoming IN reports into notify callbacks, matching
the Transport contract used by session.py.
"""
from __future__ import annotations

import asyncio
import glob
import os
import threading

from ..transport import LinkError, NotifyCallback, Transport

REPORT_BYTES = 64
FRAME_MAX = REPORT_BYTES - 1   # 1-byte length prefix


def find_hidraw(vid: int, pid: int) -> str | None:
    """Return /dev/hidrawN for the first node matching vid:pid, or None."""
    for node in sorted(glob.glob("/sys/class/hidraw/hidraw*")):
        # .../hidrawN/device/uevent carries HID_ID=BUS:VVVV:PPPP
        uevent = os.path.join(node, "device", "uevent")
        try:
            with open(uevent) as f:
                txt = f.read()
        except OSError:
            continue
        for line in txt.splitlines():
            if line.startswith("HID_ID="):
                # HID_ID=0003:0000303A:00004002
                parts = line.split("=", 1)[1].split(":")
                if len(parts) == 3 and int(parts[1], 16) == vid \
                        and int(parts[2], 16) == pid:
                    return "/dev/" + os.path.basename(node)
    return None


class HidRawTransport(Transport):
    """Transport over a Linux hidraw node."""

    def __init__(self, fd: int):
        self._fd = fd
        self._cb: NotifyCallback | None = None
        self._loop = asyncio.get_running_loop()
        self._alive = True
        self._reader = threading.Thread(target=self._read_loop, daemon=True)
        self._reader.start()

    @classmethod
    async def open(cls, vid: int = 0x303A, pid: int = 0x4002) -> "HidRawTransport":
        path = find_hidraw(vid, pid)
        if path is None:
            raise LinkError(
                f"no hidraw node for {vid:04x}:{pid:04x} — device not "
                "present, or built without CONFIG_DFU_HID_ENABLE")
        try:
            fd = os.open(path, os.O_RDWR)
        except OSError as e:
            raise LinkError(f"open {path}: {e} (try sudo / a udev rule)") from e
        return cls(fd)

    @property
    def max_write_size(self) -> int:
        return FRAME_MAX

    async def send(self, frame: bytes) -> None:
        if len(frame) > FRAME_MAX:
            raise LinkError(f"frame {len(frame)} > {FRAME_MAX}")
        # hidraw write: first byte is the report number (0 = no numbered
        # reports), then the 64-byte report [len][frame][pad].
        report = bytes([len(frame)]) + frame
        report += b"\x00" * (REPORT_BYTES - len(report))
        buf = b"\x00" + report
        try:
            await self._loop.run_in_executor(None, os.write, self._fd, buf)
        except OSError as e:
            raise LinkError(f"hid write: {e}") from e

    def set_notify_callback(self, cb: NotifyCallback) -> None:
        self._cb = cb

    def _read_loop(self) -> None:
        while self._alive:
            try:
                data = os.read(self._fd, REPORT_BYTES)
            except OSError:
                break
            if not data:
                continue
            flen = data[0]
            if flen == 0 or flen > FRAME_MAX or 1 + flen > len(data):
                continue
            frame = bytes(data[1:1 + flen])
            cb = self._cb
            if cb is not None:
                self._loop.call_soon_threadsafe(cb, frame)

    async def close(self) -> None:
        self._alive = False
        try:
            os.close(self._fd)
        except OSError:
            pass
