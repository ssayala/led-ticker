# LED Matrix Ticker

Desk sign + ambient ticker built on an ESP32-S3 and a DIYables 4-in-1 MAX7219 LED matrix. Rotates stocks, weather, and a clock; flips to a steady sign ("BUSY", "FOCUS", "ON AIR") on demand with an optional auto-clear timer.

## Features

- **Sign mode** — one-tap status text, optional auto-clear timer, overrides the ambient rotation while active.
- **Power toggle** — flip the display fully off (matrix dark, onboard LED dark, periodic fetches paused) without losing the saved ambient mode. Volatile; power cycle returns to on.
- **Live data** — stock quotes (Finnhub) and weather (Open-Meteo, multi-location).
- **12-hour clock** — steady "H:MM" when shown alone, scrolls "H:MM AM/PM" when mixed with other categories. Pacific timezone by default (change `TIMEZONE` in `src/config.h`).
- **Companion [iOS app](ios/README.md)** — multi-device switcher, preset chip grid, per-category Display toggles, Power on/off switch.
- **Configured entirely over BLE** — no build-time secrets. WiFi creds, Finnhub key, tickers, locations, mode, and active sign all settable wirelessly and persisted in NVS.

## Hardware

Targets one specific board. The custom PCB is sized for the same module.

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

Onboard RGB LED (GPIO 48) lights blue during network fetches.

**Using a different ESP32-S3 board?** Edit `DIN_PIN` / `CLK_PIN` / `CS_PIN` / `RGB_LED_PIN` near the top of `src/main.cpp`. The PCB has no flexibility — it's footprint-specific to the FNK0099.

## Custom PCB

Carries the Freenove module + a MAX7219 matrix header on a single board. Designed in [EasyEDA](https://easyeda.com/); order via JLCPCB. Sources, mechanical drawing, 3D model, and render are all in [`pcb/`](pcb/).

![PCB 3D render](pcb/pcb.png)

## Quick start

1. Install [PlatformIO](https://platformio.org/).
2. Optionally edit defaults in `src/config.h` — seed tickers/locations (first-boot NVS seed) plus user tunables (timezone, scroll speed, brightness, fetch interval, NTP server).
3. Build and upload: `pio run -t upload`. Press the physical reset button after flashing.
4. On first boot the display scrolls the BLE device name (e.g. `LED-Ticker-AB12`) — that's what to look for in the iOS app or CLI. Configure WiFi and your [Finnhub API key](https://finnhub.io/register) over BLE:
   ```
   uv run tools/led.py wifi My Network Name password123
   uv run tools/led.py apikey your-finnhub-key
   ```
   The last arg to `wifi` is always the password — everything before it is the SSID, so spaces work naturally.

## CLI control

The device advertises as `LED-Ticker-XXXX`. `tools/led.py` is a [bleak](https://github.com/hbldh/bleak)-based CLI you can run from any machine with Bluetooth:

```bash
# Sign mode
uv run tools/led.py status "BUSY" 30      # show for 30 min, then auto-clear
uv run tools/led.py status "ON AIR"       # indefinite
uv run tools/led.py status clear

# Ambient mode (subset of stocks/weather/clock, 'all', or 'none' for sign-only)
uv run tools/led.py mode stocks weather
uv run tools/led.py mode clock
uv run tools/led.py mode all
uv run tools/led.py mode none

# Power (volatile — power cycle returns to on)
uv run tools/led.py power on
uv run tools/led.py power off

# Data
uv run tools/led.py tickers AAPL TSLA NVDA SPY
uv run tools/led.py locations "Seattle, WA" 98052
uv run tools/led.py apikey your-finnhub-key
uv run tools/led.py wifi My Network Name password

# Inspect
uv run tools/led.py get version           # firmware version on the device
uv run tools/led.py get wifi|apikey|tickers|status|locations|mode|power  # read other settings

# Maintenance
uv run tools/led.py reload                # force stock refresh
uv run tools/led.py reset                 # wipe NVS, revert to config.h defaults
```

## BLE protocol

For building a custom BLE client, see [BLE_PROTOCOL.md](BLE_PROTOCOL.md) — UUID table, payload formats, semantics, cooldown.

## Versioning

Firmware version lives in [`src/version.h`](src/version.h) as a single `FW_VERSION` `#define` (semver `MAJOR.MINOR.PATCH`). It's printed on Serial at boot, exposed as a read-only BLE characteristic, and shown in the iOS app's Device tab and via `uv run tools/led.py get version`.

Per-release workflow:

1. Bump `FW_VERSION` in `src/version.h`.
2. Commit (`git commit -am "release v0.2.0"`).
3. Tag the commit (`git tag v0.2.0`) so the string in the code and the tag in history point at the same commit — you can always `git checkout v0.2.0` to rebuild the exact firmware on a board.
4. `pio run -t upload`, reset the board, confirm the new version on Serial or in the iOS app.
5. `git push && git push --tags` once you're happy.

## Configuration

**User tunables — `src/config.h`:**

| Define | Default | Description |
|--------|---------|-------------|
| `SCROLL_SPEED` | 60 | ms per scroll step (lower = faster) |
| `DISPLAY_INTENSITY` | 2 | LED brightness, 0–15. Idle mode (post-sign with no ambient data) dims to 0 regardless of this setting. |
| `SIGN_BREATH_MIN/MAX_INTENSITY`, `STEP_MS` | 1 / 6 / 400 | Subtle brightness pulse on static signs. Tune the three together — changing one in isolation loses the "breath" feel. |
| `TIMEZONE` | `PST8PDT,M3.2.0,M11.1.0` | POSIX TZ string |
| `NTP_SERVER` | `pool.ntp.org` | NTP host |
| `FETCH_INTERVAL_MS` | 5 min | Stock + weather refresh interval |

**Hardware — `src/main.cpp` (edit if porting to a different board):**

| Define | Default | Description |
|--------|---------|-------------|
| `MAX_DEVICES` | 4 | Number of 8x8 LED modules |
| `DIN_PIN` / `CLK_PIN` / `CS_PIN` | 6 / 4 / 5 | SPI pins to the matrix |
| `RGB_LED_PIN` | 48 | Onboard NeoPixel for fetch indicator |
