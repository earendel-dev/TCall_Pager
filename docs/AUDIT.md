# Baseline audit and corrective work

Reviewed baseline: `626fc1e`, 2026-11-12. Scores describe the baseline and are
engineering judgments on a 1–10 scale, not certification.

| Area | Score | Evidence |
|---|---:|---|
| Structure and maintainability | 4 | UI, transport and command macros share one class; modem transactions and unsolicited events share blocking reads |
| Functional consistency | 3 | GUI sends `PIN 12 ON` over USB, but the USB GPIO menu accepts only numeric toggle input |
| Long-running reliability | 3 | Busy polling, blocking modem initialization, unbounded network waits and timed USB macros |
| Safety and access control | 3 | Unauthenticated menu item formats LittleFS, resetting administrator credentials; weak input validation |
| Testing and builds | 2 | No committed tests, pinned board version or automated builds |
| Documentation and repository | 5 | Readable overview and clean tracked artifacts, but several behavioral claims do not match the code |

## Confirmed baseline findings

| Priority | Finding | Candidate treatment |
|---|---|---|
| High | GPIO GUI commands do not match USB firmware parser | Versioned command protocol and matching validation |
| High | Log cleanup formats credentials and administrator settings | Authenticated deletion of log files only |
| High | Modem startup and SMS sending block timer updates | Asynchronous AT transaction queue and independent output ticks |
| High | Startup/call handling deletes SIM messages indiscriminately | No bulk SIM deletion; delete only individually processed messages |
| High | AT reads consume unsolicited SMS/call notifications | Separate notification routing during active transactions |
| Medium | Commands and credentials remain queued after disconnect | Clear pending requests and secret fields; stop on an error |
| Medium | GUI reports transmission as success without device confirmation | Match request IDs and report explicit device results |
| Medium | Zero, overflowing and malformed durations are accepted | Validate complete numeric tokens and bounded durations |
| Medium | New password is visible; worker spins without idle wait | Mask both password fields and use blocking reads with timeouts |
| Medium | Opening the GUI intentionally resets the controller | Remove intentional DTR/RTS reset sequence |
| Medium | Password files can be partially overwritten; salt is generated before seeding | Atomic settings file, hardware RNG and new password KDF |
| Medium | Power-cycle behavior does not restore timers as README implies | Explicitly document cancellation of timers after restart |

## Independent baseline checks

The unmodified GUI was exercised with a PySide6 compatibility adapter because
native PyQt6 was unavailable. It constructed successfully, but generated the
unsupported USB command, accepted a zero-minute timer, retained five queued
commands plus an active macro timer after disconnect, and left the new-password
field unmasked. The idle worker performed 100,000 empty polls in approximately
0.02 seconds. These are reproducible logic checks, not hardware tests.

## Remaining work

The candidate substantially changes firmware control flow and USB compatibility.
It requires successful firmware compilation and hardware acceptance checks before
stable use. Follow [VALIDATION.md](VALIDATION.md); retain v1.0.0 as the known
working fallback. Complete hardware verification before expanding features.
