"""Delta-OTA support: firmware cache + detools patch generation.

The device (dfu_core, CONFIG_DFU_CORE_DELTA_ENABLE) auto-detects the
esp_delta_ota patch container and applies it against its RUNNING
partition. The container is a 64-byte header — magic 0xfccdde10 (LE),
the base image's appended SHA-256 (esptool's "Validation Hash"), 28
reserved bytes — followed by a heatshrink-compressed detools
'sequential' patch.

The base digest is verified ON-DEVICE before any flash write, so the
host-side bookkeeping here (which image does each device run?) never
needs to be trusted: a stale record produces a clean rejection and the
caller falls back to a full upload.

Cache layout (DFU_FW_CACHE env var or ~/.cache/dfu-fw-cache):
    <sha256>.bin    content-addressed images
    index.json      sha -> {size, added, project, built, ...}
    devices.json    device-id (BLE address) -> sha of running image
"""

from __future__ import annotations

import hashlib
import json
import os
import struct
import time
from pathlib import Path

DELTA_MAGIC = 0xFCCDDE10
DELTA_HEADER_BYTES = 64

# esp_app_desc_t sits at a fixed offset in every app image:
# esp_image_header_t (24 B) + first segment header (8 B).
_APP_DESC_OFFSET = 0x20
_APP_DESC_MAGIC = 0xABCD5432
# esp_image_header_t.hash_appended is the last byte of the 24-byte
# image header; when 1, the file's final 32 bytes are the SHA-256 of
# everything before them — exactly what esptool reports as the
# Validation Hash and what esp_partition_get_sha256() returns on the
# device for the running app.
_HASH_APPENDED_OFFSET = 23


def cache_dir() -> Path:
    d = os.environ.get("DFU_FW_CACHE")
    return Path(d) if d else Path.home() / ".cache" / "dfu-fw-cache"


def _load(path: Path) -> dict:
    return json.loads(path.read_text()) if path.exists() else {}


def _save(path: Path, data: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")


def parse_app_desc(image: bytes) -> dict:
    """Project/version/build metadata embedded in an ESP app image."""
    if len(image) < _APP_DESC_OFFSET + 256:
        return {}
    (magic,) = struct.unpack_from("<I", image, _APP_DESC_OFFSET)
    if magic != _APP_DESC_MAGIC:
        return {}
    off = _APP_DESC_OFFSET + 16
    fields = image[off:off + 96]
    version = fields[0:32].split(b"\0")[0].decode(errors="replace")
    project = fields[32:64].split(b"\0")[0].decode(errors="replace")
    btime = fields[64:80].split(b"\0")[0].decode(errors="replace")
    bdate = fields[80:96].split(b"\0")[0].decode(errors="replace")
    return {"project": project, "version": version, "built": f"{bdate} {btime}"}


def appended_sha256(image: bytes) -> bytes:
    """The image's appended SHA-256 (the digest the device verifies).

    Raises ValueError for images built without hash-appended (rare —
    it's the IDF default)."""
    if len(image) < 64 or image[_HASH_APPENDED_OFFSET] != 1:
        raise ValueError("image has no appended SHA-256 (hash_appended != 1)")
    return image[-32:]


def cache_add(image: bytes) -> str:
    """Store an image; returns its (file) sha256 hex."""
    sha = hashlib.sha256(image).hexdigest()
    d = cache_dir()
    d.mkdir(parents=True, exist_ok=True)
    blob = d / f"{sha}.bin"
    if not blob.exists():
        blob.write_bytes(image)
    index = _load(d / "index.json")
    if sha not in index:
        index[sha] = {
            "size": len(image),
            "added": time.strftime("%Y-%m-%d %H:%M:%S"),
            **parse_app_desc(image),
        }
        _save(d / "index.json", index)
    return sha


def record_running(device_id: str, image_sha: str) -> None:
    """Remember that `device_id` now runs the image with this sha."""
    d = cache_dir()
    devices = _load(d / "devices.json")
    devices[device_id] = image_sha
    _save(d / "devices.json", devices)


def lookup_running(device_id: str) -> bytes | None:
    """Return the cached image bytes the device is believed to run."""
    d = cache_dir()
    sha = _load(d / "devices.json").get(device_id)
    if not sha:
        return None
    blob = d / f"{sha}.bin"
    return blob.read_bytes() if blob.exists() else None


def make_patch(base: bytes, target: bytes) -> bytes:
    """Build a device-ready delta container: header + detools patch.

    Imports detools lazily so the rest of the package works without
    the compiled dependency installed."""
    import io

    import detools

    header = struct.pack("<I", DELTA_MAGIC) + appended_sha256(base)
    header += b"\0" * (DELTA_HEADER_BYTES - len(header))

    fpatch = io.BytesIO()
    detools.create_patch(io.BytesIO(base), io.BytesIO(target), fpatch,
                         compression="heatshrink")
    return header + fpatch.getvalue()
