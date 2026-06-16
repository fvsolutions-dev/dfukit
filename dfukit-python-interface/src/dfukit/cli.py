"""dfukit — one-stop firmware-update CLI.

Transport subcommands sit under one umbrella:
  - ble       OTA over BLE GATT (DFU Session Protocol v1.1; delta-capable)
  - msc       UF2 drag-drop: copy an app-only UF2 to a mounted MSC drive
  - scan      list BLE devices in range
Planned: dfu-util (USB DFU class), uart (ROM bootloader, incl. serial-
over-IP / RFC 2217), hid (DFU Session over USB-HID).

The BLE/UART-app/UDP transports share the dfu_session protocol layer;
msc / dfu-util / uart-ROM are standalone mechanisms with no shared wire
protocol — see Notes.md for the architecture and roadmap.
"""
from __future__ import annotations

import asyncio
import hashlib
import shutil
import sys
import time
from pathlib import Path

import click
from tqdm import tqdm

from .session import LinkError, ProtocolError, delta, upload_with_reconnect
from .session.transports.ble import BleTransport, scan_for_device


@click.group(context_settings={"help_option_names": ["-h", "--help"]})
def main() -> None:
    """dfukit — firmware updates over BLE, HID, UDP, MSC, USB-DFU, UART."""


# --------------------------------------------------------------------------
# Shared delta helpers (used by every session transport: ble, hid, udp)
# --------------------------------------------------------------------------

def _delta_payloads(target: bytes, target_sha_hex: str,
                    device_id: str | None, use_delta: bool) -> list[bytes]:
    """Return the payload(s) to upload, in preference order.

    With --delta and a cached base for `device_id`, returns
    [patch, target] — the caller tries the patch first and falls back to
    the full image if the device rejects the base. Otherwise [target]."""
    if not use_delta or device_id is None:
        return [target]
    base = delta.lookup_running(device_id)
    if base is None:
        click.secho(f"no cached base for {device_id} — full upload "
                    "(this run seeds the cache)", fg="yellow", err=True)
        return [target]
    if hashlib.sha256(base).hexdigest() == target_sha_hex:
        click.secho("device already runs this image — sending full anyway",
                    fg="yellow", err=True)
        return [target]
    patch = delta.make_patch(base, target)
    click.secho(
        f"delta: {len(patch):,} B patch "
        f"({len(patch) * 100 / len(target):.1f}% of the {len(target):,} B image)",
        fg="green", err=True)
    return [patch, target]


def _delta_record(device_id: str | None, target: bytes,
                  target_sha_hex: str) -> None:
    """Cache `target` and record it as what `device_id` now runs."""
    delta.cache_add(target)
    if device_id:
        delta.record_running(device_id, target_sha_hex)


# --------------------------------------------------------------------------
# scan
# --------------------------------------------------------------------------

@main.command()
@click.option("--seconds", default=6.0, show_default=True,
              help="Scan duration (s).")
@click.option("--svc-uuid", default="0xEEE1", show_default=True,
              help="OTA service UUID (16-bit hex) to mark in output. "
                   "Example default — override for your product.")
def scan(seconds: float, svc_uuid: str) -> None:
    """List BLE devices in range, marking those advertising the OTA service."""
    from bleak import BleakScanner

    svc_id = int(svc_uuid, 0)
    svc_full = f"0000{svc_id:04x}-0000-1000-8000-00805f9b34fb"

    async def _scan() -> None:
        click.secho(f"Scanning {seconds:.0f} s …", fg="cyan", err=True)
        devices = await BleakScanner.discover(timeout=seconds, return_adv=True)
        if not devices:
            click.secho("(no devices found)", fg="yellow", err=True)
            return
        for dev, adv in devices.values():
            ota = svc_full in (adv.service_uuids or [])
            tag = click.style("OTA", fg="green") if ota else "   "
            name = adv.local_name or dev.name or "?"
            click.echo(f"  [{tag}] {dev.address}  rssi={adv.rssi:>4}  {name}")

    asyncio.run(_scan())


