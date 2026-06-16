"""USB-DFU 1.1 uploader — wraps the `dfu-util` binary.

This is a standalone mechanism (the USB DFU class), not the DFU Session
Protocol: the device exposes a DFU interface and dfu-util drives the
standard download + manifest + detach sequence. The device side is the
dfu_core USB-DFU adapter (main/usb/usb_dfu.c); here we just orchestrate
the host tool.

The raw OTA app image (.bin) is the download payload — NOT a combined
uf2/dfu.bin. dfu_core auto-detects raw vs UF2 vs delta from the first
block, so a delta patch container could also be sent this way.
"""
from __future__ import annotations

import shutil
import subprocess

DEFAULT_VID_PID = "303a:4002"


def dfu_util_available() -> str | None:
    """Path to the dfu-util binary, or None."""
    return shutil.which("dfu-util")


def upload(image: bytes, vid_pid: str = DEFAULT_VID_PID, *,
           alt: int = 0, reset: bool = True, sudo: bool = False,
           on_line=None) -> None:
    """Download `image` to the device's DFU interface via dfu-util.

    Streams dfu-util's stderr/stdout line-by-line to `on_line` (so a
    caller can render the progress bar / forward to a log). Raises
    RuntimeError on a non-zero exit or if dfu-util is missing."""
    exe = dfu_util_available()
    if exe is None:
        raise RuntimeError(
            "dfu-util not found on PATH. Install it (apt install dfu-util) "
            "or use a different transport.")

    # dfu-util needs a file path, not a stream. Use a temp file so callers
    # can hand us bytes (e.g. a delta container assembled in memory).
    import tempfile
    with tempfile.NamedTemporaryFile(suffix=".bin", delete=True) as tmp:
        tmp.write(image)
        tmp.flush()
        cmd = ([] if not sudo else ["sudo"]) + [
            exe, "-d", vid_pid, "-a", str(alt), "-D", tmp.name,
        ]
        if reset:
            cmd.append("-R")
        proc = subprocess.Popen(
            cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, bufsize=1)
        assert proc.stdout is not None
        for line in proc.stdout:
            line = line.rstrip("\n")
            if on_line:
                on_line(line)
        rc = proc.wait()
        if rc != 0:
            raise RuntimeError(f"dfu-util exited {rc}")
