# LED Ticker

Desk sign + ambient ticker built on an ESP32-S3 and a DIYables 4-in-1 MAX7219 LED matrix. Rotates stocks, weather, and a clock; flips to a steady sign ("BUSY", "FOCUS", "ON AIR") on demand with an optional auto-clear timer.

[![Project page](https://img.shields.io/badge/Project%20Page-%E2%86%97-FF2F2F?style=for-the-badge&labelColor=FF2F2F)](https://ledticker.app/)

<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="assets/device-sketch-dark.png">
    <img src="assets/device-sketch-light.png" alt="Line-art illustration of the LED Ticker — a 4-in-1 LED matrix in a bracket, wired to an ESP32-S3 board" width="720">
  </picture>
</p>

## Features

- **Sign mode** — one-tap status text that overrides the ambient rotation, with an optional auto-clear timer. Or run a **countdown timer** (1–99 min): a live `MM:SS`, a random end animation (fireworks, sonar, sparkle), then back to ambient.
- **Display on/off** — blank the matrix and pause fetches without losing the saved ambient mode. Volatile, so a power cycle returns to on.
- **Live data** — stock quotes (Finnhub) and multi-location weather (Open-Meteo, geocoded on-device).
- **12-hour clock** — steady `H:MM` when shown alone, scrolls `H:MM AM/PM` when mixed in. Timezone via `TIMEZONE` in `firmware/src/config.h`.
- **Companion [iOS app](ios/README.md)** — multi-device switcher, preset chip grid, and a Display tab with per-category toggles plus a master on/off.
- **Configured entirely over BLE** — no build-time secrets; WiFi, Finnhub key, tickers, locations, mode, and active sign all set wirelessly and persisted in NVS.
- **PIN-gated BLE** — every write gated on a 6-digit PIN. iOS uses the native pairing dialog; the CLI sends it via a dedicated Auth characteristic. Generated on first boot, rotates on factory reset.
- **Factory reset** — hold the BOOT button (GPIO 0) for 10 s (the matrix counts down from the 2 s mark; release to abort). Wipes all NVS, forgets every BLE bond, and reboots into setup mode with a fresh PIN.

<p align="center">
  <img src="assets/features.jpg" alt="Illustrated features guide: sign mode, countdown timer, live stocks and weather, the iOS companion app, and factory reset" width="720">
</p>

## Hardware

| Component | Part |
|-----------|------|
| MCU board | [Freenove ESP32-S3-WROOM (FNK0099)](https://store.freenove.com/products/fnk0099) |
| Display | [DIYables 4-in-1 MAX7219 8×8 LED matrix](https://diyables.io/products/dot-matrix-display-fc16-4-in-1-32x4-led) |

Wiring, porting notes, and pin config are in [Getting started](GETTING_STARTED.md#hardware--wiring). The board is also available as a [custom PCB](hardware/pcb/README.md).

## Build & flash

1. Install [PlatformIO](https://platformio.org/).
2. Build and upload from the repo root: `pio run -d firmware -t upload`, then press the board's reset button.
3. First boot scrolls the BLE name + a 6-digit PIN (e.g. `LED-Ticker-AB12  PIN 482 913`) — note it; you'll need it on every client.

Full walkthrough — wiring, pairing, and `config.h` tuning — in **[Getting started →](GETTING_STARTED.md)**.

## Documentation

- **[Getting started](GETTING_STARTED.md)** — build, flash, first-boot pairing, and `config.h` tuning.
- **[CLI tool](tools/README.md)** — the `uv`-based `led.py` command reference.
- **[BLE protocol](BLE_PROTOCOL.md)** — UUIDs, payloads, and semantics for custom clients.
- **[Architecture](ARCHITECTURE.md)** — firmware internals.
- **[Custom PCB](hardware/pcb/README.md)** — board sources, render, and ordering.
- **[iOS app](ios/README.md)** — the companion app.
- **Versioning** — `FW_VERSION` in [`firmware/src/version.h`](firmware/src/version.h); release workflow in [`CLAUDE.md`](CLAUDE.md#versioning).

## License

Source-available under the [PolyForm Noncommercial License 1.0.0](LICENSE.md) © Sunil Sayala. Free to build, modify, and share for **noncommercial** use — personal, hobby, research, education. **Commercial use, including selling devices based on this work, is not permitted.** Covers the firmware, the iOS app, and the hardware design files in [`hardware/`](hardware/).
