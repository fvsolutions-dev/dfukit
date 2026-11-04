"""dfukit info|update --via TRANSPORT:ADDRESS ..."""

import argparse
import sys
import time
from pathlib import Path

from dfukit.transport import LinkError, Transport, open_transport
from dfukit.update import DEFLATE, GIVE_UP_S, KIND_PASSTHROUGH, RAW, Update, UpdateError, info


def _info(args: argparse.Namespace) -> None:
    transport = open_transport(args.via)
    try:
        found = info(transport)
    finally:
        transport.close()
    print(f"{found.name} (protocol {found.version})")
    for board in found.boards:
        kind = "pass-through" if board.kind == KIND_PASSTHROUGH else "image"
        takes = [(RAW, "raw"), (DEFLATE, "deflate")]
        encodings = ", ".join(name for encoding, name in takes if board.takes(encoding))
        print(
            f"  0x{board.board_id:08x} {board.name or '-'}: {kind}, up to {board.max_size} bytes, "
            f"{encodings}, window 2^{board.window_log2}"
            + (f", running {board.version}" if board.version else "")
        )


def _update(args: argparse.Namespace) -> None:
    image = Path(args.image).read_bytes()
    transport = open_transport(args.via)
    try:
        encoding, window = RAW, 15
        if args.encoding != "raw":
            # INFO lists only as many boards as fit a frame: one not listed may still exist, and
            # START is the real check
            board = info(transport).board(args.board)
            if board is not None and board.takes(DEFLATE):
                encoding, window = DEFLATE, board.window_log2
            elif args.encoding == "deflate":
                sys.exit(f"the device does not list board 0x{args.board:08x} as taking deflate")
        update = Update(
            transport,
            image,
            args.board,
            encoding=encoding,
            window_log2=window,
            activate=not args.no_activate,
        )

        def reconnect() -> Transport:
            nonlocal transport
            transport.close()
            # The link may take a while to come back (a USB device that restarts)
            end = time.monotonic() + GIVE_UP_S
            while True:
                try:
                    transport = open_transport(args.via)
                    return transport
                except (OSError, LinkError):
                    if time.monotonic() > end:
                        raise
                    time.sleep(0.5)

        def progress(done: int, total: int) -> None:
            print(f"\r{done}/{total} bytes", end="", flush=True)

        start = time.monotonic()
        update.run(progress=progress, reconnect=reconnect)
        took = time.monotonic() - start
        print(
            f"\r{len(image)} bytes ({len(update.stream)} sent, {update.resent} again) "
            f"in {took:.1f} s"
        )
    finally:
        transport.close()


def _board_id(text: str) -> int:
    board_id = int(text, 0)
    if not 0 <= board_id <= 0xFFFFFFFF:
        raise argparse.ArgumentTypeError(f"{text} does not fit 32 bits")
    return board_id


def main(argv: list[str] | None = None) -> None:
    parser = argparse.ArgumentParser(prog="dfukit")
    commands = parser.add_subparsers(dest="command", required=True)
    command = commands.add_parser("info", help="the device's boards")
    command.add_argument("--via", required=True, help="TRANSPORT:ADDRESS")
    command.set_defaults(run=_info)
    command = commands.add_parser("update", help="send an image")
    command.add_argument("--via", required=True, help="TRANSPORT:ADDRESS")
    command.add_argument("--board", required=True, type=_board_id, help="the board ID")
    command.add_argument(
        "--encoding",
        choices=["auto", "raw", "deflate"],
        default="auto",
        help="auto: deflate when the board takes it",
    )
    command.add_argument("--no-activate", action="store_true", help="leave the image unactivated")
    command.add_argument("image")
    command.set_defaults(run=_update)
    args = parser.parse_args(argv)
    try:
        args.run(args)
    except (UpdateError, LinkError, ValueError, OSError, ImportError) as error:
        sys.exit(f"\n{args.command} failed: {error}")


if __name__ == "__main__":
    main()
