"""TCall Pager desktop controller. Commands complete only after a device reply."""
from collections import deque
from dataclasses import dataclass
import sys
import time
from pathlib import Path
import os
import serial.tools.list_ports
from PySide6.QtCore import QTimer, QCoreApplication, QEvent, Qt, QSettings
from PySide6.QtGui import QTextCursor, QFont, QFontDatabase
from PySide6.QtWidgets import (
    QApplication, QComboBox, QFormLayout, QGroupBox, QHBoxLayout, QLabel,
    QLineEdit, QMainWindow, QMessageBox, QPushButton, QScrollArea, QSpinBox,
    QTextEdit, QVBoxLayout, QWidget,
)
from protocol import VERSION, ProtocolError, encode_request, parse_response, validate_phone, validate_sms, validate_password
from serial_worker import SerialWorker
from i18n import LANGUAGES, translate


def prepare_application(application):
    # Offscreen Windows sessions may not enumerate system fonts. Load the
    # existing OS font explicitly; no Microsoft font is redistributed.
    if not QFontDatabase.families():
        path = Path(os.environ.get("WINDIR", r"C:\Windows")) / "Fonts/segoeui.ttf"
        font_id = QFontDatabase.addApplicationFont(str(path))
        families = QFontDatabase.applicationFontFamilies(font_id) if font_id >= 0 else []
        if not families:
            raise RuntimeError("No readable system font is available.")
        application.setFont(QFont(families[0], 10))
        # Offscreen sessions also need an explicit CJK fallback. Use the OS
        # font when present; Windows font files are never bundled.
        cjk = path.parent / "msyh.ttc"
        if cjk.exists():
            QFontDatabase.addApplicationFont(str(cjk))


@dataclass
class PendingRequest:
    request_id: int
    command: str
    frame: bytes
    timeout: float


