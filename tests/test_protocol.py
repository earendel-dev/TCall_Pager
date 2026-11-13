"""Protocol boundary tests; no physical ports are opened."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "GUI"))
from protocol import ProtocolError, encode_request, parse_response, validate_minutes, validate_phone, validate_sms, validate_password


class ProtocolTests(unittest.TestCase):
    def test_hello_wire_format(self):
        self.assertEqual(encode_request(1, "HELLO"), b"TCALL/1\t1\tHELLO\n")

    def test_user_text_cannot_inject_frame_delimiters(self):
        frame = encode_request(2, "SEND_SMS", "+12345678900", "line\nTCALL/1\t5\tCLEAR_LOGS")
        self.assertEqual(frame.count(b"\n"), 1)
        self.assertEqual(len(frame.rstrip(b"\n").split(b"\t")), 5)

    def test_unicode_response(self):
        text = "Message: Привет <b>data</b>"
        response = parse_response("TCALL/1\t12\tOK\t" + text.encode().hex())
        self.assertEqual((response.request_id, response.status, response.payload), (12, "OK", text))

    def test_unframed_boot_message(self):
        self.assertIsNone(parse_response("ESP32 boot diagnostic"))

    def test_malformed_responses(self):
        for line in ("TCALL/1\t-1\tOK\t00", "TCALL/1\t1\tBAD\t", "TCALL/1\t1\tOK\t0",
                     "TCALL/1\t1\tOK\tGG", "TCALL/1\t1\tOK\tFF", "TCALL/1\t1\tOK\t20\textra"):
            with self.subTest(line=line), self.assertRaises(ProtocolError):
                parse_response(line)

    def test_request_limits(self):
        for arguments in ((0, "HELLO"), (1, "HELLO\n"), (1, "SMS", "x" * 513), (2147483648, "HELLO")):
            with self.subTest(arguments=arguments[:2]), self.assertRaises(ProtocolError):
                encode_request(*arguments)

    def test_timer_range(self):
        self.assertEqual(validate_minutes("10080"), "10080")
        for value in ("0", "-1", "10081", "1.5", "1x", "١"):
            with self.subTest(value=value), self.assertRaises(ProtocolError):
                validate_minutes(value)

    def test_phone_validation(self):
        self.assertEqual(validate_phone("+12345678900"), "+12345678900")
        for value in ("+7", "", "+1234567890\nAT", "+1234567890123456", "+１２３４５６７８９０"):
            with self.subTest(value=value), self.assertRaises(ProtocolError):
                validate_phone(value)

    def test_sms_zero_is_data(self):
        self.assertEqual(validate_sms("0"), "0")

    def test_sms_limits(self):
        self.assertEqual(validate_sms("Я" * 70), "Я" * 70)
        for text in ("", "A" * 71, "😀", "\ud800", "\x00"):
            with self.subTest(length=len(text)), self.assertRaises(ProtocolError):
                validate_sms(text)

    def test_password_limits(self):
        self.assertEqual(validate_password("пароль123"), "пароль123")
        for text in ("short", "x" * 65, "😀" * 6, "abcdef\x00"):
            with self.subTest(length=len(text)), self.assertRaises(ProtocolError):
                validate_password(text)


if __name__ == "__main__":
    unittest.main()
