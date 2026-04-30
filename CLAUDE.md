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

Single-file ESP32-S3 firmware (`src/main.cpp`) that drives a DIYables 4-in-1 MAX7219 LED matrix via SPI. It scrolls three content types:

- **Messages** — pushed over BLE and persisted to NVS, with hardcoded fallbacks from `src/config.h` until set
- **Stock quotes** — fetched from Finnhub API, held in RAM, market-hours aware
- **Weather** — per-location current conditions from Open-Meteo; user-entered zip/`City, State` strings are geocoded on-device and resolved coords live in RAM

A capacitive touch input on GPIO 14 cycles between stocks, messages, and weather at runtime. BLE writes can also set mode.

## Hardware

- **Board:** Freenove ESP32-S3 (espressif32 @ 6.5.0, Arduino framework)
- **Display:** 4-in-1 MAX7219 8x8 LED matrix, hardware SPI with explicit pin mapping
- **SPI pins:** DIN=GPIO11, CLK=GPIO12, CS=GPIO10 (must call `SPI.begin(CLK, -1, DIN, CS)` before display init — default ESP32-S3 SPI pins don't match)
- **Touch:** GPIO 14, threshold 30000; polled every 50ms with a 2s debounce
- **Brightness knob:** B10K potentiometer on GPIO 4 (ADC, optional). Internal pull-down keeps brightness at default (2/15) when not connected.
- **Buzzer:** Passive buzzer on GPIO 5 (optional). Short 2kHz beep on every BLE write as audio confirmation.
- **Status LED:** Onboard WS2812 RGB LED on GPIO 48. Lights blue during network fetches. Driven from `loop()` via a volatile flag set by the fetch task on Core 0.

## Configuration Model

There are **no build-time secrets**. `src/secrets.h` does not exist and is not needed.

- **`src/config.h`** — compile-time *defaults* only: seed `stockTickers[]`, `defaultLocations[]`, and `fallbackMessages[]`. Used to seed NVS on first boot, and as fallback text on the display.
- **NVS (ESP32 `Preferences`)** — user config only, set over BLE, survives reboots. Namespaces used: `wifi`, `apikey`, `tickers`, `msgs`, `locs`. **Fetched values (stock quotes, weather readings) are never persisted** — they live in RAM and are re-fetched on boot.
- **BLE (`LED-Ticker`)** — primary control plane for WiFi creds, Finnhub key, tickers, messages, locations, mode, and commands. On first boot, the display prompts the user to configure WiFi and the API key via BLE before anything else happens.

The companion CLI is `tools/led.py`, invoked as `uv run tools/led.py <cmd>`:
`wifi <SSID...> <password>`, `apikey <key>`, `tickers AAPL,MSFT,...`, `messages "a" "b" ...`, `locations "Seattle, WA" 98052 ...`, `mode stocks|messages|weather`, `reload`, `reset`.

## Architecture Notes

- **Boot sequence matters** (`setup()`): `initDisplay()` → load user config from NVS (`wifi`, `apikey`, `msgs`, `tickers`, `locs`) → `showNext()` so the display is live before networking (shows fallback messages or setup prompts until the first fetch completes) → `connectWifi()` → `initTime()` → `initBLE()` → `triggerFetch()`.
- **RAM-only fetched data**: stocks (`stockQuotes[]`, raw price + changePct) and weather (`weatherReadings[]`, raw tempF + WMO code) live only in RAM. The display format (e.g. `\x18`/`\x19` arrows, `°F`) is computed on every scroll in `showNextStock()`/`showNextWeather()`, so format changes take effect immediately without a reload.
- **BLE write → deferred apply pattern**: each characteristic callback copies the payload into a `pending*` buffer and sets a `*UpdatePending` volatile flag. `loop()` consumes these by calling `applyPending*()` handlers, keeping work out of the BLE callback context. Don't do heavy work (network, display) inside callbacks.
- **Fetch cooldown**: writes that trigger network activity (ticker update, locations update, `reload`, `reset`) share a 10s `BLE_FETCH_COOLDOWN_MS` gate to prevent hammering the upstream APIs if the client retries.
- **Reset semantics**: `cmd=reset` clears *all* NVS namespaces (including `wifi` and `apikey`) and re-seeds tickers and locations from `config.h`. After a reset the device needs WiFi and API key reconfigured over BLE.
- **Display gating** (`showNext()`): stocks show only when WiFi+API key are configured *and* `stockCount > 0`; weather shows only when WiFi is configured *and* `weatherCount > 0`; otherwise falls through to `getMessage()`, which itself returns a setup-prompt string if WiFi or key is missing.
- **Market-hours aware**: `isMarketOpen()` (Mon-Fri 9:30-16:00 ET) gates only *fetches*, not display. When the market is closed we keep the last-fetched quotes in RAM and skip the API call; on a cold boot with no data yet, we fetch once regardless.
- **Geocoding**: `fetchWeatherImpl()` resolves each location string to lat/lon via Open-Meteo's geocoding API on first use and caches the result in `resolved[]` (RAM). `applyPendingLocations()` invalidates that cache by setting `resolved[i].ok = false` so the next fetch re-geocodes. A trailing `", XX"` in the user string is used as an `admin1`/`country_code` filter to disambiguate duplicate city names.
- **Main loop** is cooperative: apply pending BLE updates → poll touch → check brightness knob → check buzzer → update status LED → advance display animation → fetch every `FETCH_INTERVAL_MS` (5 min) if WiFi is up. No `delay()` inside `loop()`.
- **Cross-core safety**: the fetch task runs on Core 0 but must not call `neopixelWrite()` or `tone()` directly. Instead it sets volatile flags (`fetching`, `beepPending`) that `loop()` on Core 1 consumes — same deferred pattern as BLE callbacks.

## BLE Service

Device name `LED-Ticker`, service UUID `4fafc201-1fb5-459e-8fcc-c5c9c331914b`. All characteristics are write-only; payload formats and UUIDs are documented in `README.md`. WiFi payload is `SSID|password` split on the first `|` (passwords may contain `|`; SSIDs may not).