# --------------------------------------------------------------------------
# ble — OTA over BLE GATT (DFU Session Protocol v1.1)
# --------------------------------------------------------------------------

@main.command("ble")
@click.argument("image", type=click.Path(exists=True, dir_okay=False,
                                          path_type=Path))
@click.option("--name",    default=None,
              help="Match device by advertising name.")
@click.option("--address", default=None,
              help="Match device by BLE address.")
@click.option("--hw-id",   default="0x0001", show_default=True,
              help="Hardware ID matched against the START manifest. "
                   "Example default — override for your product.")
@click.option("--fwver",   default=0, show_default=True, type=int,
              help="Firmware version field sent in START.")
@click.option("--minfw",   default=0, show_default=True, type=int,
              help="Minimum-firmware gate sent in START.")
@click.option("--apply/--no-apply", "do_apply", default=True, show_default=True,
              help="After FINISH, send APPLY to reboot into the new image.")
@click.option("--apply-delay-ms", default=500, show_default=True, type=int,
              help="Pre-reboot delay (ms) requested in APPLY.")
@click.option("--chunk-minus", default=9, show_default=True, type=int,
              help="Subtract from device-advertised max_payload.  Default 9 "
                   "gives single-LL-PDU frames (measured fastest on BlueZ).")
@click.option("--scan-seconds", default=15.0, show_default=True, type=float,
              help="Scan ceiling (s) when --address isn't given.")
@click.option("--fence-every", default=32, show_default=True, type=int,
              help="Every Nth DATA frame is sent write-with-response to drain "
                   "BlueZ's outgoing queue (prevents the ~1000-write wedge). "
                   "0 disables; ignored when AcquireWrite socket is active.")
@click.option("--reliable/--no-reliable", default=False, show_default=True,
              help="Use GATT write-with-response for all DATA frames.  Slow "
                   "but survives extreme MTU issues.  Disables AcquireWrite.")
@click.option("--svc-uuid", default="0xEEE1", show_default=True,
              help="OTA service UUID (16-bit hex). "
                   "Example default — override for your product.")
@click.option("--chr-uuid", default="0xEEE2", show_default=True,
              help="OTA characteristic UUID (16-bit hex). "
                   "Example default — override for your product.")
@click.option("--delta/--no-delta", "use_delta", default=False, show_default=True,
              help="Send a delta patch against the image this device is "
                   "recorded as running (fw-cache). The device verifies the "
                   "base on its side; on any mismatch this falls back to a "
                   "full upload automatically.")
