# CLAUDE.md

Guidance for Claude Code working in this repo. [`firmware/FIRMWARE_GUIDE.md`](firmware/FIRMWARE_GUIDE.md) is a high-level map of how the firmware works — read it for orientation before non-trivial edits to `firmware/src/main.cpp`; the specifics are in the code itself. BLE UUIDs, payload formats, and auth semantics live in [`BLE_PROTOCOL.md`](BLE_PROTOCOL.md).

## Build & Upload

```bash
pio run -d firmware -t upload    # build and flash to ESP32-S3
pio device monitor -d firmware   # serial monitor (115200 baud, /dev/ttyACM0)
pio run -d firmware              # build only
pio run -d firmware -e wokwi     # no-PSRAM build for the Wokwi sim (see firmware/WOKWI.md)
pio test -d firmware -e native   # host unit tests (console parser + pure logic)
```

Run from the repo root. Press the physical reset button after flashing.

## Project Overview

Compact ESP32-S3 firmware — `firmware/src/main.cpp` plus small host-tested pure units (`console.*`, `logic.*`) — driving a DIYables 4-in-1 MAX7219 matrix. Two orthogonal display layers, each set independently over BLE:

- **Ambient rotation** — stocks (Finnhub), weather (MET Norway current conditions; locations are geocoded by the client and sent as `lat,lon,label`), clock (NTP). Controlled by an enabled-category bitmask.
- **Active status (sign mode)** — text that overrides ambient until expiry/clear (≤5 chars steady, longer scrolls), or a countdown timer (`timer <min>` Command verb) — mutually exclusive with the text sign. Preset chips live in the companion app, not on-device.

Missing prereqs (WiFi creds, Finnhub key) divert to `MODE_SETUP`.

## Hardware

- **Board:** Freenove ESP32-S3-WROOM (FNK0099). PCB in `hardware/pcb/` is module-specific.
- **Display:** DIYables 4-in-1 MAX7219, hardware SPI. DIN=GPIO6, CLK=GPIO4, CS=GPIO5. Must call `SPI.begin(CLK, -1, DIN, CS)` before display init.
- **Status LED:** WS2812 on GPIO 48, blue during fetches.
- **Reset button:** BOOT button (GPIO 0) held 10 s during runtime → factory reset (wipes NVS, rotates PIN, forgets bonds, reboots). Safe to poll at runtime, but do **not** hold it while pressing the physical RESET button — that drops the chip into the ROM bootloader.
- **Porting:** pins plus `HARDWARE_TYPE`/`MAX_DEVICES` are in `firmware/src/config.h`.

## Configuration

- No build-time secrets — `firmware/src/secrets.h` does not exist. `config.h` is compile-time defaults only.
- **NVS namespaces:** `wifi`, `apikey`, `tickers`, `locs`, `display` (keys `mask`, `bright`, `scroll`), `time` (key `tz`), `pin` (keys `code`, `on`). `msgs`/`status` are tombstones.
- Fetched quotes/weather and the active sign are RAM-only — a power cycle clears the sign and resumes ambient.
- **BLE name:** `LED-Ticker-XXXX` (low 2 bytes of chip MAC) — primary control plane. CLI: `uv run tools/led.py <cmd>` (thin shim over the `led_ticker` package in `tools/src/`; `pip install led-ticker` also gives a `led` command). Use `led.py devices` to list units and `--device XXXX` to target one when several are in range. Companion mobile apps use the same service; BLE is the contract, so app iteration needs no firmware change.
- **Serial console:** a USB-serial command path (`src/console.{h,cpp}` pure parser + `dispatchConsoleCmd`/`pollSerialConsole` in `main.cpp`) mirrors every BLE verb by writing the same `pending*` buffers — for dev/test and as the **only** control path in Wokwi (BLE isn't simulated). Bypasses the PIN gate (physical USB already allows reflashing). Compiled out on real hardware via `CONSOLE_ENABLED` (`config.h` default 1; `env:esp32-s3` sets `-DCONSOLE_ENABLED=0`, `env:wokwi` keeps it on). `wifi <ssid>` (no pass) = open network; `sign` composes `text|0`. Details in [`firmware/FIRMWARE_GUIDE.md`](firmware/FIRMWARE_GUIDE.md).

## BLE auth

Every write characteristic is gated on `isConnAuthed()` while enforcement is on (default; `Command=pin-enforce off` disables). Two paths to authed: passkey bonding (the phone's native PIN dialog — `onPassKeyRequest` returns the NVS PIN) or writing the PIN to the Auth characteristic (CLI fallback). The PIN is shown on the LED in setup mode and on serial at every boot; factory reset rotates it. Details in [`BLE_PROTOCOL.md`](BLE_PROTOCOL.md#auth).

## Rules for editing

- **BLE callbacks:** copy payload to a `pending*` buffer + set the `*UpdatePending` flag; `loop()` applies via `applyPending*()`. No network/display work in callbacks.
- **New write characteristics:** build on `GatedStashCallbacks` (auth gate + activity stamp + pending-buffer stash are inherited; subclass only to add `onRead`). Hand-roll callbacks only when the write needs extra policy (cooldown, empty-as-clear, payload-dependent) — then use the 2-arg `onWrite(NimBLECharacteristic*, ble_gap_conn_desc*)` overload and call `isConnAuthed(desc->conn_handle)` at the top; it returns true while enforcement is off, so gate unconditionally.
- **Cross-core:** the fetch task (Core 0) must NOT touch `neopixelWrite()`. It sets the `fetching` volatile flag; Core 1 `loop()` consumes it via `updateStatusLed()`.
- **`initTime()` is WiFi-gated** — starting SNTP without a connection wedges the device.
- **No `delay()` mid-`loop()`** — it's cooperative; never stall time-sensitive work. The lone exception is a single `delay(1)` at the very end as a pacing yield (lets Core 1 idle instead of busy-spinning), plus the `displayOff`/reset-countdown early-returns.
- **Status writes bypass the 10 s fetch cooldown** — they must feel immediate.

## Versioning

`FW_VERSION` in `firmware/src/version.h`, bumped manually. Release = bump + commit + git tag (`v0.3.0`) on the same commit, flash and confirm the version on serial or in the companion app, then `git push && git push --tags`.
