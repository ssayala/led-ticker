# CLAUDE.md

Guidance for Claude Code working in this repo. Architecture details live in [`ARCHITECTURE.md`](ARCHITECTURE.md) — read it before non-trivial edits to `src/main.cpp`.

## Build & Upload

```bash
pio run -t upload       # build and flash to ESP32-S3
pio device monitor      # serial monitor (115200 baud, /dev/ttyACM0)
pio run                 # build only
```

Press the physical reset button after flashing.

## Project Overview

Single-file ESP32-S3 firmware (`src/main.cpp`) driving a DIYables 4-in-1 MAX7219 matrix. Two orthogonal display layers:

- **Ambient rotation** — stocks (Finnhub), weather (Open-Meteo, geocoded on-device), clock (NTP). Controlled by an enabled-category bitmask.
- **Active status (sign mode)** — single text string that overrides ambient until expiry/clear. ≤5 chars steady, longer scrolls. No on-device preset library — iOS app holds chips locally. Can also run as a **countdown timer** (`timer <min>` Command verb, 1–99 min) that renders a live `MM:SS`, plays a random end animation at zero, then resumes ambient — mutually exclusive with the text sign.

Both layers set independently over BLE. Missing prereqs (WiFi creds, Finnhub key) divert to `MODE_SETUP`. See [`ARCHITECTURE.md`](ARCHITECTURE.md) for mode state, sign mode internals, boot sequence, reset semantics, etc.

## Hardware

- **Board:** Freenove ESP32-S3-WROOM (FNK0099). PCB in `pcb/` is module-specific.
- **Display:** DIYables 4-in-1 MAX7219, hardware SPI.
- **SPI pins:** DIN=GPIO6, CLK=GPIO4, CS=GPIO5. Must call `SPI.begin(CLK, -1, DIN, CS)` before display init.
- **Status LED:** WS2812 on GPIO 48. Blue during fetches.
- **Reset button:** BOOT button on GPIO 0. Hold 10s during runtime → full factory reset (wipes all NVS, regenerates PIN, reboots); 2s mark starts a `RESET N` countdown for abort visibility. Safe to poll at runtime — GPIO0 is sampled by the bootloader only at hardware reset. **Do not hold this button while pressing the physical RESET button** — that combination drops the chip into the ROM bootloader.
- **Porting:** edit `DIN_PIN` / `CLK_PIN` / `CS_PIN` / `RGB_LED_PIN` / `BUTTON_PIN` (plus `HARDWARE_TYPE` / `MAX_DEVICES`) in `src/config.h`.

## Configuration

- No build-time secrets. `src/secrets.h` does not exist.
- **`src/config.h`** — compile-time defaults only: seed `stockTickers[]`, `defaultLocations[]`.
- **NVS namespaces:** `wifi`, `apikey`, `tickers`, `locs`, `display` (key `mask`), `pin` (keys `code`, `on`). Tombstones: `msgs`, `status` (wiped on reset, otherwise unused).
- **RAM-only:** fetched quotes/weather, active sign. Power cycle clears sign and resumes ambient.
- **BLE name:** `LED-Ticker-XXXX` (low 2 bytes of chip MAC) — primary control plane.

CLI: `uv run tools/led.py <cmd>` — see the script for command list.
iOS app in `ios/` uses the same BLE service. BLE is the contract; no firmware change needed for iOS iteration.

## BLE Service

Service UUID `4fafc201-1fb5-459e-8fcc-c5c9c331914b`. Full UUID/payload reference in `BLE_PROTOCOL.md`.

- WiFi payload: `SSID|password` — split on **first** `|`
- Status payload: `text|N` — split on **last** `|`
- `Command` (`...26ab`) write-only; `Version` (`...26b0`) read-only; `Auth` (`...26b2`) write-only
- UUID `...26aa` is a tombstone (old Messages characteristic) — not registered
- **Auth gate:** every write characteristic is gated on `isConnAuthed()` when `nvsPinEnforce` is true. On by default on a fresh flash; flip off via `Command=pin-enforce off`. Two ways a connection becomes authenticated:
  - **Bonded link** (`NimBLEDevice::setSecurityAuth(true, true, true)` + `DISPLAY_ONLY` IO cap → passkey entry). `onConnect` asks the central to pair; iOS shows its native PIN dialog. `onPassKeyRequest` returns `atoi(nvsPin)`. `onAuthenticationComplete` (or `desc->sec_state.encrypted` on reconnect of a bonded peer) flips `slot.authed = true`. This is the iOS path — zero app changes. Just-Works (MITM=false) would let any in-range iPhone bond silently; the PIN is what stops that.
  - **PIN on the Auth char** (`…26b2`, write-only). Fallback for clients that decline pairing (Python CLI on Linux). 5 wrong PINs → 5s silent lockout on that connection's slot.
  - Recovery: PIN is shown on the LED in setup mode and printed on serial at every boot; factory reset rotates it and forgets all bonds.

## Rules for editing

- **BLE callbacks:** copy payload to `pending*` buffer + set `*UpdatePending` flag. Do not do network/display work in the callback. `loop()` handles it via `applyPending*()`.
- **New write characteristics:** use the 2-arg `onWrite(NimBLECharacteristic*, ble_gap_conn_desc*)` overload and call `isConnAuthed(desc->conn_handle)` at the top — returns true while enforcement is off, so it's safe to gate unconditionally.
- **Cross-core:** fetch task runs on Core 0 and must NOT touch `neopixelWrite()` directly. It sets the `fetching` volatile flag; Core 1 `loop()` consumes via `updateStatusLed()`.
- **`initTime()` is WiFi-gated** — starting SNTP without a connection wedges the device.
- **No `delay()` in `loop()`** — it's cooperative.
- **Status writes bypass fetch cooldown** (`BLE_FETCH_COOLDOWN_MS = 10s`) — they must feel immediate.

## Versioning

`FW_VERSION` in `src/version.h`. Bumped manually, tagged in git. Surfaced via serial banner, `[hb]` heartbeat prefix, and read-only Version BLE characteristic. iOS treats `.version` as optional for back-compat.

Per-release workflow:

1. Bump `FW_VERSION` in `src/version.h`.
2. Commit (`git commit -am "release v0.3.0"`).
3. Tag the commit (`git tag v0.3.0`) so the string in the code and the tag in history point at the same commit — `git checkout v0.3.0` rebuilds the exact firmware on a board.
4. `pio run -t upload`, reset the board, confirm the new version on Serial or in the iOS app.
5. `git push && git push --tags` once you're happy.
