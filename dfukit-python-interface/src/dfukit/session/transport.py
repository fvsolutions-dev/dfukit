"""Transport protocol / ABC for DFU Session Protocol v1.1.

A Transport provides the BLE (or any other) I/O primitive.  Session logic
lives in session.py and is transport-agnostic — it only calls the methods
defined here.

Error taxonomy
--------------
LinkError   — retriable link-level failure: timeouts, broken pipe, BLE
              disconnects, scan-miss during link teardown.  The session
              module's reconnect loop catches these and retries.
ProtocolError — fatal application-level rejection: the device refused the
              START (wrong hw_id, SHA mismatch, etc.).  Retrying with the
              same image would fail again.
"""
from __future__ import annotations

from abc import ABC, abstractmethod
from typing import Callable


class LinkError(OSError):
    """Retriable link-level failure (timeout, disconnect, broken pipe…)."""


class ProtocolError(RuntimeError):
    """Fatal protocol rejection (wrong hw_id, SHA mismatch, BUSY…)."""


NotifyCallback = Callable[[bytes], None]


class Transport(ABC):
    """Abstract transport interface consumed by Session."""

    # Maximum bytes per outgoing frame (DATA payload = max_write_size - 5).
    @property
    @abstractmethod
    def max_write_size(self) -> int:
        """ATT MTU minus headers, or equivalent for non-BLE transports."""
        ...

    @abstractmethod
    async def send(self, frame: bytes) -> None:
        """Send one protocol frame.  Raise LinkError on failure."""
        ...

    @abstractmethod
    def set_notify_callback(self, cb: NotifyCallback) -> None:
        """Register the callback that fires on every incoming notification."""
        ...

    @abstractmethod
    async def close(self) -> None:
        """Tear down the connection cleanly."""
        ...
