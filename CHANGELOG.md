# Changelog

## 1.1.0-rc.2

- Add live English, Russian, Chinese and Persian desktop language selection.
- Persist the selected language and support right-to-left Persian forms.
- Preserve machine protocol tokens when output-mode labels are translated.
- Document Arduino IDE 2.3+ and automatic entry into upload mode.


## 1.1.0-rc.1

This release candidate changes the USB protocol. Use the desktop application
and firmware from the same release. The existing v1.0.0 release remains available.

### Changed

- Replace timed menu macros with request IDs, acknowledgements and explicit errors.
- Run modem initialization and AT transactions as bounded state machines.
- Use one serial worker with read/write timeouts and no intentional reset pulse.
- Switch the desktop application to PySide6 and bound console history.
- Separate log cleanup from credential and SIM storage management.
- Validate output modes, channel numbers, phone numbers, text and timer ranges.
- Preserve original credentials when migrating a valid legacy installation.
- Store new passwords using PBKDF2-HMAC-SHA256 with hardware-generated salts.
- Add offline protocol/GUI tests, frozen smoke checks and build-only CI jobs.
- Preserve the event LED and expose all 50 retained log entries through bounded pages.

### Safety and compatibility

- No firmware upload or eFuse modification is performed by build scripts.
- No SIM-wide deletion occurs on startup or after a call.
- First-time filesystem initialization requires an explicit USB request while
  the physical BOOT button is held down; a mounted filesystem cannot be formatted.
- A reboot restores constant-ON channels and cancels timers/cycles.
- SMS submission acknowledgement does not prove recipient delivery.
- Single-part BMP SMS only: up to 70 characters; emoji and concatenation are unsupported.

See [validation status](docs/VALIDATION.md) before using this candidate with loads.
