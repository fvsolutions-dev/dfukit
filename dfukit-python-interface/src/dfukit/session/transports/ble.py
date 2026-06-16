"""BLE transport for DFU Session Protocol v1.1 (bleak backend).

Design notes
------------
* Early-exit scan: a detection callback + asyncio.Future exits as soon as
  a matching device is sighted rather than waiting the full scan window.
  The scan_seconds argument is the ceiling only (needed for devices on slow
  advertising intervals).

* MTU exchange: bleak's BlueZ backend does NOT auto-exchange.  client.mtu_size
  stays at 23 until _acquire_mtu() is called, which means write-without-
  response is silently capped at 20 B.  We call it eagerly after connect.

* BlueZ AcquireWrite FD path: the D-Bus WriteValue path wedges silently after
  ~1000-1700 WriteCmds on Linux (link stays up, writes stop arriving at the
  device).  BlueZ's answer is AcquireWrite — a kernel socket FD with real
  backpressure.  One os.write = one WriteCmd; the kernel socket drains the
  ATT layer cleanly.  Falls back to WriteValue + periodic write-with-response
  fences when unavailable (non-BlueZ backend, old BlueZ, char without flag).

* "no device found in scan" → LinkError: after a wedged link, BlueZ keeps
  the old connection alive for up to the supervision timeout (~15 s) and the
  device doesn't show up in scans until the link tears down.  The reconnect
  loop in session.py retries; the device holds its suspended session for 60 s.
"""
from __future__ import annotations

import asyncio
import os
from typing import Callable

from bleak import BleakClient, BleakScanner
from bleak.backends.characteristic import BleakGATTCharacteristic

from ..transport import LinkError, NotifyCallback, Transport

# Project-local 16-bit UUIDs (0xEE00 range).
# Keep in sync with CONFIG_DFU_BLE_SVC_UUID16 / CONFIG_DFU_BLE_CHR_UUID16.
_DEFAULT_SVC_UUID16 = 0xEEE1
_DEFAULT_CHR_UUID16 = 0xEEE2


def _uuid16(short: int) -> str:
    return f"0000{short:04x}-0000-1000-8000-00805f9b34fb"


# --------------------------------------------------------------------------
# Scan helper
# --------------------------------------------------------------------------

async def scan_for_device(
    name: str | None = None,
    address: str | None = None,
    svc_uuid16: int = _DEFAULT_SVC_UUID16,
    scan_seconds: float = 15.0,
) -> object:
    """Scan for a BLE device matching name, address, or service UUID.

    Returns the first match as a bleak device object, or raises LinkError
    if nothing is found within scan_seconds (retriable — the device may
    not yet be advertising, or BlueZ may still be tearing down a dead link).
    """
    service_uuid = _uuid16(svc_uuid16)
    found: asyncio.Future = asyncio.get_running_loop().create_future()

    def on_adv(dev, adv) -> None:
        if found.done():
            return
        if address and dev.address.lower() == address.lower():
            found.set_result(dev)
        elif name and ((dev.name or "") == name or (adv.local_name or "") == name):
            found.set_result(dev)
        elif not name and not address and service_uuid in (adv.service_uuids or []):
            found.set_result(dev)

    async with BleakScanner(on_adv):
        try:
            return await asyncio.wait_for(found, timeout=scan_seconds)
        except asyncio.TimeoutError:
            raise LinkError(
                "no device found in scan (may still be tearing down the old "
                "link; will rescan)")


# --------------------------------------------------------------------------
# BlueZ AcquireWrite
# --------------------------------------------------------------------------

async def _acquire_write_fd(
    client: BleakClient,
    chr_obj: BleakGATTCharacteristic,
) -> tuple[int, int] | None:
    """Try BlueZ AcquireWrite.  Returns (fd, mtu) or None when unavailable."""
    try:
        from bleak.backends.bluezdbus import defs
        from dbus_fast import Message

        bus = client._backend._bus
        reply = await bus.call(Message(
            destination=defs.BLUEZ_SERVICE,
            path=chr_obj.obj[0],
            interface=defs.GATT_CHARACTERISTIC_INTERFACE,
            member="AcquireWrite",
            signature="a{sv}",
            body=[{}],
        ))
        if getattr(reply, "error_name", None) or not reply.unix_fds:
            return None
        return reply.unix_fds[0], reply.body[1]
    except Exception:
        return None


# --------------------------------------------------------------------------
# BleTransport
# --------------------------------------------------------------------------

