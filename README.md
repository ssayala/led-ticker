# LED Ticker

Desk sign + ambient ticker built on an ESP32-S3 and a DIYables 4-in-1 MAX7219 LED matrix. Rotates stocks, weather, and a clock; flips to a steady sign or a countdown timer on demand.

[![Project page](https://img.shields.io/badge/Project%20Page-%E2%86%97-FF2F2F?style=for-the-badge&labelColor=FF2F2F)](https://ledticker.app/)

<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="assets/device-sketch-dark.png">
    <img src="assets/device-sketch-light.png" alt="Line-art illustration of the LED Ticker — a 4-in-1 LED matrix in a bracket, wired to an ESP32-S3 board" width="720">
  </picture>
</p>

## Features

- **Sign mode** — one-tap status text ("BUSY", "FOCUS", "ON AIR") that overrides the ambient rotation, with an optional auto-clear timer. Or run a **countdown timer** (1–99 min): a live `MM:SS`, an end animation, then back to ambient.
- **Live data** — stock quotes (Finnhub) and multi-location weather (Open-Meteo, geocoded on-device).
- **12-hour clock** — steady `H:MM` when shown alone, scrolls `H:MM AM/PM` when mixed in.
- **Display on/off** — blank the matrix and pause fetches without losing the saved ambient mode.
- **Adjustable brightness & scroll speed** — set from the iOS app or CLI, applied live and persisted on the device.
- **Companion [iOS app](https://apps.apple.com/app/id6772027776)** — multi-device switcher, preset chip grid, and per-category display toggles.
- **Configured entirely over BLE** — no build-time secrets; WiFi, Finnhub key, tickers, locations, mode, timezone, and active sign all set wirelessly and persisted.
- **PIN-gated BLE** — every write requires a 6-digit PIN, generated on first boot and rotated on factory reset.
- **Factory reset** — hold the BOOT button for 10 s to wipe all settings, forget every BLE bond, and reboot into setup mode with a fresh PIN.

<p align="center">
  <img src="assets/features.jpg" alt="Illustrated features guide: sign mode, countdown timer, live stocks and weather, the iOS companion app, and factory reset" width="720">
</p>

## Hardware

| Component | Part |
|-----------|------|
| MCU board | [Freenove ESP32-S3-WROOM (FNK0099)](https://store.freenove.com/products/fnk0099) |
| Display | [DIYables 4-in-1 MAX7219 8×8 LED matrix](https://diyables.io/products/dot-matrix-display-fc16-4-in-1-32x4-led) |

Wiring, porting notes, and pin config are in [Getting started](GETTING_STARTED.md#hardware--wiring). The repo includes a [custom PCB](hardware/pcb/README.md) and a [3D-printable case](hardware/case/).

## Build & flash

1. Install [PlatformIO](https://platformio.org/).
2. Build and upload from the repo root: `pio run -d firmware -t upload`, then press the board's reset button.
3. First boot scrolls the BLE name + a 6-digit PIN — note it; you'll need it on every client.

No toolchain? Flash released firmware straight from a browser at **[ledticker.app/flash](https://ledticker.app/flash)**.

## Documentation

- **[Getting started](GETTING_STARTED.md)** — build, flash, first-boot pairing, and `config.h` tuning.
- **[CLI tool](tools/README.md)** — the `uv`-based `led.py` command reference.
- **[BLE protocol](BLE_PROTOCOL.md)** — UUIDs, payloads, and semantics for custom clients.
- **[Firmware guide](firmware/FIRMWARE_GUIDE.md)** — firmware internals: dual-core model, display state machine, and how to extend it.
- **[Custom PCB](hardware/pcb/README.md)** — board sources, render, and ordering.
- **[3D-printed case](hardware/case/)** — printable STLs for the matrix bracket and strips.
- **[iOS app](https://apps.apple.com/app/id6772027776)** — the companion app, on the App Store.

## License

Source-available under the [PolyForm Noncommercial License 1.0.0](LICENSE.md) © Sunil Sayala. Free to build, modify, and share for **noncommercial** use — personal, hobby, research, education. **Commercial use, including selling devices based on this work, is not permitted.** Covers the firmware and the hardware design files in [`hardware/`](hardware/).
