"""What goes wrong in a Python callback, inside the C code, comes out in Python."""

import pytest
from simulator import BOARD

import dfukit


def test_exception_in_the_clients_send_is_raised() -> None:
    def send(_frame: bytes) -> None:
        raise ValueError("from send")

    client = dfukit.Client(send, lambda _offset, _length: b"")

    with pytest.raises(ValueError, match="from send"):
        client.begin(BOARD, bytes(10), 0)


def test_exception_in_the_servers_send_is_raised() -> None:
    def send(_peer: bytes, _frame: bytes) -> None:
        raise ValueError("from send")

    server = dfukit.Server(send)

    with pytest.raises(ValueError, match="from send"):
        server.feed(b"\x01", bytes([0x01]), 0)


def test_it_is_raised_once() -> None:
    def send(_peer: bytes, _frame: bytes) -> None:
        raise ValueError("from send")

    server = dfukit.Server(send)
    with pytest.raises(ValueError):
        server.feed(b"\x01", bytes([0x01]), 0)

    server.poll(0)


def test_a_status_it_does_not_know_stays_a_number() -> None:
    client = dfukit.Client(lambda _frame: None, lambda _offset, _length: b"")
    client.begin(BOARD, bytes(10), 0)

    client.feed(bytes([0x82, 0x55]) + bytes(12), 0)

    assert client.state == dfukit.ClientState.FAILED and client.status == 0x55
