"""UART ROM-bootloader uploader — esptool as a library.

Standalone mechanism: the ESP chip's mask-ROM serial bootloader (the
esptool protocol), not the DFU Session protocol and nothing to do with
the running app. Writes the raw app image to flash over a local serial
port or a serial-over-IP (RFC 2217) bridge.

Entry into download mode: on a real serial port esptool straps GPIO0 +
toggles reset via DTR/RTS automatically. Over an RFC 2217 bridge that
auto-strap is the fragile part — many bridges drop or reorder the
control-line round-trips (see Notes.md). If sync fails over RFC 2217,
put the device in download mode manually first, and/or try --no-stub.
"""
from __future__ import annotations

# App partition offset in this project's layout (partitions.csv: ota_0 at
# 0x20000). The ROM bootloader writes absolute flash offsets; the app slot
# is the sensible default target for a single-image flash.
DEFAULT_APP_OFFSET = 0x20000


def write_flash(image_path: str, *, port: str, baud: int = 460800,
                offset: int = DEFAULT_APP_OFFSET, chip: str = "esp32s3",
                stub: bool = True) -> None:
    """Flash one image via esptool.main(). Raises RuntimeError on failure.

    esptool resolves rfc2217:// ports through pyserial, so `port` may be a
    device path or an RFC 2217 URL transparently."""
    try:
        import esptool
    except ImportError as e:
        raise RuntimeError(
            "esptool not installed — the uart (ROM bootloader) transport "
            "needs it. Install the extra: pip install dfukit[uart]") from e

    argv = [
        "--chip", chip,
        "--port", port,
        "--baud", str(baud),
        "--before", "default-reset",
        "--after", "hard-reset",
    ]
    if not stub:
        argv.append("--no-stub")
    argv += ["write-flash", hex(offset), image_path]

    try:
        # esptool.main raises SystemExit on argparse / fatal errors and
        # propagates its own exceptions; normalise to RuntimeError.
        esptool.main(argv)
    except SystemExit as e:
        if e.code not in (0, None):
            raise RuntimeError(f"esptool exited {e.code}") from e
    except Exception as e:  # esptool.FatalError et al.
        raise RuntimeError(f"esptool: {e}") from e
