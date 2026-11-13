# USB protocol version 1

Transport: 115200 baud, 8N1, UTF-8/ASCII lines terminated by LF. CR before LF is
ignored. The desktop application does not intentionally reset the board.
On a new connection, clients send a single LF before HELLO to discard a
partially received frame from an earlier interrupted connection.

Request:

```text
TCALL/1<TAB>request_id<TAB>COMMAND[<TAB>HEX_ARGUMENT...]
```

Response:

```text
TCALL/1<TAB>request_id<TAB>OK|ERR<TAB>HEX_UTF8_PAYLOAD
```

Events use ID `0` and status `EVENT`. IDs `1..2147483647` identify requests.
Arguments are hex-encoded UTF-8, including passwords. Hex encoding provides
framing, not encryption. USB access is a trusted local control interface.
Maximum line length is 4096 bytes; at most five arguments, each up to 512 bytes.
The desktop submits one request at a time and never retries a mutating request
after an uncertain timeout. A matching response is required to report success.

| Command | Arguments | Result |
|---|---|---|
| `HELLO` | none | Device name and firmware version |
| `STATUS` | none | Storage, modem/network status and output states |
| `GPIO` | pin, mode, work minutes, pause minutes | Persist and apply an output setting |
| `SEND_SMS` | number, text | Submit one SMS; network delivery is not confirmed |
| `SMS_LOG` | none | Latest ten retained SMS log entries |
| `CALL_LOG` | none | Latest ten retained call log entries |
| `SET_ADMIN` | current password, new number | Persist administrator number |
| `SET_PASSWORD` | current password, new password | Persist a new salted password verifier |
| `SAFE_RESET` | current password | Turn all outputs OFF and save the state |
| `CLEAR_LOGS` | current password | Remove two log files, preserving settings and SIM |
| `INIT_STORAGE` | none | Format only an unmounted filesystem, with physical BOOT held |

Modes are `OFF`, `ON`, `TIMER`, `CYCLE`. OFF/ON require both durations to be zero.
TIMER requires work `1..10080` and pause `0`. CYCLE requires both `1..10080`.
Supported pins: 12, 14, 15, 2, 21, 22, 25.

Example, channel 12 ON:

```text
TCALL/1<TAB>2<TAB>GPIO<TAB>3132<TAB>4F4E<TAB>30<TAB>30
```

Five failed password attempts lock administrative authentication for 30 seconds.
Phone numbers contain 10–15 digits with an optional leading plus. Administrator
matching compares the full digits; suffix matching is deliberately unsupported.

The old numbered serial menu is not part of this protocol. Use the matching GUI
or `tools/pager_cli.py` for read-only operations and first-time provisioning.
