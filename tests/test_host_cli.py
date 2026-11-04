"""The dfukit command, against a device on UDP."""

from __future__ import annotations

from collections.abc import Iterator
from pathlib import Path
from typing import Any

import pytest
from simulator import BOARD, Device, UdpDevice, image

import dfukit
import dfukit.cli
from dfukit.cli import main


@pytest.fixture
def server() -> Iterator[UdpDevice]:
    device = Device()
    device.add_board(BOARD, version="1.2.3")
    server = UdpDevice(device)
    yield server
    server.close()


def test_info(server: UdpDevice, capsys: pytest.CaptureFixture[str]) -> None:
    main(["info", "--via", f"udp:127.0.0.1:{server.port}"])

    printed = capsys.readouterr().out
    assert "test (protocol 2)" in printed
    assert "0x0000a001 ram: image" in printed and "running 1.2.3" in printed


def test_update_raw(server: UdpDevice, tmp_path: Path) -> None:
    data = image(20_000)
    path = tmp_path / "image.bin"
    path.write_bytes(data)
    ram = server.device.add_board(0xB001)

    main(["update", "--via", f"udp:127.0.0.1:{server.port}", "--board", "0xB001", str(path)])

    assert ram.activated and ram.image == data


def test_update_without_activate(server: UdpDevice, tmp_path: Path) -> None:
    path = tmp_path / "image.bin"
    path.write_bytes(image(1000))
    ram = server.device.add_board(0xB001)

    main(
        [
            "update",
            "--via",
            f"udp:127.0.0.1:{server.port}",
            "--board",
            "0xB001",
            "--encoding",
            "raw",
            "--no-activate",
            str(path),
        ]
    )

    assert ram.finished and not ram.activated


def test_unknown_board(server: UdpDevice, tmp_path: Path) -> None:
    path = tmp_path / "image.bin"
    path.write_bytes(b"x")

    with pytest.raises(SystemExit, match="update failed: UNKNOWN_BOARD"):
        main(["update", "--via", f"udp:127.0.0.1:{server.port}", "--board", "0x1234", str(path)])


def test_a_board_that_does_not_fit_info_is_still_updated(tmp_path: Path) -> None:
    device = Device(max_frame=64)
    rams = [device.add_board(0xA000 + board) for board in range(5)]
    data = image(2000)
    path = tmp_path / "image.bin"
    path.write_bytes(data)
    server = UdpDevice(device)

    main(["update", "--via", f"udp:127.0.0.1:{server.port}", "--board", "0xA000", str(path)])

    server.close()
    assert rams[0].activated and rams[0].image == data


def test_board_id_over_32_bits_is_a_message(capsys: pytest.CaptureFixture[str]) -> None:
    with pytest.raises(SystemExit):
        main(["update", "--via", "udp:127.0.0.1:1", "--board", "0x1FFFFFFFF", "image.bin"])

    assert "does not fit 32 bits" in capsys.readouterr().err


def test_link_lost_during_an_update_reconnects(
    server: UdpDevice, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    ram = server.device.add_board(0xB001)
    ram.behave(window=2048)
    data = image(40_000)
    path = tmp_path / "image.bin"
    path.write_bytes(data)
    server.deaf_after, server.deaf_for = 20, 0.1
    opens: list[str] = []

    def open_transport(via: str) -> dfukit.Transport:
        opens.append(via)
        # The link does not come back at the first try after it was lost
        if len(opens) == 2:
            raise dfukit.LinkError("not back yet")
        return dfukit.open_transport(via)

    def quick_update(*args: Any, **options: Any) -> dfukit.Update:
        return dfukit.Update(*args, rto_ms=10, tries=2, **options)

    monkeypatch.setattr(dfukit.cli, "open_transport", open_transport)
    monkeypatch.setattr(dfukit.cli, "Update", quick_update)

    main(["update", "--via", f"udp:127.0.0.1:{server.port}", "--board", "0xB001", str(path)])

    assert len(opens) >= 3 and ram.begins == 1 and ram.image == data


def test_deflate_on_a_board_without_it(server: UdpDevice, tmp_path: Path) -> None:
    path = tmp_path / "image.bin"
    path.write_bytes(b"x")

    with pytest.raises(SystemExit, match="as taking deflate"):
        main(
            [
                "update",
                "--via",
                f"udp:127.0.0.1:{server.port}",
                "--board",
                str(BOARD),
                "--encoding",
                "deflate",
                str(path),
            ]
        )


def test_unknown_transport_is_a_message() -> None:
    with pytest.raises(SystemExit, match="info failed: no transport"):
        main(["info", "--via", "nothing:address"])
