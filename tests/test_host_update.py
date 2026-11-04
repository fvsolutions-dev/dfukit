"""dfukit.Update through a transport: a device in this process, on the real clock."""

from __future__ import annotations

import pytest
from simulator import BOARD, OP_ACTIVATE, OP_ANSWER, Device, Loopback, image

import dfukit
import dfukit.update


def text_image(size: int) -> bytes:
    return (b"alpha beta gamma delta " * (size // 23 + 1))[:size]


def device_with_inflater() -> tuple[Device, dfukit.RamBackend]:
    device = Device()
    device.set_inflater(dfukit.Inflate(15))
    return device, device.add_board()


def test_info() -> None:
    device = Device()
    device.add_board(BOARD, version="1.2.3")
    transport = Loopback(device)

    found = dfukit.info(transport)

    transport.close()
    assert found.name == "test" and found.version == 2
    board = found.board(BOARD)
    assert board is not None and board.name == "ram" and board.version == "1.2.3"
    assert board.takes(dfukit.RAW) and found.board(0x1234) is None


def test_info_from_a_device_that_does_not_answer() -> None:
    transport = Loopback(Device(), dead=True)

    with pytest.raises(dfukit.UpdateError, match="TIMEOUT"):
        dfukit.info(transport, timeout=0.01, tries=2)

    transport.close()


@pytest.mark.parametrize("frame", [b"", b"\x81", b"\x81\x02\x01a\x01\x00", b"\x01\x02\x00\x00"])
def test_malformed_info(frame: bytes) -> None:
    with pytest.raises(ValueError):
        dfukit.parse_info(frame)


def test_raw() -> None:
    device = Device()
    ram = device.add_board()
    data = image(50_000)
    transport = Loopback(device)
    seen: list[tuple[int, int]] = []

    update = dfukit.Update(transport, data, BOARD)
    update.run(progress=lambda done, total: seen.append((done, total)))

    transport.close()
    assert ram.activated and ram.image == data and update.resent == 0
    assert seen[-1] == (len(data), len(data))


def test_deflate() -> None:
    device, ram = device_with_inflater()
    data = text_image(50_000)
    transport = Loopback(device)

    update = dfukit.Update(transport, data, BOARD, encoding=dfukit.DEFLATE)
    update.run()

    transport.close()
    assert len(update.stream) < len(data) // 2 and ram.image == data


def test_lossy_link() -> None:
    device = Device()
    ram = device.add_board()
    data = image(30_000)
    transport = Loopback(device, drop=0.05)

    update = dfukit.Update(transport, data, BOARD, rto_ms=20)
    update.run()

    transport.close()
    assert ram.image == data and update.resent > 0


def test_without_activate() -> None:
    device = Device()
    ram = device.add_board()
    transport = Loopback(device)

    dfukit.Update(transport, image(5000), BOARD, activate=False).run()

    transport.close()
    assert ram.finished and not ram.activated


def test_refused() -> None:
    device = Device()
    device.add_board()
    transport = Loopback(device)

    with pytest.raises(dfukit.UpdateError, match="UNKNOWN_BOARD") as refused:
        dfukit.Update(transport, image(100), 0x1234).run()

    transport.close()
    assert refused.value.status == dfukit.Status.UNKNOWN_BOARD


def test_lost_link_without_reconnect_fails() -> None:
    device = Device()
    device.add_board()
    transport = Loopback(device, fail_after=3)

    with pytest.raises(dfukit.LinkError):
        dfukit.Update(transport, image(50_000), BOARD).run()

    transport.close()


def test_reconnect_resumes() -> None:
    device = Device()
    ram = device.add_board()
    ram.behave(window=2048)
    data = image(50_000)
    first = Loopback(device, fail_after=20)
    second: list[Loopback] = []

    def reconnect() -> dfukit.Transport:
        first.close()
        second.append(Loopback(device))
        return second[0]

    dfukit.Update(first, data, BOARD).run(reconnect=reconnect)

    second[0].close()
    assert len(second) == 1 and ram.begins == 1 and ram.image == data


def test_reconnecting_to_a_dead_device_gives_up(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(dfukit.update, "GIVE_UP_S", 0.2)
    device = Device()
    device.add_board()
    transport = Loopback(device, dead=True)
    reconnects: list[int] = []

    def reconnect() -> dfukit.Transport:
        reconnects.append(1)
        return transport

    with pytest.raises(dfukit.UpdateError, match="TIMEOUT"):
        dfukit.Update(transport, image(100), BOARD, rto_ms=5, tries=1).run(reconnect=reconnect)

    transport.close()
    assert reconnects


def lose_answers_to_activate(transport: Loopback) -> None:
    def answer(_to: bytes, frame: bytes) -> None:
        if frame[0] != OP_ANSWER | OP_ACTIVATE:
            transport.received(frame)

    transport.device.on_host = answer


def test_lost_answer_to_activate_does_not_flash_again() -> None:
    device = Device()
    ram = device.add_board()
    transport = Loopback(device)
    lose_answers_to_activate(transport)
    reconnects: list[int] = []

    def reconnect() -> dfukit.Transport:
        reconnects.append(1)
        return transport

    with pytest.raises(dfukit.ActivationNotConfirmed, match="not confirmed"):
        dfukit.Update(transport, image(5000), BOARD, rto_ms=5, tries=1).run(reconnect=reconnect)

    transport.close()
    assert ram.begins == 1 and ram.activated and not reconnects


def test_link_lost_on_activate_does_not_flash_again() -> None:
    device = Device()
    ram = device.add_board()
    transport = Loopback(device)
    lose_answers_to_activate(transport)

    def lost_link() -> dfukit.Transport:
        raise AssertionError("no reconnect once ACTIVATE is sent")

    update = dfukit.Update(transport, image(5000), BOARD, rto_ms=5, tries=3)
    original = transport.send

    def send(frame: bytes) -> None:
        original(frame)
        if frame[0] == OP_ACTIVATE:
            raise dfukit.LinkError("gone")

    transport.send = send  # type: ignore[method-assign]

    with pytest.raises(dfukit.ActivationNotConfirmed):
        update.run(reconnect=lost_link)

    transport.close()
    assert ram.begins == 1


def test_a_refused_activate_is_not_a_lost_answer() -> None:
    device = Device()
    ram = device.add_board()
    ram.behave(refuse_activate=True)
    transport = Loopback(device)

    with pytest.raises(dfukit.UpdateError, match="BACKEND") as refused:
        dfukit.Update(transport, image(5000), BOARD).run()

    transport.close()
    assert not isinstance(refused.value, dfukit.ActivationNotConfirmed)
