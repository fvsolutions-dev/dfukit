"""UDP: one datagram per frame, to HOST:PORT."""

import contextlib
import socket
import threading

from dfukit.transport import LinkError, Transport


class UdpTransport(Transport):
    max_frame = 1024

    def __init__(self) -> None:
        super().__init__()
        self._sock: socket.socket | None = None
        self._reader: threading.Thread | None = None

    def open(self, address: str) -> None:
        host, _, port = address.rpartition(":")
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._sock.connect((host, int(port)))
        self._reader = threading.Thread(target=self._read, daemon=True)
        self._reader.start()

    def send(self, frame: bytes) -> None:
        assert self._sock is not None
        try:
            self._sock.send(frame)
        except OSError as e:
            raise LinkError(str(e)) from e

    def close(self) -> None:
        if self._sock is not None:
            # Wakes the reader, which close() alone does not. Not every platform takes it on UDP
            with contextlib.suppress(OSError):
                self._sock.shutdown(socket.SHUT_RDWR)
            self._sock.close()
            self._sock = None

    def _read(self) -> None:
        sock = self._sock
        assert sock is not None
        while True:
            try:
                frame = sock.recv(2048)
            except (ConnectionRefusedError, ConnectionResetError):
                # Nobody on the port (yet): the client's resends find out. Windows says reset
                continue
            except OSError:
                return
            # Nothing to read: the socket was shut down
            if not frame:
                return
            self.received(frame)
