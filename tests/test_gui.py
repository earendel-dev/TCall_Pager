"""Exercise the native Qt application with a fake transport, never real hardware."""
import os
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
from pathlib import Path
import sys
import time
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "GUI"))
from PySide6.QtWidgets import QApplication, QLineEdit
from gui import MainWindow
from serial_worker import SerialWorker

APP = QApplication.instance() or QApplication([])


class GuiTests(unittest.TestCase):
    def setUp(self):
        with patch("serial.tools.list_ports.comports", return_value=[]):
            self.window = MainWindow()

    def tearDown(self):
        self.window.worker = None
        self.window.close()
        APP.processEvents()

    def transport(self):
        self.window.worker = Mock()
        return self.window.worker

    def reply(self, request, status="OK", payload="done"):
        self.window._line(f"TCALL/1\t{request.request_id}\t{status}\t{payload.encode().hex()}")

    def test_disconnected_controls_are_disabled(self):
        self.assertFalse(self.window.controls.isEnabled())
        self.assertFalse(self.window.read_actions.isEnabled())

    def test_controls_wait_for_device_handshake(self):
        worker = self.transport()
        self.window._connected()
        self.assertFalse(self.window._ready)
        self.window._tick()
        self.assertEqual(worker.submit.call_args_list[0].args[0], b"\n")
        self.assertEqual(worker.submit.call_count, 2)
        self.reply(self.window._pending, payload="TCall Pager 1.1.0-rc.1")
        self.assertTrue(self.window._ready)
        self.assertEqual(self.window._queue[0].command, "STATUS")

    def test_wrong_device_response_does_not_enable_controls(self):
        self.transport()
        self.window._connected()
        self.window._tick()
        self.reply(self.window._pending, payload="Other controller")
        self.assertFalse(self.window._ready)

    def test_gpio_uses_machine_protocol(self):
        self.transport()
        self.window._set_ready(True)
        self.window.mode.setCurrentText("TIMER")
        self.window.work_minutes.setValue(5)
        self.window.apply_gpio()
        frame = self.window._queue[0].frame
        fields = frame.decode().strip().split("\t")
        self.assertEqual(fields[2], "GPIO")
        self.assertEqual([bytes.fromhex(v).decode() for v in fields[3:]], ["12", "TIMER", "5", "0"])

    def test_failed_request_cancels_remaining_commands(self):
        self.transport()
        self.window._set_ready(True)
        self.window.request("SET_ADMIN", "wrong", "+12345678900")
        self.window.request("SAFE_RESET", "wrong")
        self.window._tick()
        self.reply(self.window._pending, status="ERR", payload="Authentication failed")
        self.assertEqual(len(self.window._queue), 0)

    def test_disconnect_clears_queue_and_passwords(self):
        worker = self.transport()
        self.window._set_ready(True)
        self.window.password.setText("secret")
        self.window.new_password.setText("another")
        self.window.request("STATUS")
        self.window.disconnect()
        worker.stop.assert_called_once()
        self.assertEqual(len(self.window._queue), 0)
        self.assertEqual(self.window.password.text(), "")
        self.assertEqual(self.window.new_password.text(), "")

    def test_password_fields_are_masked(self):
        for field in (self.window.password, self.window.new_password):
            self.assertEqual(field.echoMode(), QLineEdit.EchoMode.Password)

    def test_mismatched_response_cannot_complete_request(self):
        self.transport()
        self.window._set_ready(True)
        self.window.request("STATUS")
        self.window._tick()
        pending = self.window._pending
        self.window._line("TCALL/1\t0\tOK\t")
        self.assertIs(self.window._pending, pending)

    def test_timeout_disconnects_without_replaying_command(self):
        worker = self.transport()
        self.window._set_ready(True)
        self.window.request("GPIO", "12", "ON", "0", "0")
        self.window._tick()
        self.window._deadline = time.monotonic() - 1
        self.window._tick()
        worker.stop.assert_called_once()
        self.assertFalse(self.window._ready)
        self.assertIsNone(self.window._pending)

    def test_untrusted_device_text_stays_plain_text(self):
        self.window._line("<b>device text</b>")
        self.assertIn("<b>device text</b>", self.window.console.toPlainText())


class WorkerTests(unittest.TestCase):
    def test_factory_failure_is_reported(self):
        worker = SerialWorker("fake", serial_factory=Mock(side_effect=OSError("unavailable")))
        errors = []
        worker.failed.connect(errors.append)
        worker.run()
        self.assertEqual(len(errors), 1)

    def test_partial_utf8_and_frames_are_assembled(self):
        chunks = iter([b"boot \xd0", b"\x90\nTCALL/1\t1\tOK\t6869\n"])
        worker = None
        class FakePort:
            in_waiting = 1
            def open(self):
                pass
            def read(self, size):
                try:
                    return next(chunks)
                except StopIteration:
                    worker.stop()
                    return b""
            def close(self):
                self.closed = True
        port = FakePort()
        worker = SerialWorker("fake", serial_factory=Mock(return_value=port))
        lines = []
        worker.line_received.connect(lines.append)
        worker.run()
        self.assertEqual(lines, ["boot А", "TCALL/1\t1\tOK\t6869"])
        self.assertFalse(port.dtr)
        self.assertFalse(port.rts)
        self.assertTrue(port.closed)


if __name__ == "__main__":
    unittest.main()
