"""The links dfukit runs over: a frame is one dfukit message, the transport keeps them apart."""

from collections.abc import Callable
from importlib.metadata import entry_points

ENTRY_POINT = "dfukit.transports"


class LinkError(Exception):
    """The link is gone: reopening it may bring it back."""


class Transport:
    """A link to one device. Subclasses implement open, send and close, and hand each frame from
    the device to received(), from any thread."""

    # The largest frame the link carries
    max_frame = 512

    def __init__(self) -> None:
        self.on_frame: Callable[[bytes], None] | None = None

    def open(self, address: str) -> None:
        raise NotImplementedError

    def send(self, frame: bytes) -> None:
        raise NotImplementedError

    def close(self) -> None:
        raise NotImplementedError

    def received(self, frame: bytes) -> None:
        if self.on_frame is not None:
            self.on_frame(frame)


def _built_in() -> dict[str, type[Transport]]:
    from dfukit.transports.serial import SerialTransport
    from dfukit.transports.udp import UdpTransport

    return {"serial": SerialTransport, "udp": UdpTransport}


def available_transports() -> list[str]:
    """The names of the built-in transports and those that packages register under
    dfukit.transports. A plugin is only loaded when it is opened: a broken one breaks only
    itself."""
    return sorted({*_built_in(), *(entry.name for entry in entry_points(group=ENTRY_POINT))})


def open_transport(via: str) -> Transport:
    """A transport for NAME:ADDRESS, opened."""
    name, _, address = via.partition(":")
    plugins = entry_points(group=ENTRY_POINT, name=name)
    if plugins:
        transport_type = next(iter(plugins)).load()
    elif name in _built_in():
        transport_type = _built_in()[name]
    else:
        known = ", ".join(available_transports())
        raise ValueError(f"no transport {name!r}; known: {known}")
    transport: Transport = transport_type()
    transport.open(address)
    return transport
