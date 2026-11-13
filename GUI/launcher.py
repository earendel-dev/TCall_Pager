"""Frozen application entry point and an offline packaging smoke check."""
import os
import sys


def self_check():
    print("Starting offline desktop self-check", flush=True)
    os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
    from PySide6.QtWidgets import QApplication
    print("Qt runtime loaded", flush=True)
    from gui import MainWindow, prepare_application
    from protocol import encode_request, parse_response
    print("Application modules loaded", flush=True)
    assert encode_request(1, "HELLO") == b"TCALL/1\t1\tHELLO\n"
    assert parse_response("TCALL/1\t1\tOK\t68656C6C6F").payload == "hello"
    application = QApplication.instance() or QApplication([])
    prepare_application(application)
    from PySide6.QtGui import QFontMetrics
    assert QFontMetrics(application.font()).inFontUcs4(ord("A"))
    print("Qt application initialized", flush=True)
    window = MainWindow(port_provider=lambda: [])
    window.show()
    application.processEvents()
    assert not window.controls.isEnabled()
    window.close()
    window._timer.stop()
    window.deleteLater()
    from PySide6.QtCore import QCoreApplication, QEvent
    QCoreApplication.sendPostedEvents(None, QEvent.Type.DeferredDelete)
    application.processEvents()
    print("SELF-CHECK PASSED: protocol, native Qt window, disconnected controls")
    return 0


if __name__ == "__main__":
    if "--self-check" in sys.argv:
        raise SystemExit(self_check())
    from gui import main
    raise SystemExit(main())
