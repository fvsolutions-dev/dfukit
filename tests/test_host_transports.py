"""The transports: UDP on this machine, serial on a pseudo-terminal, plugins by name."""

from __future__ import annotations

import os
import socket
import threading
from types import SimpleNamespace

import pytest
from simulator import BOARD, HOST, Device, Loopback, UdpDevice, image, real_ms

import dfukit
import dfukit.transport


def test_udp() -> None:
    device = Device()
    ram = device.add_board()
    data = image(50_000)
    server = UdpDevice(device)
    transport = dfukit.open_transport(f"udp:127.0.0.1:{server.port}")

    dfukit.Update(transport, data, BOARD).run()

    transport.close()
    server.close()
    assert ram.activated and ram.image == data


def test_udp_device_that_comes_later() -> None:
    # Sending to nobody makes the next receive fail, on Linux refused and on Windows reset
    probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    probe.bind(("127.0.0.1", 0))
    port = probe.getsockname()[1]
    probe.close()
    transport = dfukit.open_transport(f"udp:127.0.0.1:{port}")
    transport.send(b"\x01")
    threading.Event().wait(0.05)

    device = Device()
    ram = device.add_board()
    server = UdpDevice(device, port)
    dfukit.Update(transport, image(5000), BOARD).run()

    transport.close()
    server.close()
    assert ram.activated


def test_unknown_transport() -> None:
    with pytest.raises(ValueError, match="no transport 'nothing'"):
        dfukit.open_transport("nothing:address")


def test_plugin_found_by_name(monkeypatch: pytest.MonkeyPatch) -> None:
    device = Device()
    device.add_board()
    opened: list[str] = []

    class Plugin(Loopback):
        def __init__(self) -> None:
            super().__init__(device)

        def open(self, address: str) -> None:
            opened.append(address)

    monkeypatch.setattr(
        dfukit.transport,
        "entry_points",
        lambda group, name=None: [entry for entry in plugins if name is None or entry.name == name],
    )
    plugins = [
        SimpleNamespace(name="mylink", load=lambda: Plugin),
        SimpleNamespace(name="broken", load=_broken_plugin),
    ]

    transport = dfukit.open_transport("mylink:the:address")

    transport.close()
    assert opened == ["the:address"]
    assert dfukit.available_transports() == ["broken", "mylink", "serial", "udp"]


def _broken_plugin() -> type[dfukit.Transport]:
    raise ImportError("broken")


def test_a_broken_plugin_breaks_only_itself(monkeypatch: pytest.MonkeyPatch) -> None:
    entries = [SimpleNamespace(name="broken", load=_broken_plugin)]
    monkeypatch.setattr(
        dfukit.transport,
        "entry_points",
        lambda group, name=None: [entry for entry in entries if name in (None, entry.name)],
    )
    server = UdpDevice(Device())

    transport = dfukit.open_transport(f"udp:127.0.0.1:{server.port}")
    transport.close()
    server.close()

    with pytest.raises(ImportError):
        dfukit.open_transport("broken:address")


def test_closing_udp_stops_its_reader() -> None:
    server = UdpDevice(Device())
    transport = dfukit.open_transport(f"udp:127.0.0.1:{server.port}")
    reader = transport._reader

    transport.close()
    server.close()

    assert reader is not None
    reader.join(timeout=2)
    assert not reader.is_alive()


def test_serial_on_a_pseudo_terminal() -> None:
    pytest.importorskip("pty")
    pytest.importorskip("serial")
    burst = pytest.importorskip("burst_interface_c")
    import select
    import tty

    device = Device(max_frame=1024)
    ram = device.add_board()
    data = image(50_000)
    master, slave = os.openpty()
    tty.setraw(slave)
    from_host, to_host = burst.BurstInterfaceC(), burst.BurstInterfaceC()
    device.on_host = lambda _to, frame: os.write(master, to_host.encode([frame]))
    running = True

    def run_device() -> None:
        while running:
            if select.select([master], [], [], 0.002)[0]:
                for frame in from_host.decode(os.read(master, 4096)):
                    device.feed(HOST, bytes(frame), real_ms())
            device.poll(real_ms())

    thread = threading.Thread(target=run_device, daemon=True)
    thread.start()
    transport = dfukit.open_transport(f"serial:{os.ttyname(slave)}@115200")

    update = dfukit.Update(transport, data, BOARD)
    update.run()

    transport.close()
    running = False
    thread.join(timeout=1)
    assert ram.activated and ram.image == data
