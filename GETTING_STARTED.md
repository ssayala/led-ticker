# Getting started

Build, flash, and configure an LED Ticker. For what it is and what it does, see the [main README](README.md).

## Hardware & wiring

The firmware targets one specific board; the custom PCB is sized for the same module (see [`hardware/pcb/`](hardware/pcb/README.md)).

| Component | Part |
|-----------|------|
| MCU board | [**Freenove ESP32-S3-WROOM (FNK0099)**](https://store.freenove.com/products/fnk0099) — onboard NeoPixel on GPIO 48, native USB-CDC |
| Display | [DIYables 4-in-1 MAX7219 8x8 LED matrix](https://diyables.io/products/dot-matrix-display-fc16-4-in-1-32x4-led) |

### Wiring (matches the PCB)

| Matrix Pin | ESP32-S3 GPIO |
|------------|---------------|
| VCC | 5V |
| GND | GND |
| DIN | 6 (MOSI) |
| CLK | 4 (SCK) |
| CS | 5 |

Onboard RGB LED (GPIO 48) lights blue during network fetches. The Freenove board's **BOOT button (GPIO 0)** doubles as the factory-reset trigger — hold for 10 s during normal runtime.

**Using a different ESP32-S3 board?** Edit `DIN_PIN` / `CLK_PIN` / `CS_PIN` / `RGB_LED_PIN` in `firmware/src/config.h`. The PCB has no flexibility — it's footprint-specific to the FNK0099.

## Build & flash

1. Install [PlatformIO](https://platformio.org/).
2. Optionally edit defaults in `firmware/src/config.h` — seed tickers/locations (first-boot NVS seed) and the tunables under [Configuration](#configuration) below.
3. Build and upload from the repo root: `pio run -d firmware -t upload`. Press the physical reset button after flashing.
4. On first boot the display scrolls the BLE device name **and a 6-digit PIN** (e.g. `LED-Ticker-AB12  PIN 482 913`). Note the PIN — you'll need it on every client.

> **No toolchain?** Released firmware can be flashed straight from a browser over USB at **[ledticker.app/flash](https://ledticker.app/flash)** — Chrome or Edge on a desktop, no PlatformIO required.

## First-boot pairing

- **Phone app:** open the app, tap the device. The phone pops a system pairing dialog — type the PIN. Bonded. Future reconnects skip the dialog.
- **CLI:** save the PIN once; future calls auto-include it (see the [CLI tool](tools/README.md)):
  ```
  uv run tools/led.py pin 482913
  uv run tools/led.py wifi My Network Name password123
  uv run tools/led.py apikey your-finnhub-key
  ```
  The last arg to `wifi` is always the password — everything before it is the SSID, so spaces work naturally. To avoid saving the PIN to disk, pass `--pin 482913` per command or set `LED_TICKER_PIN=482913` in the environment instead.

If you ever forget the PIN, read it off the serial monitor (`pio device monitor -d firmware`) at boot, or factory-reset to rotate it.

## Provisioning over USB serial (opt-in / Wokwi)

**Most users provision over BLE (above).** The firmware can *also* expose a **serial command console** over USB (115200 baud) that mirrors every BLE setting — but it's a build-time opt-in, **off in the default and released firmware**. Rebuild with `-DCONSOLE_ENABLED=1` to enable it (it's always on in the [Wokwi simulator](firmware/WOKWI.md)). Then open `pio device monitor -d firmware` and type:

```
wifi MyNetwork mypassword
apikey your-finnhub-key
tickers AAPL,MSFT,GOOG
sign HELLO
help
```

`help` lists every verb; `info` prints current state. Full reference — the PIN-bypass rationale and the `wifi` SSID/password split — is in [Firmware guide → Serial console](firmware/FIRMWARE_GUIDE.md).

## Configuration

**User tunables — `firmware/src/config.h`:**

| Define | Default | Description |
|--------|---------|-------------|
| `SCROLL_SPEED` | 70 | ms per scroll step (lower = faster) |
| `SETUP_SCROLL_SPEED` | 100 | Slower scroll used only in setup mode so the BLE name + PIN are easy to read. Reverts to `SCROLL_SPEED` once setup completes. |
| `DISPLAY_INTENSITY` | 2 | LED brightness, 0–15. Idle mode (post-sign with no ambient data) dims to 0 regardless of this setting. |
| `SIGN_BREATH_MIN/MAX_INTENSITY`, `STEP_MS` | 1 / 6 / 400 | Subtle brightness pulse on static signs. Tune the three together — changing one in isolation loses the "breath" feel. |
| `TIMEZONE` | `PST8PDT,M3.2.0,M11.1.0` | POSIX TZ string |
| `NTP_SERVER_1` / `NTP_SERVER_2` | `time.google.com` / `time.cloudflare.com` | NTP hosts (anycast, no vendor-zone registration) |
| `FETCH_INTERVAL_MS` | 5 min | Stock + weather refresh interval |

**Hardware pins — `firmware/src/config.h`:** `HARDWARE_TYPE`, `MAX_DEVICES` (4), `DIN_PIN`/`CLK_PIN`/`CS_PIN` (6/4/5), `RGB_LED_PIN` (48), `BUTTON_PIN` (0). Edit these when porting to a different board — see [Hardware & wiring](#hardware--wiring) above.