def upload(
    image: Path,
    name: str | None,
    address: str | None,
    hw_id: str,
    fwver: int,
    minfw: int,
    do_apply: bool,
    apply_delay_ms: int,
    chunk_minus: int,
    scan_seconds: float,
    fence_every: int,
    reliable: bool,
    svc_uuid: str,
    chr_uuid: str,
    use_delta: bool,
) -> None:
    """Upload an OTA image to a device speaking the v1.1 protocol."""
    try:
        hw = int(hw_id, 0)
        svc_id = int(svc_uuid, 0)
        chr_id = int(chr_uuid, 0)
    except ValueError as e:
        raise click.ClickException(str(e))

    target = image.read_bytes()
    if not target:
        raise click.ClickException(f"empty file: {image}")
    target_sha_hex = hashlib.sha256(target).hexdigest()

    # Delta is keyed by the device's BLE address; resolve it with one
    # early-exit scan when only a name was given (the upload path
    # re-scans per attempt as usual).
    if use_delta and address is None:
        dev = asyncio.run(scan_for_device(
            name=name, address=None,
            svc_uuid16=svc_id, scan_seconds=scan_seconds))
        address = dev.address

    payloads = _delta_payloads(target, target_sha_hex, address, use_delta)

    click.secho(
        f"image: {image} ({len(payloads[0]):,} B to send), "
        f"target sha256 {target_sha_hex[:16]}…", fg="cyan", err=True)

    pbar: tqdm | None = None
    connected_address: str | None = address

    def on_progress(done: int, total: int) -> None:
        nonlocal pbar
        if pbar is None:
            pbar = tqdm(total=total, unit="B", unit_scale=True,
                        desc="upload", file=sys.stderr)
        pbar.update(done - pbar.n)

    async def transport_factory() -> BleTransport:
        nonlocal connected_address
        dev = await scan_for_device(
            name=name, address=address,
            svc_uuid16=svc_id, scan_seconds=scan_seconds,
        )
        connected_address = dev.address
        click.secho(
            f"connecting to {dev.address} ({dev.name or '?'})",
            fg="cyan", err=True,
        )
        return await BleTransport.connect(
            dev,
            chr_uuid16=chr_id,
            svc_uuid16=svc_id,
            fence_every=fence_every,
            reliable=reliable,
        )

    # When sending a delta, a ProtocolError can mean the device
    # rejected the base (its on-device digest check) — fall back to
    # the full image (last entry of payloads) instead of failing.
    t0 = time.monotonic()
    sent_size = len(payloads[0])
    for attempt_idx, payload in enumerate(payloads):
        payload_sha = hashlib.sha256(payload).digest()
        sent_size = len(payload)
        try:
            asyncio.run(upload_with_reconnect(
                transport_factory,
                payload, payload_sha,
                hw_id=hw,
                fw_ver=fwver,
                min_fw=minfw,
                do_apply=do_apply,
                apply_delay_ms=apply_delay_ms,
                chunk_minus=chunk_minus,
                progress_cb=on_progress,
            ))
            break
        except ProtocolError as e:
            if pbar:
                pbar.close()
                pbar = None
            if attempt_idx + 1 < len(payloads):
                click.secho(
                    f"delta rejected by device ({e}) — falling back to "
                    "full upload", fg="yellow", err=True)
                continue
            raise click.ClickException(str(e))
        except LinkError as e:
            if pbar:
                pbar.close()
            raise click.ClickException(str(e))
        except KeyboardInterrupt:
            if pbar:
                pbar.close()
            click.secho("\ninterrupted", fg="red", err=True)
            sys.exit(2)
        finally:
            if pbar and not pbar.disable:
                pbar.close()
                pbar = None

    # Bookkeeping for future deltas: the device now runs `target`.
    _delta_record(connected_address, target, target_sha_hex)

    elapsed = time.monotonic() - t0
    click.secho(
        f"done: {sent_size/1024:.1f} KB sent in {elapsed:.1f} s "
        f"({sent_size/1024/max(elapsed, 0.001):.1f} KB/s wall)",
        fg="green", err=True,
    )
    if not do_apply:
        click.secho(
            "skipping APPLY — use --apply to reboot into the staged image",
            fg="yellow", err=True,
        )


# --------------------------------------------------------------------------
# msc — UF2 drag-drop to a mounted MSC drive
# --------------------------------------------------------------------------

@main.command("msc")
@click.argument("image", type=click.Path(exists=True, dir_okay=False,
                                          path_type=Path))
@click.option("--mount", "mount", required=True,
              type=click.Path(exists=True, file_okay=False, path_type=Path),
              help="Path where the device's MSC drive is mounted.")
@click.option("--name", default="firmware.uf2", show_default=True,
              help="Destination filename on the drive.")
