# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & Upload

```bash
pio run -t upload       # build and flash to ESP32-S3
pio device monitor      # serial monitor (115200 baud, /dev/ttyACM0)
pio run                 # build only, no upload
```

After flashing, press the physical reset button on the board to ensure new firmware runs.

## Project Overview

Single-file ESP32-S3 firmware (`src/main.cpp`) that drives a DIYables 4-in-1 MAX7219 LED matrix via SPI. Two orthogonal display layers:

**Ambient rotation** — scrolls three content types based on the enabled-category mask:
- **Stock quotes** — fetched from Finnhub API, held in RAM, market-hours aware
- **Weather** — per-location current conditions from Open-Meteo; user-entered zip/`City, State` strings are geocoded on-device and resolved coords live in RAM
- **Clock** — 12-hour HH:MM AM/PM, sourced from NTP via `configTzTime`. Scrolls in mixed mode; when `clock` is the *only* enabled category the display switches to a static "H:MM" with a 1Hz blinking colon (see "Static clock fast path" below).

**Active status (sign mode)** — a single text string ("BUSY", "FOCUS", "ON AIR", etc.) with optional auto-clear timer. When set, it overrides the ambient rotation completely until it expires or is cleared. Short text (≤5 chars) renders steady; longer text scrolls. There is no rotating-messages category and no on-device preset library — the iOS app holds its own chip list locally and writes one-shot signs to the device.

BLE writes set the ambient mode and the active status independently. When the user requests a mode whose prereqs are missing (WiFi creds, Finnhub key), the device enters a transient `MODE_SETUP` that displays configuration hints — see "Setup mode" below.

## Hardware

