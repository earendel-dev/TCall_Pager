# TCall Pager 1.1.0-rc.1

This release candidate introduces a versioned USB protocol and a matching
desktop controller. The previous v1.0.0 release and its assets remain available.

## Main changes

- Explicit GPIO commands with validated ON/OFF/timer/cycle semantics.
- Request IDs and acknowledgements replace timed serial-menu macros.
- Nonblocking modem startup and queued AT transactions; unsolicited SMS/call
  notifications are handled separately from command responses.
- Log cleanup preserves administrator settings, output configuration and SIM messages.
- New password KDF, hardware-generated salts, atomic settings writes and lockout.
- Native PySide6 desktop, masked password fields, bounded serial/console buffers,
  and queue cancellation on errors, disconnects and uncertain timeouts.
- English README/comments, protocol documentation, build scripts and offline tests.

## Compatibility

Use the firmware and desktop client from the same release. The candidate GUI
refuses to enable controls when an older firmware does not answer its handshake.
Valid legacy credentials can be imported. Reboots cancel timers/cycles and restore
only constant-ON channels. Full phone-number matching replaces suffix matching.

## Verification and limits

24 offline tests pass using native Qt. The frozen desktop self-check exits 0.
The firmware also compiles and links in GitHub Actions using ESP32 core 3.3.8.
The release includes the resulting firmware binaries. Hardware acceptance tests
remain required before stable use; see [validation notes](https://github.com/earendel-dev/TCall_Pager/blob/v1.1.0-rc.1/docs/VALIDATION.md).

No device was flashed during local preparation because COM7 could not be opened.
Do not infer recipient SMS delivery from a modem submission acknowledgement.
Do not use this candidate as a hardware safety controller.
