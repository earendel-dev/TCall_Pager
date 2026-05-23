import sys
import subprocess
import time  # Необходим для аппаратного сброса ESP32

# =====================================================================
# БЛОК АВТОМАТИЧЕСКОЙ УСТАНОВКИ ЗАВИСИМОСТЕЙ
# =====================================================================
try:
    import serial
    import serial.tools.list_ports
    from PyQt6.QtWidgets import QApplication, QMainWindow, QWidget, QVBoxLayout, \
        QHBoxLayout, QComboBox, QPushButton, QTextEdit, \
        QLineEdit, QLabel, QGroupBox, QFormLayout, QMessageBox, \
        QSplitter, QScrollArea
    from PyQt6.QtCore import QThread, pyqtSignal, QTimer, Qt
except ModuleNotFoundError:
    print("Библиотеки не найдены. Запускаю автоматическую установку в .venv...")
    subprocess.check_call([sys.executable, "-m", "pip", "install", "pyserial", "PyQt6"])
    print("Установка завершена успешно! Перезапустите скрипт.")
    sys.exit(0)

# =====================================================================
# СОВРЕМЕННЫЙ СТИЛЬ (QSS)
# =====================================================================
MODERN_STYLE = """
QWidget {
    background-color: #2b2d30;
    color: #dfe1e5;
    font-family: 'Segoe UI', 'San Francisco', Arial, sans-serif;
    font-size: 13px;
}
QGroupBox {
    border: 1px solid #43454a;
    border-radius: 6px;
    margin-top: 12px;
    padding-top: 10px;
    font-weight: bold;
    color: #8bb9ff;
}
QGroupBox::title {
    subcontrol-origin: margin;
    left: 10px;
    padding: 0 5px;
}
QPushButton {
    background-color: #43454a;
    border: 1px solid #55575c;
    border-radius: 5px;
    padding: 6px 12px;
    color: #ffffff;
    font-weight: 500;
}
QPushButton:hover {
    background-color: #4e5157;
    border: 1px solid #66686d;
}
QPushButton:pressed {
    background-color: #393b40;
}
QLineEdit, QComboBox {
    background-color: #1e1f22;
    border: 1px solid #43454a;
    border-radius: 4px;
    padding: 5px;
    color: #dfe1e5;
}
QLineEdit:focus, QComboBox:focus {
    border: 1px solid #8bb9ff;
}
QTextEdit {
    background-color: #1e1f22;
    border: 1px solid #43454a;
    border-radius: 4px;
    padding: 5px;
    color: #a5d6a7; /* Светло-зеленый текст консоли */
    font-family: 'Consolas', 'Courier New', monospace;
    font-size: 13px;
}
/* Специальные классы кнопок */
QPushButton#primary_btn {
    background-color: #3574f0;
    border: 1px solid #3574f0;
}
QPushButton#primary_btn:hover {
    background-color: #4682f2;
}
QPushButton#danger_btn {
    background-color: #c94f4f;
    border: 1px solid #c94f4f;
    color: white;
}
QPushButton#danger_btn:hover {
    background-color: #d65c5c;
}
QSplitter::handle {
    background-color: #43454a;
    margin: 2px;
}
QScrollArea {
    border: none;
}
"""

# =====================================================================
# КЛАСС РАБОТЫ С COM-ПОРТОМ
# =====================================================================
class SerialWorker(QThread):
    data_received = pyqtSignal(str)
    error_occurred = pyqtSignal(str)

    def __init__(self):
        super().__init__()
        self.serial_port = None
        self.running = False

    def connect_port(self, port, baudrate):
        try:
            # Создаем объект порта, но пока не открываем
            self.serial_port = serial.Serial()
            self.serial_port.port = port
            self.serial_port.baudrate = baudrate
            self.serial_port.timeout = 0.1

            # Открываем порт
            self.serial_port.open()

            # --- АППАРАТНЫЙ СБРОС ESP32 ---
            # Принудительно перезагружаем плату и выводим из режима прошивки
            self.serial_port.setDTR(False)
            self.serial_port.setRTS(True)
            time.sleep(0.1) # Короткая пауза для сброса
            self.serial_port.setDTR(False)
            self.serial_port.setRTS(False)
            # ------------------------------

            self.running = True
            self.start()
            return True
        except Exception as e:
            self.error_occurred.emit(str(e))
            return False

    def disconnect_port(self):
        self.running = False
        self.wait()
        if self.serial_port and self.serial_port.is_open:
            self.serial_port.close()

    def run(self):
        while self.running and self.serial_port and self.serial_port.is_open:
            try:
                if self.serial_port.in_waiting:
                    data = self.serial_port.read(self.serial_port.in_waiting).decode('utf-8', errors='replace')
                    if data:
                        self.data_received.emit(data)
            except Exception as e:
                self.error_occurred.emit(f"Ошибка чтения: {str(e)}")
                self.running = False

    def send_data(self, data):
        if self.serial_port and self.serial_port.is_open:
            try:
                self.serial_port.write((data + '\n').encode('utf-8'))
            except Exception as e:
                self.error_occurred.emit(f"Ошибка записи: {str(e)}")

