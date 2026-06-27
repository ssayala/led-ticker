# LED Ticker

Desk sign + ambient ticker built on an ESP32-S3 and a DIYables 4-in-1 MAX7219 LED matrix. Rotates stocks, weather, and a clock; flips to a steady sign or a countdown timer on demand. Wi-Fi only feeds the live data; the sign and timer run fully offline.

<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="assets/device-sketch-dark.png">
    <img src="assets/device-sketch-light.png" alt="Line-art illustration of the LED Ticker — a 4-in-1 LED matrix in a bracket, wired to an ESP32-S3 board" width="720">
  </picture>
</p>

## Features

- **Sign mode** — one-tap status text ("BUSY", "FOCUS", "ON AIR") that overrides the ambient rotation, with an optional auto-clear timer. Or run a **countdown timer** (1–99 min): a live `MM:SS`, an end animation, then back to ambient.
- **Live data** — stock quotes (Finnhub) and multi-location weather (MET Norway).
- **12-hour clock** — steady `H:MM` when shown alone, scrolls `H:MM AM/PM` when mixed in.
- **Display on/off** — blank the matrix and pause fetches without losing the saved ambient mode.
- **Adjustable brightness & scroll speed** — set from the mobile app or CLI, applied live and persisted on the device.
- **Companion mobile app** — multi-device switcher, preset chip grid, and per-category display toggles, on [iOS](https://apps.apple.com/app/id6772027776) and [Android](https://github.com/ssayala/led-ticker-android).
- **Configured entirely over BLE** — no build-time secrets; WiFi, Finnhub key, tickers, locations, mode, timezone, and active sign all set wirelessly and persisted.
- **PIN-gated BLE** — every write requires a 6-digit PIN, generated on first boot and rotated on factory reset.
- **Factory reset** — hold the BOOT button for 10 s to wipe all settings, forget every BLE bond, and reboot into setup mode with a fresh PIN.

<p align="center">
  <img src="assets/features.jpg" alt="Illustrated features guide: sign mode, countdown timer, live stocks and weather, the companion mobile app, and factory reset" width="720">
</p>

## Hardware

A [Freenove ESP32-S3-WROOM (FNK0099)](https://store.freenove.com/products/fnk0099) and a [DIYables 4-in-1 MAX7219 8×8 LED matrix](https://diyables.io/products/dot-matrix-display-fc16-4-in-1-32x4-led). Wiring, porting notes, and pin config are in [Getting started](GETTING_STARTED.md#hardware--wiring). The repo includes a [custom PCB](hardware/pcb/README.md) and a [3D-printable frame](hardware/case/).

## Build & flash

1. Install [PlatformIO](https://platformio.org/).
2. Build and upload from the repo root: `pio run -d firmware -t upload`, then press the board's reset button.
3. First boot scrolls the BLE name + a 6-digit PIN — note it; you'll need it on every client.

No toolchain? Flash released firmware straight from a browser at **[ledticker.app/flash](https://ledticker.app/flash)**.

Provision from the mobile app or the CLI. (The Wokwi simulator has no BLE — there you provision over a [USB serial console](GETTING_STARTED.md#provisioning-over-usb-serial-opt-in--wokwi) instead.)

## Documentation

- **[Getting started](GETTING_STARTED.md)** — build, flash, first-boot pairing, and `config.h` tuning.
- **[Python client (`led_ticker`)](tools/README.md)** — `pip`-installable library (`from led_ticker import LedTicker`) and the `uv`-based `led.py` CLI, for driving the device or building your own tools.
- **[BLE protocol](BLE_PROTOCOL.md)** — UUIDs, payloads, and semantics for custom clients.
- **[Firmware guide](firmware/FIRMWARE_GUIDE.md)** — firmware internals: dual-core model, display state machine, and how to extend it.
- **[Custom PCB](hardware/pcb/README.md)** — board sources, render, and ordering.
- **[3D-printed frame](hardware/case/)** — printable parts (top/bottom strips or a single bracket) that hold the matrix and PCB together.
- **Companion mobile app** — [iOS](https://apps.apple.com/app/id6772027776) (App Store) and [Android](https://github.com/ssayala/led-ticker-android) (open source).

## License

Open and permissive © Sunil Sayala — free to use, build, modify, share, and sell, including commercially, with attribution.

- **Firmware** — [Apache-2.0](LICENSE).
- **Hardware design files** in [`hardware/`](hardware/) (PCB, mechanical, frame STLs) — [CC BY 4.0](hardware/LICENSE).
- **Python client** in [`tools/`](tools/) (the `led_ticker` package & `led` CLI) — [Apache-2.0](tools/LICENSE).

Attribution is required by these licenses — please keep the credit; see [NOTICE](NOTICE).
