#pragma once

// Firmware version (semver MAJOR.MINOR.PATCH). Bump on every build that
// goes onto a device, then tag the same commit in git (e.g. `git tag v0.1.0`)
// so the string here and the tag in history always point at the same code.
// Surfaced on Serial at boot and via the Version BLE characteristic so
// the iOS app and `tools/led.py get version` can identify what's running.
#define FW_VERSION "0.2.0"