class BleTransport(Transport):
    """bleak-based BLE transport for dfukit.session.Session.

    Use as an async context manager::

        async with BleTransport.connect(dev, chr_uuid16=0xEEE2) as tr:
            session = Session(tr)
            await session.upload(...)
    """

    def __init__(self, client: BleakClient,
                 chr_obj: BleakGATTCharacteristic,
                 write_fd: int | None = None,
                 fd_mtu: int | None = None,
                 fence_every: int = 32) -> None:
        self._client     = client
        self._chr        = chr_obj
        self._write_fd   = write_fd
        self._fd_mtu     = fd_mtu
        self._fence_every = fence_every
        self._write_count = 0
        self._notify_cb: NotifyCallback | None = None
        # Compute max_write_size from ATT MTU.  Bleak's mtu_size is misleading
        # on BlueZ (stays 23 even when the underlying MTU is larger), so prefer
        # fd_mtu when the AcquireWrite path is active.
        mtu = fd_mtu or getattr(client, "mtu_size", 23) or 23
        mtu = max(mtu, 23)
        # ATT NOTIFY/WRITE header is 3 bytes.
        self._max_write_size = mtu - 3

    @classmethod
    async def connect(
        cls,
        device,  # bleak device object from scan_for_device
        chr_uuid16: int = _DEFAULT_CHR_UUID16,
        svc_uuid16: int = _DEFAULT_SVC_UUID16,
        fence_every: int = 32,
        reliable: bool = False,
    ) -> "BleTransport":
        """Connect, negotiate MTU, subscribe to notifications, try AcquireWrite."""
        client = BleakClient(device)
        try:
            await client.connect()
        except Exception as e:
            raise LinkError(f"connect failed: {e}") from e

        # EVERYTHING between connect and a fully-subscribed transport
        # must map to LinkError: the link can die at any point during
        # setup (seen live: NotConnected from start_notify when the
        # device dropped mid-handshake) and the reconnect loop should
        # treat that as one more retriable attempt, not a crash.
        try:
            # MTU exchange (BlueZ doesn't do this automatically).
            if hasattr(client, "_acquire_mtu"):
                try:
                    await client._acquire_mtu()
                except Exception:
                    pass

            # Locate OTA characteristic.
            svc_str = _uuid16(svc_uuid16)
            chr_str = _uuid16(chr_uuid16)
            chr_obj = None
            for svc in client.services:
                if svc.uuid.lower() == svc_str:
                    for c in svc.characteristics:
                        if c.uuid.lower() == chr_str:
                            chr_obj = c
                            break
            if chr_obj is None:
                raise LinkError(
                    f"OTA characteristic 0x{chr_uuid16:04X} not found — "
                    "wrong device or firmware built without CONFIG_DFU_BLE_ENABLE?")

            # Try BlueZ AcquireWrite for high-throughput writes.
            write_fd = fd_mtu = None
            if not reliable:
                result = await _acquire_write_fd(client, chr_obj)
                if result is not None:
                    write_fd, fd_mtu = result
                    fence_every = 0   # kernel socket = no fences needed

            tr = cls(client, chr_obj, write_fd=write_fd, fd_mtu=fd_mtu,
                     fence_every=fence_every)
            # Subscribe to notifications (must precede any upload traffic).
            await client.start_notify(chr_obj, tr._on_notify)
            return tr
        except Exception as e:
            try:
                await client.disconnect()
            except Exception:
                pass
            if isinstance(e, LinkError):
                raise
            raise LinkError(f"connection setup failed: {e}") from e

    def _on_notify(self, _chr: BleakGATTCharacteristic, data: bytearray) -> None:
        if self._notify_cb is not None:
            self._notify_cb(bytes(data))

    # ---- Transport interface ------------------------------------------

    @property
    def max_write_size(self) -> int:
        return self._max_write_size

    def set_notify_callback(self, cb: NotifyCallback) -> None:
        self._notify_cb = cb

    async def send(self, frame: bytes) -> None:
        try:
            if self._write_fd is not None:
                # Kernel socket path: real backpressure, no D-Bus overhead.
                loop = asyncio.get_running_loop()
                await loop.run_in_executor(None, os.write, self._write_fd, frame)
            else:
                # D-Bus WriteValue path.  Periodic write-with-response fences
                # drain the outgoing queue and prevent the ~1000-write wedge.
                self._write_count += 1
                is_fence = (self._fence_every > 0 and
                            self._write_count % self._fence_every == 0)
                await self._client.write_gatt_char(
                    self._chr, frame, response=is_fence)
        except (BrokenPipeError, OSError) as e:
            raise LinkError(f"write failed: {e}") from e
        except Exception as e:
            from bleak.exc import BleakError
            if isinstance(e, BleakError):
                raise LinkError(f"BLE write error: {e}") from e
            raise

    async def close(self) -> None:
        if self._write_fd is not None:
            try:
                os.close(self._write_fd)
            except OSError:
                pass
            self._write_fd = None
        try:
            await self._client.stop_notify(self._chr)
        except Exception:
            pass
        try:
            await self._client.disconnect()
        except Exception:
            pass

    # ---- Context manager -------------------------------------------------

    async def __aenter__(self) -> "BleTransport":
        return self

    async def __aexit__(self, *_) -> None:
        await self.close()
