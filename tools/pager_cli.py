"""Send one protocol request without resetting or flashing the board."""
import argparse
from pathlib import Path
import secrets
import sys
import time
import serial

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "GUI"))
from protocol import encode_request, parse_response


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("command", choices=["HELLO", "STATUS", "SMS_LOG", "CALL_LOG", "INIT_STORAGE"])
    parser.add_argument("--page", type=int, choices=range(1, 11), default=1, help="Log page, 1–10")
    args = parser.parse_args()
    request_id = secrets.randbelow(1000000) + 1
    port = serial.Serial(port=None, baudrate=115200, timeout=.2, write_timeout=1)
    port.dtr = port.rts = False
    port.port = args.port
    try:
        port.open()
        port.write(b"\n")
        fields = (str((args.page - 1) * 5),) if args.command in {"SMS_LOG", "CALL_LOG"} else ()
        port.write(encode_request(request_id, args.command, *fields))
        deadline = time.monotonic() + 10
        pending = bytearray()
        while time.monotonic() < deadline:
            pending.extend(port.read(min(max(port.in_waiting, 1), 1024)))
            if len(pending) > 4096 and b"\n" not in pending:
                raise ValueError("The device response is too long.")
            while b"\n" in pending:
                line, _, remainder = pending.partition(b"\n")
                pending = bytearray(remainder)
                response = parse_response(line.decode("utf-8", "replace"))
                if response and response.request_id == request_id:
                    print(response.payload)
                    return 0 if response.status == "OK" else 1
        print("No compatible response. Confirm the port and firmware version.", file=sys.stderr)
        return 1
    finally:
        port.close()


if __name__ == "__main__":
    raise SystemExit(main())
