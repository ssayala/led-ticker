# `led_ticker` — Python library & CLI for the LED Ticker

`led_ticker` speaks the same BLE service as the companion mobile app, so you can drive the
device — or build your own tools — from any machine with Bluetooth. The device
advertises as `LED-Ticker-XXXX`.

## Install

```bash
pip install led-ticker      # or: uv add led-ticker
```

This installs the importable `led_ticker` package and the `led` command.
From a checkout you can also run the CLI with no install via uv:

```bash
uv run tools/led.py <cmd>   # uses ./src directly
```

## Library

```python
from led_ticker import LedTicker, scan

# Scan for available devices (returns DeviceInfo with name, address, rssi):
for d in scan():
    print(d.name, d.address, d.rssi)

# Reuse one connection for several operations:
with LedTicker(pin="482913") as d:
    d.set_tickers(["AAPL", "MSFT"])
    d.set_status("BUSY", minutes=30)
    print(d.get_version())          # -> "0.7.0"
    s = d.get_status()              # -> Status(text="BUSY", seconds=1800)

# Or a one-shot for a single call (opens and closes its own connection):
import led_ticker
led_ticker.set_mode(["stocks", "weather"])
```

`LedTicker(select=None, address=None, name_prefix="LED-Ticker", scan_timeout=4.0, timeout=15.0, pin=None)`.
By default the first `LED-Ticker-*` in range is used; if several are in range it raises
`AmbiguousDeviceError` (whose `.candidates` is a list of `DeviceInfo`). Pass `select=` to
choose a specific unit (see [Selecting a device](#selecting-a-device)), or `address=` to
target a known address directly (skips the scan). Methods raise
`ValidationError`, `AuthError`, `DeviceNotFoundError`, `AmbiguousDeviceError`, or
`ProtocolError` (all subclasses of `LedTickerError`).

`pin=None` does **not** mean "no auth" — it falls back to the same resolution the
CLI uses (`LED_TICKER_PIN` env var, then the `~/.config/led-ticker/pin` cache),
so a saved PIN is picked up automatically. Pass `pin="482913"` to override.

## Selecting a device

With more than one LED-Ticker in range, list them and target one explicitly:

```bash
uv run tools/led.py devices                 # list units: name, address, signal
uv run tools/led.py --device A1B2 status "BUSY" 30   # target by name suffix
```

`--device` matches a unit by its name suffix (the `XXXX` in `LED-Ticker-XXXX`),
its full name, or its Bluetooth address. If you run a command with several units
in range and no `--device`, an interactive terminal prompts you to choose; in a
script (no TTY) it lists the candidates and exits non-zero so you can re-run with
`--device`.

## CLI

```bash
# Sign mode
uv run tools/led.py status "BUSY" 30      # show for 30 min, then auto-clear
uv run tools/led.py status "ON AIR"       # indefinite
uv run tools/led.py status clear

# Timer mode (countdown sign — random animation at zero, then resumes ambient)
uv run tools/led.py timer 10              # 10-minute countdown
uv run tools/led.py timer cancel

# Ambient mode (subset of stocks/weather/clock, 'all', or 'none' for sign-only)
uv run tools/led.py mode stocks weather
uv run tools/led.py mode clock
uv run tools/led.py mode all
uv run tools/led.py mode none

# Power (volatile — power cycle returns to on)
uv run tools/led.py power on
uv run tools/led.py power off

# Display settings (persisted on the device)
uv run tools/led.py display                  # show current brightness + scroll speed
uv run tools/led.py display brightness 8     # brightness 0-15
uv run tools/led.py display speed 50         # scroll ms/step 20-500 (lower = faster)
uv run tools/led.py display 8 50             # both at once

# Timezone (persisted; POSIX TZ string — companion apps have a friendly picker)
uv run tools/led.py timezone                              # show current
uv run tools/led.py timezone "EST5EDT,M3.2.0,M11.1.0"     # US Eastern

# Data
uv run tools/led.py tickers AAPL TSLA NVDA SPY
uv run tools/led.py locations "47.61,-122.33,Seattle"   # lat,lon,label (look up coords online)
uv run tools/led.py apikey your-finnhub-key
uv run tools/led.py wifi My Network Name password

# Inspect
uv run tools/led.py get version           # firmware version on the device
uv run tools/led.py get wifi|apikey|tickers|status|locations|mode|power|display|timezone|version  # read other settings

# Auth
uv run tools/led.py pin 482913            # save the device's PIN locally (~/.config/led-ticker/pin)
uv run tools/led.py pin clear             # forget the saved PIN
uv run tools/led.py --pin 482913 status "BUSY" 30   # use a PIN for one call, without saving it
uv run tools/led.py pin-enforce on        # device: require PIN for writes (default after a fresh flash)
uv run tools/led.py pin-enforce off       # device: stop requiring PIN (escape hatch)

# Maintenance
uv run tools/led.py reload                # force stock refresh
uv run tools/led.py reset                 # wipe NVS, rotate PIN, revert to config.h defaults
```

Save once, every platform: run `led.py pin <6 digits>` a single time and it's replayed on each connection automatically — no per-command `--pin`, on Linux, Windows, and macOS alike. (Companion mobile apps instead use OS-level Bluetooth *bonding* — the phone prompts for the PIN once and the OS remembers it; the CLI never bonds.)

PIN resolution: every command (and the library) looks for the PIN in this order — `--pin XXXXXX` flag → `LED_TICKER_PIN` env var → the saved cache file (`~/.config/led-ticker/pin`). The env var suits CI or anyone who'd rather not write the PIN to disk. If none is found and the device has enforcement on, writes fail with a clear `AuthError`.

Stale-PIN safety: every write probes the device after sending the PIN and exits with a clear error if the PIN was rotated by a factory reset — a write never fails silently because of an out-of-date local PIN.

## License

[Apache-2.0](LICENSE) © 2026 Sunil Sayala — free to use, including commercially, with attribution. (The firmware in the parent repo is also Apache-2.0; the hardware design files are CC BY 4.0 — see the [repo root](https://github.com/ssayala/esp32-led-simple).)