- **Board:** Freenove ESP32-S3 (espressif32 @ 6.5.0, Arduino framework)
- **Display:** 4-in-1 MAX7219 8x8 LED matrix, hardware SPI with explicit pin mapping
- **SPI pins:** DIN=GPIO6, CLK=GPIO4, CS=GPIO5 (must call `SPI.begin(CLK, -1, DIN, CS)` before display init — default ESP32-S3 SPI pins don't match)
- **Status LED:** Onboard WS2812 RGB LED on GPIO 48. Lights blue during network fetches. Driven from `loop()` via a volatile flag set by the fetch task on Core 0.

## Configuration Model

There are **no build-time secrets**. `src/secrets.h` does not exist and is not needed.

- **`src/config.h`** — compile-time *defaults* only: seed `stockTickers[]` and `defaultLocations[]`. Used to seed NVS on first boot. No fallback message text (there's no messages feature anymore).
- **NVS (ESP32 `Preferences`)** — user config only, set over BLE, survives reboots. Namespaces used: `wifi`, `apikey`, `tickers`, `locs`, `display` (single `mask` byte holding the enabled-category bitmask), `status` (active sign text + expiry epoch). `msgs` is a tombstone namespace from the old messages feature — wiped on `cmd=reset` but otherwise unused. **Fetched values (stock quotes, weather readings) are never persisted** — they live in RAM and are re-fetched on boot.
- **BLE (`LED-Ticker-XXXX`, where `XXXX` is the low 2 bytes of the chip MAC)** — primary control plane for WiFi creds, Finnhub key, tickers, locations, mode, active status, and commands. On first boot with no WiFi configured, the device enters `MODE_SETUP` and stays there indefinitely until WiFi is set (see Architecture Notes).

The companion CLI is `tools/led.py`, invoked as `uv run tools/led.py <cmd>`:
`wifi <SSID...> <password>`, `apikey <key>`, `tickers AAPL,MSFT,...`, `locations "Seattle, WA" 98052 ...`, `mode all | <cat> [<cat> ...]` (cat ∈ {stocks, weather, clock}), `status [TEXT [MINUTES] | clear]`, `get wifi|apikey|tickers|status|locations|mode`, `reload`, `reset`.

There's also a SwiftUI iOS app in `ios/` that talks to the same BLE service. **Note:** at the time of this writing the iOS app is mid-refactor for the messages→status change — see `IOS_STATUS_PLAN.md` (untracked) for the work pending on a Mac. Until that lands, the iOS app still references the removed Messages characteristic and won't fully function against current firmware. The BLE service is the contract; no firmware change is needed when iterating on iOS.

## Architecture Notes

- **Boot sequence matters** (`setup()`): `initDisplay()` → load user config from NVS (`wifi`, `apikey`, `tickers`, `locs`, `display`, `status`) → `buildDeviceName()` → enter `MODE_SETUP(enabledMask)` if any prereq for the saved mask is missing, else `enterContent()` → `showNext()` so the display is live before networking → `connectWifi()` → `initTime()` → `initBLE()` → `triggerFetch()`. `initTime()` is **gated on `WiFi.status() == WL_CONNECTED`** and no-ops otherwise; this avoids starting lwIP's SNTP daemon into a retry loop on no-WiFi boots (the pre-IDF-5 SNTP client accumulates state on failed DNS lookups and wedges the device after 10s of minutes). When WiFi is configured later via BLE, `applyPendingWifi()` calls `initTime()` after the reconnect to kick off NTP.
- **Display mode is a bitmask**: `enabledMask` holds any non-empty subset of `BIT_STOCKS | BIT_WEATHER | BIT_CLOCK` (= `MASK_ALL`, `0x0D`). Bit `0x02` is reserved (was `BIT_MESSAGES`; tombstoned and stripped from any legacy NVS mask on load). Persisted in NVS namespace `display` under key `mask`. Within `MODE_CONTENT`, `currentBit` tracks which category is scrolling and `advanceCategory()` rotates STOCKS → WEATHER → CLOCK, skipping bits that are disabled or that have no data. A single-bit mask just never advances. The Mode characteristic accepts `"all"`, a single token, or a comma-separated subset like `"stocks,weather"`; the token `"messages"` is no longer accepted (writes containing it are rejected). Reads return the canonical form (`"all"` when full, otherwise comma-joined).
- **Static clock fast path**: when `enabledMask == BIT_CLOCK` *and* `timeReady` *and* no status sign is active, `loop()` calls `tickStaticClock()` instead of the normal `display.displayAnimate()` → `showNext()` pump. `tickStaticClock()` formats `"H:MM"` and swaps the `':'` for `' '` on a 500ms cadence (1Hz visual blink) via `displayText(..., PA_PRINT, PA_NO_EFFECT)`, and only redraws when minute or blink phase changes. Before NTP completes, the static branch is skipped and the scroll path renders `"Loading time..."`. In any mixed mode (`BIT_CLOCK` + any other bit) clock scrolls as `"H:MM AM/PM"` via `showNextClock()` like the other categories, and `advanceCategory()` rotates off it immediately since there's only one item per pass.
- **Sign mode (active status)**: orthogonal override layer driven by the `Status` BLE characteristic (UUID `...26af`). When `activeStatusText` is non-empty and not yet expired, `tickActiveStatus()` runs *instead of* the ambient rotation. Short text (`strlen ≤ STATUS_STATIC_MAX_CHARS = 5`) renders steady via `displayText(PA_PRINT, PA_NO_EFFECT)` — no blink, since a sign is information, not an alarm. Longer text scrolls on a loop. `statusShown`/`statusShownIsScroll` are file-scope caches so the static path is one-shot per text change; out-of-band wipes (clear, expiry, reset) go through `invalidateStatusRender()` so the next tick repaints. `clearActiveStatusAndResume()` is the standard "clear + invalidate + displayClear + enterContent" helper. State is persisted to NVS namespace `status` (keys `text`, `exp`) so a power blip mid-meeting still ends the sign on time. **`statusExpiresAt == 0`** means "no status active"; **`UINT32_MAX`** means "indefinite (until cleared)"; otherwise it's a Unix epoch second. A timed write (`secs > 0`) before NTP completes is silently coerced to indefinite to avoid bad-clock expiry. `checkStatusForRender()` is the single combined gate-plus-expiry function called once per loop iteration — it returns `false` and clears in-place if expiry has passed, saving a redundant `time(NULL)` versus a separate "active?" and "expired?" pair. There is no preset library on the device — chips live entirely in the iOS app.
- **Setup mode** (`MODE_SETUP`): Transient mode shown when prereqs are missing for the requested mask. `setupTargetMask` records the mask to resume into. Entered on boot if the saved mask's prereqs aren't met, when the user writes a mode whose prereqs are missing, or after `cmd=reset`. Cycles two scroll frames (contextual hint + BLE device name like `LED-Ticker-AB12`). The hint targets the first unsatisfied prereq in `setupTargetMask` (WiFi → Finnhub key → weather-needs-WiFi). Clock has the same prereq as weather (WiFi for NTP), so it shares the "Configure WiFi over BLE" hint without a new branch. Exits when prereqs are satisfied (`applyPendingWifi`/`applyPendingApiKey` call `exitSetupIfReady()`, which also persists the new mask), or when the user sends an explicit `mode=` write. **Timeout behavior** (`SETUP_TIMEOUT_MS = 60s` of no BLE activity): if `wifiConfigured()`, fall through to `MODE_CONTENT` with the same mask — unmet-prereq bits become "Loading..." hints. If **not** `wifiConfigured()`, the timeout no-ops (and pushes the next check out by another 60s) — every category in the mask requires WiFi, so falling through would just rotate "Loading X..." forever; the "Configure WiFi over BLE" setup hint is strictly better in that state. The Status sign overrides setup mode either way, so a no-WiFi device can still be used as a manual sign over BLE. Every BLE characteristic `onWrite` resets the activity timer.
- **RAM-only fetched data**: stocks (`stockQuotes[]`, raw price + changePct) and weather (`weatherReadings[]`, raw tempF) live only in RAM. The display format (e.g. `\x18`/`\x19` arrows, `°F`) is computed on every scroll in `showNextStock()`/`showNextWeather()`, so format changes take effect immediately without a reload.
- **BLE write → deferred apply pattern**: each characteristic callback copies the payload into a `pending*` buffer and sets a `*UpdatePending` volatile flag. `loop()` consumes these by calling `applyPending*()` handlers, keeping work out of the BLE callback context. Don't do heavy work (network, display) inside callbacks.
- **Fetch cooldown**: writes that trigger network activity (ticker update, locations update, `reload`, `reset`) share a 10s `BLE_FETCH_COOLDOWN_MS` gate to prevent hammering the upstream APIs if the client retries. Status writes are *not* gated — they're cheap and need to feel immediate.
- **Reset semantics**: `cmd=reset` clears *all* NVS namespaces (`wifi`, `apikey`, `tickers`, `msgs` tombstone, `locs`, `display`, `status`), zeroes the in-memory `nvsWifiSsid`/`nvsWifiPass`/`nvsApiKey`/`activeStatusText` buffers (otherwise `wifiConfigured()`/`apiKeyConfigured()` keep returning true and `statusActive` keeps overriding until reboot), calls `WiFi.disconnect()` to drop the live session, re-seeds tickers and locations from `config.h`, resets `enabledMask` to `MASK_ALL`, calls `invalidateStatusRender()`, and enters `MODE_SETUP(MASK_ALL)`. After reset the device needs WiFi and API key reconfigured over BLE.
- **Display gating** (`showNext()`): the cycle skips category bits whose data isn't ready — stocks only when WiFi+API key are configured *and* `stockCount > 0`; weather only when WiFi is configured *and* `weatherCount > 0`; clock only when `timeReady` (NTP has synced at least once). If no enabled bit has data yet, `showNext()` shows a `"Loading <category>..."` hint for `currentBit`. Mode writes whose prereqs are missing are diverted to `MODE_SETUP` by `applyPendingMode`, but the *mask is still saved* so the device resumes into the user's selection once prereqs are met.
- **Market-hours aware**: `isMarketOpen()` (Mon-Fri 9:30-16:00 ET) gates only *fetches*, not display. When the market is closed we keep the last-fetched quotes in RAM and skip the API call; on a cold boot with no data yet, we fetch once regardless.
- **Geocoding**: `fetchWeatherImpl()` resolves each location string to lat/lon via Open-Meteo's geocoding API on first use and caches the result in `resolved[]` (RAM). `applyPendingLocations()` invalidates that cache by setting `resolved[i].ok = false` so the next fetch re-geocodes. A trailing `", XX"` in the user string is used as an `admin1`/`country_code` filter to disambiguate duplicate city names.
- **Main loop** is cooperative: apply pending BLE updates → update status LED → check setup-mode timeout (gated on `wifiConfigured()` — see Setup mode above) → render precedence: `checkStatusForRender()` (active sign overrides everything) → static-clock fast path → normal scroll pump → fetch every `FETCH_INTERVAL_MS` (5 min) if WiFi is up. No `delay()` inside `loop()`.
- **Cross-core safety**: the fetch task runs on Core 0 but must not call `neopixelWrite()` directly. Instead it sets the volatile `fetching` flag that `loop()` on Core 1 consumes via `updateStatusLed()` — same deferred pattern as BLE callbacks.

## BLE Service

Advertised as `LED-Ticker-XXXX` (low 2 bytes of chip MAC), service UUID `4fafc201-1fb5-459e-8fcc-c5c9c331914b`. All characteristics except `Command` are read/write — reads return current state (e.g. `Tickers` returns the comma-joined ticker list, `WiFi` returns SSID only, `Status` returns `text|seconds_remaining` or empty); `Command` is write-only. Payload formats and UUIDs are documented in `README.md`. WiFi payload is `SSID|password` split on the first `|` (passwords may contain `|`; SSIDs may not). Status payload is `text|N` split on the *last* `|` (text may contain `|` in theory; the seconds tail never does).

UUID `...26aa` was once a "Messages" characteristic; it is **not** registered in the current firmware (kept as a tombstone comment so it won't be reused). Reads against it fail at the GATT layer — relevant if you're updating old client code that still probes for it.