def msc(image: Path, mount: Path, name: str) -> None:
    """Copy an app-only UF2 to a mounted MSC drive (driverless drag-drop).

    IMPORTANT: pass an APP-ONLY UF2 (build.py uf2-app / `dfukit` won't
    build it for you). A combined uf2.bin is multi-segment and, while the
    device now tolerates it, app-only is the correct OTA payload. The copy
    is flushed (fsync) before returning; the device reboots into the new
    image once every block has landed."""
    data = image.read_bytes()
    # Cheap guard against handing over a raw .bin or a wrong file: a UF2's
    # first block starts with the "UF2\n" magic.
    if data[:4] != b"UF2\n":
        raise click.ClickException(
            f"{image} is not a UF2 (no UF2\\n magic) — build one with "
            "`./build.py uf2-app`")

    dst = mount / name
    click.secho(f"copying {image.name} ({len(data):,} B) → {dst}",
                fg="cyan", err=True)
    try:
        with open(dst, "wb") as f:
            f.write(data)
            f.flush()
            import os
            os.fsync(f.fileno())
    except OSError as e:
        raise click.ClickException(
            f"copy failed: {e}. If it's 'No space left', the host cached a "
            "stale FAT — re-mount the drive (the device serves a fresh empty "
            "volume each enumeration).")
    # shutil import kept for parity with future verify/compare paths.
    _ = shutil
    click.secho("copied + flushed; device will ingest and reboot when the "
                "full image has arrived.", fg="green", err=True)


# --------------------------------------------------------------------------
# dfu-util — USB DFU 1.1 class (standalone mechanism)
# --------------------------------------------------------------------------

@main.command("dfu-util")
@click.argument("image", type=click.Path(exists=True, dir_okay=False,
                                          path_type=Path))
@click.option("--vid-pid", default="303a:4002", show_default=True,
              help="USB vendor:product of the DFU device. "
                   "303a is Espressif's VID; override the PID for your product.")
@click.option("--alt", default=0, show_default=True, type=int,
              help="DFU alt-setting (interface) number.")
@click.option("--reset/--no-reset", default=True, show_default=True,
              help="Reset the device after download (-R).")
@click.option("--sudo/--no-sudo", default=False, show_default=True,
              help="Run dfu-util under sudo (or install the udev rule).")
def dfu_util(image: Path, vid_pid: str, alt: int, reset: bool, sudo: bool) -> None:
    """Flash the raw OTA app image over the USB DFU 1.1 class via dfu-util."""
    from . import usbdfu
    data = image.read_bytes()
    click.secho(f"image: {image} ({len(data):,} B) → dfu {vid_pid} alt {alt}",
                fg="cyan", err=True)
    try:
        usbdfu.upload(data, vid_pid=vid_pid, alt=alt, reset=reset, sudo=sudo,
                      on_line=lambda ln: click.echo(ln, err=True))
    except RuntimeError as e:
        raise click.ClickException(str(e))
    click.secho("done.", fg="green", err=True)


# --------------------------------------------------------------------------
# uart — ESP ROM serial bootloader (esptool), incl. serial-over-IP
# --------------------------------------------------------------------------

@main.command("uart")
@click.argument("image", type=click.Path(exists=True, dir_okay=False,
                                          path_type=Path))
@click.option("--port", required=True,
              help="Serial port (e.g. /dev/ttyUSB0) or RFC 2217 URL "
                   "(rfc2217://host:4000). The device must be in ROM "
                   "download mode (esptool straps it via DTR/RTS on a real "
                   "serial port; remote bridges may need manual entry).")
@click.option("--baud", default=460800, show_default=True, type=int,
              help="Flash baud rate.")
@click.option("--offset", default=None,
              help="Flash offset. Default: the app slot from the build's "
                   "flasher_args.json if given via --flasher-args, else 0x10000.")
@click.option("--chip", default="esp32s3", show_default=True,
              help="Target chip.")
@click.option("--no-stub/--stub", default=False, show_default=True,
              help="Disable the download stub (RFC 2217 bridges that mangle "
                   "the stub upload sometimes need --no-stub).")
