"""Bounded USB frames: UTF-8 hex fields cannot inject transport delimiters."""
from dataclasses import dataclass
import re

VERSION = "1.1.0-rc.1"
PREFIX = "TCALL/1"
MAX_LINE_BYTES = 4096
MAX_ARGUMENT_BYTES = 512
MAX_TIMER_MINUTES = 10080


class ProtocolError(ValueError):
    """The frame or command violates the protocol contract."""


def encode_request(request_id: int, command: str, *arguments: str) -> bytes:
    if not 1 <= request_id <= 2147483647 or not re.fullmatch(r"[A-Z_]{1,24}", command):
        raise ProtocolError("Invalid request identifier or command.")
    if len(arguments) > 5:
        raise ProtocolError("Too many arguments.")
    fields = [PREFIX, str(request_id), command]
    for argument in arguments:
        encoded = argument.encode("utf-8")
        if len(encoded) > MAX_ARGUMENT_BYTES or b"\x00" in encoded:
            raise ProtocolError("Argument is too long or contains a NUL character.")
        fields.append(encoded.hex().upper())
    frame = "\t".join(fields).encode("ascii") + b"\n"
    if len(frame) > MAX_LINE_BYTES:
        raise ProtocolError("Frame is too long.")
    return frame


@dataclass(frozen=True)
class Response:
    request_id: int
    status: str
    payload: str


def parse_response(line: str) -> Response | None:
    if len(line.encode("utf-8")) > MAX_LINE_BYTES:
        raise ProtocolError("Response frame is too long.")
    if not line.startswith(PREFIX + "\t"):
        return None
    fields = line.rstrip("\r\n").split("\t")
    if len(fields) != 4 or not re.fullmatch(r"[0-9]{1,10}", fields[1]):
        raise ProtocolError("Malformed response frame.")
    request_id = int(fields[1])
    if request_id > 2147483647 or fields[2] not in {"OK", "ERR", "EVENT"}:
        raise ProtocolError("Invalid response identifier or status.")
    if len(fields[3]) % 2 or not re.fullmatch(r"[0-9A-Fa-f]*", fields[3]):
        raise ProtocolError("Malformed response payload.")
    try:
        payload = bytes.fromhex(fields[3]).decode("utf-8")
    except (ValueError, UnicodeDecodeError) as error:
        raise ProtocolError("Invalid response text.") from error
    return Response(request_id, fields[2], payload)


def validate_phone(number: str) -> str:
    number = number.strip()
    if not re.fullmatch(r"\+?[0-9]{10,15}", number, re.ASCII):
        raise ProtocolError("Use 10–15 digits, optionally starting with +.")
    return number


def validate_minutes(value: str) -> str:
    value = value.strip()
    if not re.fullmatch(r"[0-9]{1,5}", value) or not 1 <= int(value) <= MAX_TIMER_MINUTES:
        raise ProtocolError("Duration must be 1–10080 whole minutes.")
    return str(int(value))


def validate_sms(text: str) -> str:
    if not text or len(text) > 70 or any(ord(c) == 0 or ord(c) > 65535 or 0xD800 <= ord(c) <= 0xDFFF for c in text):
        raise ProtocolError("SMS must contain 1–70 BMP characters; emoji are not supported.")
    return text


def validate_password(text: str) -> str:
    if not 6 <= len(text) <= 64 or any(ord(c) == 0 or ord(c) > 65535 or 0xD800 <= ord(c) <= 0xDFFF for c in text):
        raise ProtocolError("Password must contain 6–64 BMP characters.")
    return text