class MainWindow(QMainWindow):
    def __init__(self, port_provider=None, language="en"):
        super().__init__()
        self.setWindowTitle(f"TCall Pager {VERSION}")
        self.resize(1050, 760)
        self.worker = None
        self._queue = deque()
        self._pending = None
        self._deadline = 0.0
        self._next_id = time.monotonic_ns() % 1000000 + 1
        self._port_provider = port_provider or serial.tools.list_ports.comports
        self._ready = False
        self._closing = False
        self.language = language if language in dict(LANGUAGES) else "en"
        self._build_ui()
        self._capture_translations()
        self.set_language(self.language)
        self._timer = QTimer(self)
        self._timer.timeout.connect(self._tick)
        self._timer.start(50)
        self.refresh_ports()
        self._set_ready(False)

    @staticmethod
    def _button(label, action, layout):
        button = QPushButton(label)
        button.clicked.connect(action)
        layout.addWidget(button)
        return button

    def _build_ui(self):
        central = QWidget()
        self.setCentralWidget(central)
        root = QVBoxLayout(central)
        connection = QHBoxLayout()
        self.ports = QComboBox()
        connection.addWidget(QLabel("USB port"))
        connection.addWidget(self.ports)
        self.refresh_button = self._button("Refresh", self.refresh_ports, connection)
        self.connect_button = self._button("Connect", self.toggle_connection, connection)
        self.status = QLabel("Disconnected")
        connection.addWidget(self.status, 1)
        self.language_combo = QComboBox()
        self.language_combo.setAccessibleName("Language / زبان / 语言 / Язык")
        for code, name in LANGUAGES:
            self.language_combo.addItem(name, code)
        self.language_combo.setCurrentIndex(self.language_combo.findData(self.language))
        self.language_combo.currentIndexChanged.connect(
            lambda: self.set_language(self.language_combo.currentData()))
        connection.addWidget(self.language_combo)
        root.addLayout(connection)
        columns = QHBoxLayout()
        self.controls = QWidget()
        layout = QVBoxLayout(self.controls)
        gpio = QGroupBox("Outputs")
        form = QFormLayout(gpio)
        self.pin, self.mode = QComboBox(), QComboBox()
        self.pin.addItems(["12", "14", "15", "2", "21", "22", "25"])
        for mode in ("OFF", "ON", "TIMER", "CYCLE"):
            self.mode.addItem(mode, mode)
        self.work_minutes, self.pause_minutes = QSpinBox(), QSpinBox()
        for field in (self.work_minutes, self.pause_minutes):
            field.setRange(1, 10080)
            field.setSuffix(" min")
        for label, field in (("GPIO", self.pin), ("Mode", self.mode),
                             ("Work duration", self.work_minutes), ("Pause duration", self.pause_minutes)):
            form.addRow(label, field)
        button = QPushButton("Apply output setting")
        button.clicked.connect(self.apply_gpio)
        form.addRow(button)
        self.mode.currentTextChanged.connect(self._update_durations)
        self._update_durations()
        layout.addWidget(gpio)
        sms = QGroupBox("Send one SMS")
        form = QFormLayout(sms)
        self.sms_number, self.sms_text = QLineEdit(), QLineEdit()
        self.sms_number.setPlaceholderText("+12345678900")
        self.sms_text.setMaxLength(70)
        form.addRow("Destination", self.sms_number)
        form.addRow("Text (up to 70 BMP characters)", self.sms_text)
        button = QPushButton("Send SMS")
        button.clicked.connect(self.send_sms)
        form.addRow(button)
        layout.addWidget(sms)
        admin = QGroupBox("Administration")
        form = QFormLayout(admin)
        self.password, self.new_password, self.new_number = QLineEdit(), QLineEdit(), QLineEdit()
        for field in (self.password, self.new_password):
            field.setEchoMode(QLineEdit.EchoMode.Password)
            field.setMaxLength(64)
        form.addRow("Current password", self.password)
        form.addRow("New administrator number", self.new_number)
        button = QPushButton("Change administrator number")
        button.clicked.connect(self.change_number)
        form.addRow(button)
        form.addRow("New password (6–64 characters)", self.new_password)
        button = QPushButton("Change password")
        button.clicked.connect(self.change_password)
        form.addRow(button)
        buttons = QHBoxLayout()
        self._button("All outputs OFF", self.safe_reset, buttons)
        self._button("Clear device logs…", self.clear_logs, buttons)
        form.addRow(buttons)
        layout.addWidget(admin)
        layout.addStretch()
        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setWidget(self.controls)
        columns.addWidget(scroll, 1)
        right = QVBoxLayout()
        actions = QHBoxLayout()
        self.log_page = QSpinBox()
        self.log_page.setRange(1, 10)
        self.log_page.setPrefix("Page ")
        actions.addWidget(self.log_page)
        for label, command in (("Status", "STATUS"), ("SMS log", "SMS_LOG"), ("Call log", "CALL_LOG")):
            self._button(label, lambda checked=False, cmd=command: self.read_status_or_log(cmd), actions)
        self.read_actions = QWidget()
        self.read_actions.setLayout(actions)
        right.addWidget(self.read_actions)
        self.console = QTextEdit()
        self.console.setReadOnly(True)
        self.console.document().setMaximumBlockCount(1000)
        right.addWidget(self.console, 1)
        columns.addLayout(right, 1)
        root.addLayout(columns, 1)

    def log(self, message):
        self.console.moveCursor(QTextCursor.MoveOperation.End)
        self.console.insertPlainText(self.text(message) + "\n")

    def _set_ready(self, ready):
        self._ready = ready
        self.controls.setEnabled(ready)
        self.read_actions.setEnabled(ready)

    def _update_durations(self):
        self.work_minutes.setEnabled(self.mode.currentData() in {"TIMER", "CYCLE"})
        self.pause_minutes.setEnabled(self.mode.currentData() == "CYCLE")

    def refresh_ports(self):
        selected = self.ports.currentData()
        self.ports.clear()
        for port in self._port_provider():
            self.ports.addItem(f"{port.device} — {port.description}", port.device)
        index = self.ports.findData(selected)
        if index >= 0:
            self.ports.setCurrentIndex(index)

    def toggle_connection(self):
        if self.worker is not None:
            self.disconnect()
            return
        port = self.ports.currentData()
        if not port:
            self.log("Select a serial port first.")
            return
        worker = SerialWorker(port, self)
        worker.connected.connect(self._connected)
        worker.line_received.connect(self._line)
        worker.failed.connect(self._serial_failed)
        worker.finished.connect(self._finished)
        self.worker = worker
        self.ports.setEnabled(False)
        self.refresh_button.setEnabled(False)
        self._set_text(self.connect_button, "Disconnect")
        self._set_text(self.status, "Opening port…")
        worker.start()

    def _connected(self):
        self._set_text(self.status, "Checking firmware…")
        try:
            self.worker.submit(b"\n")  # Discard an incomplete frame left by an interrupted connection.
        except Exception as error:
            self._serial_failed(str(error))
            return
        self.request("HELLO", internal=True)

    def disconnect(self):
        self._set_ready(False)
        self._queue.clear()
        self._pending = None
        self.password.clear()
        self.new_password.clear()
        if self.worker is not None:
            self.worker.stop()
            self._set_text(self.status, "Disconnecting…")
            self.connect_button.setEnabled(False)

    def _serial_failed(self, error):
        self.log(error)
        self.disconnect()

    def _finished(self):
        worker, self.worker = self.worker, None
        if worker is not None:
            worker.deleteLater()
        self._set_ready(False)
        self._queue.clear()
        self._pending = None
        self._set_text(self.status, "Disconnected")
        self.ports.setEnabled(True)
        self.refresh_button.setEnabled(True)
        self.connect_button.setEnabled(True)
        self._set_text(self.connect_button, "Connect")
        if self._closing:
            self.close()

    def request(self, command, *arguments, internal=False):
        if self.worker is None or (not self._ready and not internal):
            self.log("Connect to compatible firmware first.")
            return
        try:
            if len(self._queue) >= 16:
                raise ProtocolError("Too many pending commands; wait for the device.")
            frame = encode_request(self._next_id, command, *arguments)
        except (ValueError, UnicodeError) as error:
            self.log(str(error))
            return
        self._queue.append(PendingRequest(self._next_id, command, frame, 65 if command == "SEND_SMS" else 10))
        self._next_id = self._next_id % 2147483647 + 1

    def _tick(self):
        if self._pending is not None:
            if time.monotonic() > self._deadline:
                self.log(self.text("{command}: response timeout; reconnect before retrying.", command=self._pending.command))
                self.disconnect()
            return
        if self.worker is not None and self._queue:
            request = self._queue.popleft()
            try:
                self.worker.submit(request.frame)
            except Exception as error:
                self._serial_failed(str(error))
                return
            self._pending = request
            self._deadline = time.monotonic() + request.timeout
            self.log(f"> {request.command}")

    def _line(self, line):
        try:
            response = parse_response(line)
        except ProtocolError as error:
            self._serial_failed(str(error))
            return
        if response is None:
            self.log(line)
            return
        if response.status == "EVENT":
            self.log(response.payload)
            return
        if self._pending is None or response.request_id != self._pending.request_id:
            self.log("Ignored an unmatched device response.")
            return
        command = self._pending.command
        self._pending = None
        self.log(f"{command}: {response.status}\n{response.payload}")
        if response.status == "ERR":
            self._queue.clear()
            if command == "HELLO":
                self.disconnect()
            return
        if command == "HELLO":
            if not response.payload.startswith("TCall Pager "):
                self._serial_failed("The selected device is not a TCall Pager.")
                return
            self._set_ready(True)
            self._set_text(self.status, response.payload)
            self.request("STATUS")
        if command in {"SET_PASSWORD", "SET_ADMIN", "CLEAR_LOGS", "SAFE_RESET"}:
            self.password.clear()
            self.new_password.clear()

    def apply_gpio(self):
        mode = self.mode.currentData()
        self.request("GPIO", self.pin.currentText(), mode,
                     str(self.work_minutes.value()) if mode in {"TIMER", "CYCLE"} else "0",
                     str(self.pause_minutes.value()) if mode == "CYCLE" else "0")

    def read_status_or_log(self, command):
        if command in {"SMS_LOG", "CALL_LOG"}:
            self.request(command, str((self.log_page.value() - 1) * 5))
        else:
            self.request(command)

    def send_sms(self):
        try:
            number, text = validate_phone(self.sms_number.text()), validate_sms(self.sms_text.text())
        except ProtocolError as error:
            self.log(str(error))
            return
        self.request("SEND_SMS", number, text)

    def change_number(self):
        try:
            number = validate_phone(self.new_number.text())
        except ProtocolError as error:
            self.log(str(error))
            return
        self.request("SET_ADMIN", self.password.text(), number)

    def change_password(self):
        try:
            password = validate_password(self.new_password.text())
        except ProtocolError as error:
            self.log(str(error))
            return
        self.request("SET_PASSWORD", self.password.text(), password)

    def safe_reset(self):
        self.request("SAFE_RESET", self.password.text())

    def clear_logs(self):
        question = "Delete device SMS and call logs? Settings and SIM messages will be preserved."
        dialog = QMessageBox(QMessageBox.Icon.Question, self.text("Clear logs"), self.text(question),
                             QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No, self)
        dialog.button(QMessageBox.StandardButton.Yes).setText(self.text("Yes"))
        dialog.button(QMessageBox.StandardButton.No).setText(self.text("No"))
        dialog.setDefaultButton(QMessageBox.StandardButton.No)
        if dialog.exec() == QMessageBox.StandardButton.Yes:
            self.request("CLEAR_LOGS", self.password.text())

    def text(self, source, **values):
        return translate(source, self.language, **values)

    def _set_text(self, widget, source):
        widget.setProperty("translation_source", source)
        widget.setText(self.text(source))

    def _capture_translations(self):
        # Capture once; translating a translated label would lose its source key.
        self._translated_widgets = []
        for widget in self.findChildren(QWidget):
            if isinstance(widget, (QLabel, QPushButton, QGroupBox)):
                source = widget.title() if isinstance(widget, QGroupBox) else widget.text()
                widget.setProperty("translation_source", source)
                self._translated_widgets.append(widget)

    def set_language(self, language):
        if language not in dict(LANGUAGES):
            return
        self.language = language
        # Keep the language selector in the top-right corner in every locale.
        # Mirror the content area for Persian; serial identifiers stay LTR.
        self.controls.setLayoutDirection(Qt.LayoutDirection.RightToLeft if language == "fa"
                                         else Qt.LayoutDirection.LeftToRight)
        for widget in self._translated_widgets:
            text = self.text(widget.property("translation_source"))
            if isinstance(widget, QGroupBox):
                widget.setTitle(text)
            else:
                widget.setText(text)
        for index in range(self.mode.count()):
            self.mode.setItemText(index, self.text(self.mode.itemData(index)))
        for field in (self.work_minutes, self.pause_minutes):
            field.setSuffix(self.text(" min"))
        self.log_page.setPrefix(self.text("Page "))
        example = {"en": "+12025550123", "ru": "+79991234567",
                   "zh": "+8613812345678", "fa": "+989121234567"}[language]
        self.sms_number.setPlaceholderText(example)
        self.new_number.setPlaceholderText(example)
        for field in (self.ports, self.pin, self.sms_number, self.new_number,
                      self.password, self.new_password, self.console):
            field.setLayoutDirection(Qt.LayoutDirection.LeftToRight)
        index = self.language_combo.findData(language)
        self.language_combo.blockSignals(True)
        self.language_combo.setCurrentIndex(index)
        self.language_combo.blockSignals(False)

    def closeEvent(self, event):
        if self.worker is not None:
            self._closing = True
            self.disconnect()
            event.ignore()
        else:
            event.accept()


def main():
    app = QApplication(sys.argv)
    prepare_application(app)
    settings = QSettings("earendel-dev", "TCallPager")
    window = MainWindow(language=settings.value("language", "en"))
    window.language_combo.currentIndexChanged.connect(
        lambda: settings.setValue("language", window.language))
    window.show()
    result = app.exec()
    window._timer.stop()
    window.deleteLater()
    QCoreApplication.sendPostedEvents(None, QEvent.Type.DeferredDelete)
    return result


if __name__ == "__main__":
    raise SystemExit(main())