def uart(image: Path, port: str, baud: int, offset: str | None,
         chip: str, no_stub: bool) -> None:
    """Flash over the ESP ROM serial bootloader (esptool as a library).

    This is the ROM download protocol, NOT the DFU Session protocol — it
    writes the raw app image to flash. Works over a local serial port or
    a serial-over-IP (RFC 2217) bridge; see Notes.md for the RFC 2217
    handshake caveats (control-line round-trips are the fragile part)."""
    from . import rom as uartrom
    off = int(offset, 0) if offset else uartrom.DEFAULT_APP_OFFSET
    click.secho(
        f"image: {image} → {chip} @ {port} (offset {off:#x}, {baud} baud"
        f"{', no-stub' if no_stub else ''})", fg="cyan", err=True)
    try:
        uartrom.write_flash(str(image), port=port, baud=baud, offset=off,
                            chip=chip, stub=not no_stub)
    except RuntimeError as e:
        raise click.ClickException(str(e))
    click.secho("done.", fg="green", err=True)


# --------------------------------------------------------------------------
# hid — DFU Session over USB-HID (driverless, via Linux hidraw)
# --------------------------------------------------------------------------

@main.command("hid")
@click.argument("image", type=click.Path(exists=True, dir_okay=False,
                                          path_type=Path))
@click.option("--vid", default="0x303a", show_default=True,
              help="USB VID. 0x303a is Espressif's VID; override for your product.")
@click.option("--pid", default="0x4002", show_default=True,
              help="USB PID. Example default — override for your product.")
@click.option("--hw-id", default="0x0001", show_default=True,
              help="Hardware ID matched against the START manifest. "
                   "Example default — override for your product.")
@click.option("--fwver", default=0, show_default=True, type=int)
@click.option("--minfw", default=0, show_default=True, type=int)
@click.option("--apply/--no-apply", "do_apply", default=True, show_default=True)
@click.option("--apply-delay-ms", default=500, show_default=True, type=int)
@click.option("--delta/--no-delta", "use_delta", default=False, show_default=True,
              help="Send a delta patch against the device's recorded image "
                   "(keyed by VID:PID); device verifies the base and the "
                   "client falls back to a full upload on mismatch. Especially "
                   "worthwhile on HID, where bytes are dearer than on BLE.")
def hid(image: Path, vid: str, pid: str, hw_id: str, fwver: int, minfw: int,
        do_apply: bool, apply_delay_ms: int, use_delta: bool) -> None:
    """OTA over the vendor USB-HID interface (no driver needed).

    Same DFU Session protocol as `ble`, carried in 64-byte HID reports.
    Needs hidraw access (root or a udev rule). Device must be built with
    CONFIG_DFU_HID_ENABLE."""
    from .session.transports.hid import HidRawTransport

    vid_i, pid_i, hw = int(vid, 0), int(pid, 0), int(hw_id, 0)
    target = image.read_bytes()
    if not target:
        raise click.ClickException(f"empty file: {image}")
    target_sha_hex = hashlib.sha256(target).hexdigest()
    device_id = f"hid:{vid_i:04x}:{pid_i:04x}"

    payloads = _delta_payloads(target, target_sha_hex, device_id, use_delta)
    click.secho(f"image: {image} ({len(payloads[0]):,} B to send) → "
                f"HID {vid_i:04x}:{pid_i:04x}", fg="cyan", err=True)

    pbar: tqdm | None = None

    def on_progress(done: int, total: int) -> None:
        nonlocal pbar
        if pbar is None:
            pbar = tqdm(total=total, unit="B", unit_scale=True,
                        desc="hid", file=sys.stderr)
        pbar.update(done - pbar.n)

    async def transport_factory():
        return await HidRawTransport.open(vid=vid_i, pid=pid_i)

    t0 = time.monotonic()
    sent_size = len(payloads[0])
    for idx, payload in enumerate(payloads):
        sent_size = len(payload)
        try:
            asyncio.run(upload_with_reconnect(
                transport_factory, payload, hashlib.sha256(payload).digest(),
                hw_id=hw, fw_ver=fwver, min_fw=minfw,
                do_apply=do_apply, apply_delay_ms=apply_delay_ms,
                progress_cb=on_progress))
            break
        except ProtocolError as e:
            if pbar:
                pbar.close(); pbar = None
            if idx + 1 < len(payloads):
                click.secho(f"delta rejected ({e}) — falling back to full",
                            fg="yellow", err=True)
                continue
            raise click.ClickException(str(e))
        except LinkError as e:
            if pbar:
                pbar.close()
            raise click.ClickException(str(e))
        finally:
            if pbar and not pbar.disable:
                pbar.close(); pbar = None

    _delta_record(device_id, target, target_sha_hex)
    elapsed = time.monotonic() - t0
    click.secho(f"done: {sent_size/1024:.1f} KB sent in {elapsed:.1f} s",
                fg="green", err=True)


