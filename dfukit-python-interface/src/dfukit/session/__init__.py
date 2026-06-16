"""DFU Session Protocol v1.1 — transport-agnostic OTA mechanism.

A frame-based protocol (manifest + SHA-256, sliding-window flow control,
PROGRESS acks, suspend/resume, delta patches). The protocol lives in
protocol.py / wire.py; concrete links live under transports/.
"""
from .protocol import Session, upload_with_reconnect
from .transport import LinkError, NotifyCallback, ProtocolError, Transport
from . import delta, wire

__all__ = [
    "Session", "upload_with_reconnect",
    "Transport", "NotifyCallback", "LinkError", "ProtocolError",
    "delta", "wire",
]
