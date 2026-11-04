"""A UART: its frames burst-link encoded. Address: PORT or PORT@BAUD, e.g. /dev/ttyUSB1@3000000.
Needs pyserial and burst-link (the package's serial extra)."""

import threading

from dfukit.transport import LinkError, Transport

DEFAULT_BAUD = 115200


class SerialTransport(Transport):
    max_frame = 1024

    def open(self, address: str) -> None:
        try:
            import serial
            from burst_interface_c import BurstInterfaceC
        except ImportError as error:
            raise ValueError("the serial transport needs: pip install 'dfukit[serial]'") from error

        self._burst = BurstInterfaceC()
        port, _, baud = address.partition("@")
        try:
            self._port = serial.Serial(port, int(baud) if baud else DEFAULT_BAUD, timeout=0.05)
        except serial.SerialException as e:
            raise LinkError(str(e)) from e
        self._running = True
        self._lost = False
        self._reader = threading.Thread(target=self._read, daemon=True)
        self._reader.start()

    def send(self, frame: bytes) -> None:
        import serial

        # The reader saw the port go: the link is lost, whether a write fails or not
        if self._lost:
            raise LinkError(f"{self._port.port}: gone")
        try:
            self._port.write(self._burst.encode([frame]))
        except serial.SerialException as e:
            raise LinkError(str(e)) from e

    def close(self) -> None:
        self._running = False
        self._reader.join(timeout=1)
        self._port.close()

    def _read(self) -> None:
        while self._running:
            try:
                data = self._port.read(4096)
            except Exception:  # the port went away (a USB device unplugged, reset)
                self._lost = True
                return
            for frame in self._burst.decode(data) if data else []:
                self.received(bytes(frame))
