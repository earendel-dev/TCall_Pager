# Release candidate validation

## Scope

The baseline reviewed was commit `626fc1e` (v1.0.0). The device was not flashed,
reset intentionally, or sent output/SMS/cleanup commands during this work.
The existing release and its assets must remain unchanged.

## Local checks

- Python parsing, native PySide6 GUI construction and protocol/GUI/worker tests.
- Offline packaging smoke check: protocol round trip and disabled controls before
  a device handshake. This check does not open a physical serial port.
- Desktop packaging with the installed Python 3.14 / PySide6 6.12 toolchain.
- 24 offline tests pass; the packaged console twin completes its offline
  self-check with exit code 0. No compatibility adapter is used for candidate tests.
- The installed GCC C++ frontend completes semantic checking of the candidate
  with real ESP32 core 3.3.8 headers. The only diagnostic is a toolchain-header
  `_POSIX_READER_WRITER_LOCKS` redefinition warning.

## Continuous integration

The firmware was fully compiled and linked on GitHub Actions with pinned ESP32
core 3.3.8 and the ESP32 Dev Module profile with PSRAM enabled. The build-only job
produced application, bootloader and partition binaries without contacting hardware.
Desktop CI additionally runs the offline tests and the frozen packaging smoke check.

## Hardware and firmware limits

Opening COM7 was rejected by Windows (access denied). This may mean another
application owns the port or the execution environment restricts it. Hardware
identity, output levels and GSM/SMS operation have therefore not been verified.

The installed ESP32 GCC tools fail to launch in the execution environment with
Windows error 623 (DLL relocation). This is not a compiler diagnostic about the
source. The separate C++ frontend runs successfully; the complete linked image
was produced by CI instead of the local compiler. Successful compilation does
not replace the hardware acceptance checks below.

Treat this as a release candidate, not a validated 24/7 controller. Do not infer
firmware correctness from passing desktop tests.

## Required acceptance checks

1. Compile using pinned ESP32 core 3.3.8 and inspect warnings and flash/RAM usage.
2. After a separate authorization to flash, back up the existing flash/settings,
   then test the candidate with relay loads disconnected.
3. Verify HELLO/STATUS, every output mode, wrong passwords, invalid durations,
   disconnect/reconnect and legacy-credential migration.
4. Verify timers while SMS sending times out, while the network is absent, and
   during modem resets. These are soft timers; filesystem/cryptographic operations
   can introduce scheduling jitter. Use hardware interlocks for safety functions.
5. Verify incoming SMS/calls during AT responses, SIM-full recovery and retention
   when logging fails. Session duplicate protection is not an exactly-once
   guarantee across a power failure.
6. Power-cycle constant outputs and timers; confirm the documented restart policy.
7. Run a 24-hour soak test and a simulated `millis()` rollover test before calling
   the release stable.
