"""src/js/dfukit.js in Node, against the C server."""

from __future__ import annotations

import json
import shutil
import struct
import subprocess
import threading
import time
import zlib
from pathlib import Path

import pytest
from simulator import BOARD, HOST, OP_ACTIVATE, OP_ANSWER, Device, image, real_ms

import dfukit

NODE = shutil.which("node")
pytestmark = pytest.mark.skipif(NODE is None, reason="no Node")
CLIENT = Path(__file__).parent / "js_client.js"


def text_image(size: int) -> bytes:
    return (b"alpha beta gamma delta " * (size // 23 + 1))[:size]


def run_client(
    device: Device, data: bytes, tmp_path: Path, lose: int | None = None, **options: object
) -> list[str]:
    """Runs the client against the device, which loses its answers with the opcode lose; what the
    client said on stderr."""
    path = tmp_path / "image.bin"
    path.write_bytes(data)
    assert NODE is not None
    node = subprocess.Popen(
        [NODE, str(CLIENT), json.dumps({"image": str(path), "board": BOARD, **options})],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    assert node.stdin is not None and node.stdout is not None and node.stderr is not None
    frames: list[bytes] = []

    def read_frames() -> None:
        assert node.stdout is not None
        while header := node.stdout.read(2):
            (length,) = struct.unpack("<H", header)
            frames.append(node.stdout.read(length))

    def to_client(_to: bytes, frame: bytes) -> None:
        assert node.stdin is not None
        if frame[0] == lose:
            return
        node.stdin.write(struct.pack("<H", len(frame)) + frame)
        node.stdin.flush()

    device.on_host = to_client
    reader = threading.Thread(target=read_frames, daemon=True)
    reader.start()
    end = time.monotonic() + 60
    try:
        while node.poll() is None and time.monotonic() < end:
            while frames:
                device.feed(HOST, frames.pop(0), real_ms())
            device.poll(real_ms())
            time.sleep(0.001)
    except BrokenPipeError:
        pass
    assert node.poll() is not None, "the client did not end"
    return node.stderr.read().decode().splitlines()


def test_crc32_as_zlib(tmp_path: Path) -> None:
    device = Device()
    device.add_board()
    data = image(100_000)

    said = run_client(device, data, tmp_path)

    assert said[0] == f"crc32 {zlib.crc32(data)}"


def test_raw(tmp_path: Path) -> None:
    device = Device()
    ram = device.add_board()
    data = image(100_000)

    said = run_client(device, data, tmp_path)

    assert said[-1] == f"done {len(data)} 0"
    assert ram.activated and ram.image == data


def test_deflate(tmp_path: Path) -> None:
    device = Device()
    device.set_inflater(dfukit.Inflate(15))
    ram = device.add_board()
    data = text_image(100_000)

    said = run_client(device, data, tmp_path, deflate=True)

    assert said[-1].startswith("done ") and int(said[-1].split()[1]) < len(data) // 2
    assert ram.image == data


def test_lossy_link(tmp_path: Path) -> None:
    device = Device()
    ram = device.add_board()
    data = image(30_000)

    said = run_client(device, data, tmp_path, drop=0.05, rtoMs=20)

    assert said[-1].startswith("done ") and int(said[-1].split()[2]) > 0
    assert ram.image == data


def test_without_activate(tmp_path: Path) -> None:
    device = Device()
    ram = device.add_board()

    said = run_client(device, image(5000), tmp_path, activate=False)

    assert said[-1].startswith("done ") and ram.finished and not ram.activated


def test_refused(tmp_path: Path) -> None:
    device = Device()
    device.add_board()

    said = run_client(device, image(100), tmp_path, board=0x1234)

    assert said[-1] == "failed UNKNOWN_BOARD"


def test_lost_answer_to_activate_is_said_so(tmp_path: Path) -> None:
    device = Device()
    ram = device.add_board()

    said = run_client(device, image(5000), tmp_path, lose=OP_ANSWER | OP_ACTIVATE, rtoMs=5, tries=2)

    assert said[-1] == "failed TIMEOUT: the image was sent, but its activation is not confirmed"
    assert ram.begins == 1 and ram.activated