# =====================================================================
# ГЛАВНОЕ ОКНО ПРИЛОЖЕНИЯ
# =====================================================================
class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("TCall Pager Controller - ESP32 PRO")
        self.resize(1050, 800)
        self.setStyleSheet(MODERN_STYLE)

        self.worker = SerialWorker()
        self.worker.data_received.connect(self.append_console)
        self.worker.error_occurred.connect(self.show_error)

        self.command_queue = []
        self.queue_timer = QTimer()
        self.queue_timer.timeout.connect(self.process_queue)

        self.init_ui()

    def init_ui(self):
        main_widget = QWidget()
        self.setCentralWidget(main_widget)
        main_layout = QVBoxLayout(main_widget)
        main_layout.setContentsMargins(15, 15, 15, 15)
        main_layout.setSpacing(10)

        # Верхняя панель подключения
        conn_layout = QHBoxLayout()
        self.port_combo = QComboBox()
        self.port_combo.setMinimumWidth(150)
        self.refresh_ports()

        self.btn_refresh = QPushButton("🔄 Обновить")
        self.btn_refresh.clicked.connect(self.refresh_ports)

        self.btn_connect = QPushButton("Подключить")
        self.btn_connect.setObjectName("primary_btn")
        self.btn_connect.clicked.connect(self.toggle_connection)

        conn_layout.addWidget(QLabel("COM Порт:"))
        conn_layout.addWidget(self.port_combo)
        conn_layout.addWidget(self.btn_refresh)
        conn_layout.addWidget(self.btn_connect)
        conn_layout.addStretch()
        main_layout.addLayout(conn_layout)

        splitter = QSplitter(Qt.Orientation.Horizontal)
        main_layout.addWidget(splitter)

        # Левая часть - Настройки
        control_scroll = QScrollArea()
        control_scroll.setWidgetResizable(True)
        control_widget = QWidget()
        control_layout = QVBoxLayout(control_widget)
        control_layout.setContentsMargins(0, 0, 10, 0)
        control_layout.setSpacing(15)

        # 1. Логи и память
        log_group = QGroupBox("📋 Логи и память")
        log_layout = QVBoxLayout()
        log_layout.setSpacing(8)

        btn_sms_log = QPushButton("Читать лог SMS (1)")
        btn_sms_log.clicked.connect(lambda: self.run_macro(['0', '1']))

        btn_call_log = QPushButton("Читать историю звонков (2)")
        btn_call_log.clicked.connect(lambda: self.run_macro(['0', '2']))

        btn_wipe = QPushButton("⚠️ Форматировать LittleFS и очистить SIM (5)")
        btn_wipe.setObjectName("danger_btn")
        btn_wipe.clicked.connect(lambda: self.run_macro(['0', '5']))

        log_layout.addWidget(btn_sms_log)
        log_layout.addWidget(btn_call_log)
        log_layout.addWidget(btn_wipe)
        log_group.setLayout(log_layout)
        control_layout.addWidget(log_group)

        # 2. Управление GPIO
        gpio_group = QGroupBox("⚡ Управление GPIO")
        gpio_layout = QFormLayout()
        gpio_layout.setVerticalSpacing(10)

        btn_pin_status = QPushButton("📊 Запросить состояние пинов (4)")
        btn_pin_status.clicked.connect(lambda: self.run_macro(['0', '4']))

        self.pin_combo = QComboBox()
        self.pin_combo.addItems(["12", "14", "15", "2", "21", "22", "25"])

        self.gpio_action_combo = QComboBox()
        self.gpio_action_combo.addItems([
            "Включить постоянно (ON)",
            "Выключить (OFF)",
            "Включить на время (Таймер)",
            "Бесконечный цикл (Работа / Пауза)"
        ])

        self.in_gpio_t1 = QLineEdit()
        self.in_gpio_t1.setPlaceholderText("Минуты (например: 5)")
        self.in_gpio_t1.setEnabled(False)

        self.in_gpio_t2 = QLineEdit()
        self.in_gpio_t2.setPlaceholderText("Минуты (например: 10)")
        self.in_gpio_t2.setEnabled(False)

        self.gpio_action_combo.currentIndexChanged.connect(self.update_gpio_ui)

        btn_gpio_send = QPushButton("▶ Отправить команду GPIO")
        btn_gpio_send.setObjectName("primary_btn")
        btn_gpio_send.clicked.connect(self.cmd_control_gpio)

        gpio_layout.addRow("", btn_pin_status)
        gpio_layout.addRow(QLabel("")) # Отступ
        gpio_layout.addRow("ПИН (PIN):", self.pin_combo)
        gpio_layout.addRow("Режим работы:", self.gpio_action_combo)
        gpio_layout.addRow("Время работы (мин):", self.in_gpio_t1)
        gpio_layout.addRow("Время паузы (мин):", self.in_gpio_t2)
        gpio_layout.addRow("", btn_gpio_send)
        gpio_group.setLayout(gpio_layout)
        control_layout.addWidget(gpio_group)

        # 3. SMS
        sms_group = QGroupBox("✉️ Отправить SMS (3)")
        sms_layout = QFormLayout()
        self.in_sms_num = QLineEdit("+7")
        self.in_sms_msg = QLineEdit()
        btn_send_sms = QPushButton("Отправить SMS")
        btn_send_sms.setObjectName("primary_btn")
        btn_send_sms.clicked.connect(self.cmd_send_sms)

        sms_layout.addRow("Номер:", self.in_sms_num)
        sms_layout.addRow("Текст:", self.in_sms_msg)
        sms_layout.addRow("", btn_send_sms)
        sms_group.setLayout(sms_layout)
        control_layout.addWidget(sms_group)

        # 4. Настройки Админа
        admin_group = QGroupBox("⚙️ Настройки Администратора (6)")
        admin_layout = QFormLayout()
        admin_layout.setVerticalSpacing(10)

        self.in_admin_pass = QLineEdit()
        self.in_admin_pass.setEchoMode(QLineEdit.EchoMode.Password)
        self.in_admin_pass.setPlaceholderText("Текущий пароль для подтверждения")

        self.in_new_num = QLineEdit("+7")
        btn_set_num = QPushButton("Сохранить новый номер")
        btn_set_num.clicked.connect(self.cmd_change_admin_num)

        self.in_new_pass = QLineEdit()
        btn_set_pass = QPushButton("Сохранить новый пароль")
        btn_set_pass.clicked.connect(self.cmd_change_admin_pass)

        btn_safe_reset = QPushButton("🛑 SAFE RESET (Аварийное откл. всех PIN)")
        btn_safe_reset.setObjectName("danger_btn")
        btn_safe_reset.clicked.connect(self.cmd_safe_reset)

        admin_layout.addRow("Ваш пароль:", self.in_admin_pass)
        admin_layout.addRow(QLabel("")) # Отступ
        admin_layout.addRow("Новый номер:", self.in_new_num)
        admin_layout.addRow("", btn_set_num)
        admin_layout.addRow("Новый пароль:", self.in_new_pass)
        admin_layout.addRow("", btn_set_pass)
        admin_layout.addRow(QLabel("")) # Отступ
        admin_layout.addRow("", btn_safe_reset)
        admin_group.setLayout(admin_layout)
        control_layout.addWidget(admin_group)

        control_layout.addStretch()
        control_scroll.setWidget(control_widget)
        splitter.addWidget(control_scroll)

        # Правая часть - Терминал
        console_widget = QWidget()
        console_layout = QVBoxLayout(console_widget)
        console_layout.setContentsMargins(10, 0, 0, 0)

        self.console = QTextEdit()
        self.console.setReadOnly(True)

        input_layout = QHBoxLayout()
        self.in_console = QLineEdit()
        self.in_console.setPlaceholderText("Введите команду вручную и нажмите Enter...")
        self.in_console.returnPressed.connect(self.send_manual_cmd)

        btn_manual_send = QPushButton("Отправить")
        btn_manual_send.clicked.connect(self.send_manual_cmd)

        btn_clear = QPushButton("Очистить лог")
        btn_clear.clicked.connect(self.console.clear)

        input_layout.addWidget(self.in_console)
        input_layout.addWidget(btn_manual_send)
        input_layout.addWidget(btn_clear)

        lbl_terminal = QLabel("💻 Встроенный монитор порта:")
        lbl_terminal.setStyleSheet("font-weight: bold; color: #8bb9ff; padding-bottom: 5px;")

        console_layout.addWidget(lbl_terminal)
        console_layout.addWidget(self.console)
        console_layout.addLayout(input_layout)

        splitter.addWidget(console_widget)
        splitter.setSizes([500, 500])

    # --- ЛОГИКА ИНТЕРФЕЙСА GPIO ---
    def update_gpio_ui(self, index):
        self.in_gpio_t1.setEnabled(index >= 2)
        self.in_gpio_t2.setEnabled(index == 3)
        if index < 2:
            self.in_gpio_t1.clear()
        if index < 3:
            self.in_gpio_t2.clear()

    def cmd_control_gpio(self):
        if not self.worker.running:
            self.show_error("Сначала подключитесь к COM-порту!")
            return

        pin = self.pin_combo.currentText()
        idx = self.gpio_action_combo.currentIndex()

        if idx == 0:
            cmd = f"PIN {pin} ON"
        elif idx == 1:
            cmd = f"PIN {pin} OFF"
        elif idx == 2:
            t1 = self.in_gpio_t1.text().strip()
            if not t1.isdigit():
                self.show_error("Укажите корректное время работы в минутах (только цифры)!")
                return
            cmd = f"PIN {pin} ON {t1}"
        elif idx == 3:
            t1 = self.in_gpio_t1.text().strip()
            t2 = self.in_gpio_t2.text().strip()
            if not t1.isdigit() or not t2.isdigit():
                self.show_error("Укажите корректное время работы и паузы в минутах (только цифры)!")
                return
            cmd = f"PIN {pin} ON {t1} {t2}"

        self.worker.send_data(cmd)
        self.append_console(f"\n[GUI] Отправлена команда: {cmd}\n")

    # --- ОСТАЛЬНАЯ ЛОГИКА ---
    def refresh_ports(self):
        self.port_combo.clear()
        ports = serial.tools.list_ports.comports()
        for p in ports:
            self.port_combo.addItem(p.device)

    def toggle_connection(self):
        if self.worker.running:
            self.worker.disconnect_port()
            self.btn_connect.setText("Подключить")
            self.btn_connect.setObjectName("primary_btn")
            self.btn_connect.setStyleSheet("") # Сброс кастомного стиля
            self.port_combo.setEnabled(True)
            self.btn_refresh.setEnabled(True)
            self.append_console("\n--- ОТКЛЮЧЕНО ---\n")
        else:
            port = self.port_combo.currentText()
            if not port:
                self.show_error("COM-порт не найден!")
                return
            if self.worker.connect_port(port, 115200):
                self.btn_connect.setText("Отключить")
                self.btn_connect.setObjectName("danger_btn")
                self.btn_connect.setStyleSheet("")
                self.port_combo.setEnabled(False)
                self.btn_refresh.setEnabled(False)
                self.append_console(f"\n--- ПОДКЛЮЧЕНО К {port} ---\n")

    def append_console(self, text):
        scrollbar = self.console.verticalScrollBar()
        at_bottom = scrollbar.value() == scrollbar.maximum()
        self.console.insertPlainText(text)
        if at_bottom:
            scrollbar.setValue(scrollbar.maximum())

    def show_error(self, message):
        QMessageBox.warning(self, "Внимание", message)

    def send_manual_cmd(self):
        text = self.in_console.text()
        if text:
            self.worker.send_data(text)
            self.in_console.clear()

    def run_macro(self, commands):
        if not self.worker.running:
            self.show_error("Сначала подключитесь к COM-порту!")
            return
        self.command_queue.extend(commands)
        if not self.queue_timer.isActive():
            self.queue_timer.start(300)

    def process_queue(self):
        if self.command_queue:
            cmd = self.command_queue.pop(0)
            self.worker.send_data(cmd)
        else:
            self.queue_timer.stop()

    def cmd_send_sms(self):
        num = self.in_sms_num.text().strip()
        msg = self.in_sms_msg.text().strip()
        if not num or not msg:
            self.show_error("Заполните номер телефона и текст сообщения!")
            return
        self.run_macro(['0', '3', num, msg])

    def cmd_change_admin_num(self):
        pwd = self.in_admin_pass.text().strip()
        new_num = self.in_new_num.text().strip()
        if not pwd or not new_num:
            self.show_error("Для смены номера укажите ваш текущий пароль и новый номер.")
            return
        self.run_macro(['0', '6', pwd, '1', new_num, '0'])

    def cmd_change_admin_pass(self):
        pwd = self.in_admin_pass.text().strip()
        new_pwd = self.in_new_pass.text().strip()
        if not pwd or not new_pwd:
            self.show_error("Для смены пароля укажите текущий и новый пароль.")
            return
        self.run_macro(['0', '6', pwd, '2', new_pwd, '0'])

    def cmd_safe_reset(self):
        pwd = self.in_admin_pass.text().strip()
        if not pwd:
            self.show_error("Операция требует подтверждения! Введите текущий пароль.")
            return
        self.run_macro(['0', '6', pwd, '3', '0'])

    def closeEvent(self, event):
        self.worker.disconnect_port()
        event.accept()

if __name__ == "__main__":
    app = QApplication(sys.argv)
    window = MainWindow()
    window.show()
    sys.exit(app.exec())