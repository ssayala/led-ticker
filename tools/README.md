# CLI tool (`led.py`)

`led.py` is a [bleak](https://github.com/hbldh/bleak)-based command-line client for the LED Ticker. It speaks the same BLE service as the iOS app, so you can drive the device from any machine with Bluetooth. The device advertises as `LED-Ticker-XXXX`.

## Requirements

The CLI runs under [**uv**](https://docs.astral.sh/uv/), which fetches its Python dependencies on first run — no manual `pip install` or virtualenv to manage.

```bash
# install uv (macOS / Linux)
curl -LsSf https://astral.sh/uv/install.sh | sh
```

Every command is then `uv run tools/led.py <cmd>` (run from the repo root).

## Commands

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

# Timezone (persisted; POSIX TZ string — the iOS app has a friendly picker)
uv run tools/led.py timezone                              # show current
uv run tools/led.py timezone "EST5EDT,M3.2.0,M11.1.0"     # US Eastern

# Data
uv run tools/led.py tickers AAPL TSLA NVDA SPY
uv run tools/led.py locations "47.61,-122.33,Seattle"   # lat,lon,label (look up coords online)
uv run tools/led.py apikey your-finnhub-key
uv run tools/led.py wifi My Network Name password

# Inspect
uv run tools/led.py get version           # firmware version on the device
uv run tools/led.py get wifi|apikey|tickers|status|locations|mode|power|display|timezone  # read other settings

# Auth
uv run tools/led.py pin 482913            # save the device's PIN locally (~/.config/led-ticker/pin)
uv run tools/led.py pin clear             # forget the saved PIN
uv run tools/led.py pin-enforce on        # device: require PIN for writes (default after a fresh flash)
uv run tools/led.py pin-enforce off       # device: stop requiring PIN (escape hatch)

# Maintenance
uv run tools/led.py reload                # force stock refresh
uv run tools/led.py reset                 # wipe NVS, rotate PIN, revert to config.h defaults
```

Stale-PIN safety: every write probes the device after sending the PIN and exits with a clear error if the PIN was rotated by a factory reset — a write never fails silently because of an out-of-date local PIN.