# --------------------------------------------------------------------------
# udp — DFU Session over UDP/IP
# --------------------------------------------------------------------------

@main.command("udp")
@click.argument("image", type=click.Path(exists=True, dir_okay=False,
                                          path_type=Path))
@click.option("--host", required=True, help="Device IP address.")
@click.option("--port", default=3333, show_default=True, type=int)
@click.option("--hw-id", default="0x0001", show_default=True,
              help="Hardware ID matched against the START manifest. "
                   "Example default — override for your product.")
@click.option("--fwver", default=0, show_default=True, type=int)
@click.option("--minfw", default=0, show_default=True, type=int)
@click.option("--apply/--no-apply", "do_apply", default=True, show_default=True)
@click.option("--apply-delay-ms", default=500, show_default=True, type=int)
@click.option("--delta/--no-delta", "use_delta", default=False, show_default=True,
              help="Send a delta patch (keyed by host:port); device verifies "
                   "the base and falls back to full on mismatch.")
def udp(image: Path, host: str, port: int, hw_id: str, fwver: int, minfw: int,
        do_apply: bool, apply_delay_ms: int, use_delta: bool) -> None:
    """OTA over UDP/IP. The session protocol's windowed acks + resume make
    it robust to UDP's loss/reordering. Device needs an IP (Wi-Fi)."""
    from .session.transports.udp import UdpTransport

    hw = int(hw_id, 0)
    target = image.read_bytes()
    if not target:
        raise click.ClickException(f"empty file: {image}")
    target_sha_hex = hashlib.sha256(target).hexdigest()
    device_id = f"udp:{host}:{port}"

    payloads = _delta_payloads(target, target_sha_hex, device_id, use_delta)
    click.secho(f"image: {image} ({len(payloads[0]):,} B to send) → "
                f"udp {host}:{port}", fg="cyan", err=True)

    pbar: tqdm | None = None

    def on_progress(done: int, total: int) -> None:
        nonlocal pbar
        if pbar is None:
            pbar = tqdm(total=total, unit="B", unit_scale=True,
                        desc="udp", file=sys.stderr)
        pbar.update(done - pbar.n)

    async def transport_factory():
        return await UdpTransport.open(host, port)

    t0 = time.monotonic()
    sent_size = len(payloads[0])
    for idx, payload in enumerate(payloads):
        sent_size = len(payload)
        try:
            asyncio.run(upload_with_reconnect(
                transport_factory, payload, hashlib.sha256(payload).digest(),
                hw_id=hw, fw_ver=fwver, min_fw=minfw,
                do_apply=do_apply, apply_delay_ms=apply_delay_ms,
                progress_cb=on_progress))
            break
        except ProtocolError as e:
            if pbar:
                pbar.close(); pbar = None
            if idx + 1 < len(payloads):
                click.secho(f"delta rejected ({e}) — falling back to full",
                            fg="yellow", err=True)
                continue
            raise click.ClickException(str(e))
        except LinkError as e:
            if pbar:
                pbar.close()
            raise click.ClickException(str(e))
        finally:
            if pbar and not pbar.disable:
                pbar.close(); pbar = None

    _delta_record(device_id, target, target_sha_hex)
    elapsed = time.monotonic() - t0
    click.secho(f"done: {sent_size/1024:.1f} KB sent in {elapsed:.1f} s "
                f"({sent_size/1024/max(elapsed,0.001):.1f} KB/s wall)",
                fg="green", err=True)

