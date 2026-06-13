# Wokwi simulation

A virtual ESP32-S3 + MAX7219 matrix + WS2812 for testing firmware without
hardware. Config lives in [`wokwi.toml`](wokwi.toml) (points at the PlatformIO
build artifacts) and [`diagram.json`](diagram.json) (the wiring, matching the
pins in [`src/config.h`](src/config.h)).

## Run it

1. Build first — the sim runs the compiled binary, not the source:
   ```bash
   pio run -d firmware
   ```
2. Install the **Wokwi for VS Code** extension, open this repo, and run
   **"Wokwi: Start Simulator"** with `firmware/diagram.json` focused. (Or use
   the [Wokwi CLI](https://docs.wokwi.com/wokwi-ci/getting-started).)
3. The serial monitor opens automatically over the board's USB — no TX/RX
   wiring needed. Watch boot logs, the PIN, and `[fetch]` lines there.

## What it can and can't test

| Subsystem | Works? | Notes |
|-----------|--------|-------|
| Boot, serial, NVS | ✅ | Emulated flash persists within a session |
| MAX7219 display + scrolling | ✅ | 4 chained modules via the `chain` attr |
| WS2812 status LED | ✅ | Lights blue during fetches |
| WiFi + HTTP (stocks/weather) | ✅ | Joins `Wokwi-GUEST` with a real internet gateway |
| **BLE / NimBLE control plane** | ❌ | **Not simulated** — no auth, sign mode, or config writes |

Because BLE provisioning is unavailable, a fresh sim boots with empty NVS →
`MODE_SETUP` (shows the device name + PIN), and never fetches.

## Exercising the weather/stock fetch path

The fetch path needs WiFi creds and a Finnhub key in NVS, which are normally
written over BLE. For sim-only testing, temporarily seed them at boot (e.g. in
`setup()` after `prefs` init, guarded by a `#ifdef WOKWI_SIM`) using SSID
`Wokwi-GUEST` and an empty password — **never commit real secrets**. Then the
ambient rotation runs against live Finnhub/Open-Meteo.

To watch the new 30-minute weather throttle ([`WEATHER_INTERVAL_MS`](src/config.h))
without waiting half an hour, temporarily lower it (e.g. to `60 * 1000`) and
confirm weather fetches less often than the 5-minute stock cadence in the
`[fetch]` serial logs. The throttle's skip path is currently silent — add a
`Serial.printf` there if you want an explicit marker.
