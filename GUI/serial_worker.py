"""All serial operations run on one worker, with bounded reads and writes."""
from queue import Empty, Queue
import threading
import serial
from PySide6.QtCore import QThread, Signal
from protocol import MAX_LINE_BYTES


class SerialWorker(QThread):
    connected = Signal()
    line_received = Signal(str)
    failed = Signal(str)

    def __init__(self, port, parent=None, serial_factory=serial.Serial):
        super().__init__(parent)
        self.port = port
        self._factory = serial_factory
        self._stop_event = threading.Event()
        self._outgoing = Queue(maxsize=32)

    def submit(self, frame):
        if self._stop_event.is_set() or not self.isRunning():
            raise RuntimeError("The serial connection is closed.")
        self._outgoing.put_nowait(frame)

    def stop(self):
        self._stop_event.set()

    def run(self):
        port = None
        pending = bytearray()
        discarding = False
        try:
            port = self._factory(port=None, baudrate=115200, timeout=.1, write_timeout=1)
            # Do not pulse RTS to reset a running controller. USB drivers can
            # still briefly change control lines when opening the port.
            port.dtr = False
            port.rts = False
            port.port = self.port
            port.open()
            if self._stop_event.is_set():
                return
            self.connected.emit()
            while not self._stop_event.is_set():
                try:
                    frame = self._outgoing.get_nowait()
                except Empty:
                    frame = None
                if frame is not None and port.write(frame) != len(frame):
                    raise OSError("Incomplete serial write.")
                chunk = port.read(min(max(port.in_waiting, 1), 1024))
                for byte in chunk:
                    if byte == 10:
                        if not discarding:
                            self.line_received.emit(pending.rstrip(b"\r").decode("utf-8", "replace"))
                        pending.clear()
                        discarding = False
                    elif not discarding:
                        pending.append(byte)
                        if len(pending) > MAX_LINE_BYTES:
                            pending.clear()
                            discarding = True
                            self.failed.emit("Oversized serial line ignored.")
        except Exception as error:
            if not self._stop_event.is_set():
                self.failed.emit(f"Serial connection failed: {error}")
        finally:
            if port is not None:
                port.close()
