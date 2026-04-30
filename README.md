# LED Matrix Ticker
Scrolling message, stock and weather ticker using a ESP32-S3 and a DIYables 4-in-1 MAX7219 LED matrix.

A simpler version of my other project, [esp32-led-matrix](https://github.com/ssayala/esp32-led-matrix). This version also inlcudes a custom PCB design for plugging in ESP32-S3 and LED Matrix into.



## Features

- Scrolling text display on a 32x8 LED matrix
- Live stock quotes from Finnhub API
- Live weather for multiple locations (Open-Meteo, zip or "City, State")
- No secrets at build time — WiFi credentials and API key configured via BLE and stored in NVS
- Bluetooth (BLE) control — update WiFi, API key, stock symbols, messages, and display mode wirelessly
- Companion [iOS app](ios/README.md) (SwiftUI + CoreBluetooth) mirrors the Python CLI
- Onboard RGB LED lights blue during network fetches
- All settings persist across reboots (NVS flash storage)
- Fallback messages shown until you set your own via BLE
- Prompts on display if WiFi or API key not yet configured
- Market-hours aware — skips API calls when NYSE is closed
- Auto-refreshes every 5 minutes

## Hardware

| Component | Details |
|-----------|---------|
| MCU | ESP32-S3 |
| Display | DIYables 4-in-1 MAX7219 8x8 LED matrix (SPI) |

### Wiring

| Matrix Pin | ESP32-S3 GPIO |
|------------|---------------|
| VCC | 5V |
| GND | GND |
| DIN | 6 (MOSI) |
| CLK | 4 (SCK) |
| CS | 5 |

The onboard RGB LED (GPIO 48) lights blue during network fetches.

## Custom PCB

If you prefer, you can order a custom PCB 
that carries a ESP32-S3 and MAX7219 matrix header on a single board. 

![PCB 3D render](pcb/pcb.png)

Customer PCB was designed in [EasyEDA](https://easyeda.com/). You can order this via JLCPCB or any other PCB manufacturer.  

| File | Purpose |
|------|---------|
| [`pcb/EasyEDA_PCB.json`](pcb/EasyEDA_PCB.json) | EasyEDA project source — open this to edit the schematic and layout |
| [`pcb/EasyEDA_PCB.dxf`](pcb/EasyEDA_PCB.dxf) | Board outline / mechanical drawing (DXF) |
| [`pcb/EasyEDA_PCB.obj`](pcb/EasyEDA_PCB.obj) | 3D model of the assembled board (Wavefront OBJ) |
| [`pcb/EasyEDA_PCB.mtl`](pcb/EasyEDA_PCB.mtl) | Material definitions for the OBJ model |
| [`pcb/pcb.png`](pcb/pcb.png) | 3D render shown above |

## Setup

1. Install [PlatformIO](https://platformio.org/)

2. Optionally edit `src/config.h` to set default stock symbols and fallback messages:
   ```c
   const char *stockTickers[] = {"AAPL", "GOOGL", "MSFT", "AMZN"};
   ```
   These seed NVS on first boot. After that, use the BLE tool to change them.

3. Build and upload:
   ```
   pio run -t upload
   ```

4. Monitor serial output:
   ```
   pio device monitor
   ```

5. On first boot the display will prompt you to configure WiFi and the API key via BLE:
   ```
   uv run tools/led.py wifi My Network Name password123
   uv run tools/led.py apikey your-finnhub-key
   ```
   The last argument is always the password — everything before it is the SSID, so spaces in network names work naturally.

   Get a free Finnhub API key at https://finnhub.io/register

## BLE Control

The device advertises as `LED-Ticker`. Use the included script to control it from any machine with Bluetooth:

```bash
# Install uv if needed: https://docs.astral.sh/uv/getting-started/installation/

# Set stock symbols (fetches new quotes immediately)
uv run tools/led.py tickers AAPL TSLA NVDA SPY

# Set scrolling messages (persisted across reboots)
uv run tools/led.py messages "Take a break!" "Drink water!" "Stand up!"

# Set weather locations (zip codes or "City, State" to disambiguate)
uv run tools/led.py locations "Seattle, WA" "Redmond, WA" 98052

# Switch display mode (all = round-robin across stocks, messages, weather)
uv run tools/led.py mode stocks
uv run tools/led.py mode messages
uv run tools/led.py mode weather
uv run tools/led.py mode all

# Update WiFi credentials and reconnect (SSID may contain spaces)
uv run tools/led.py wifi My Network Name password123

# Set Finnhub API key
uv run tools/led.py apikey your-finnhub-key

# Force an immediate stock quote refresh
uv run tools/led.py reload

# Clear all NVS data and revert to config.h defaults
# Note: this also clears WiFi and API key — you will need to reconfigure them
uv run tools/led.py reset
```

### BLE Service UUIDs

For building a custom app (e.g. iOS with CoreBluetooth):

| | UUID | Access |
|---|---|---|
| Service | `4fafc201-1fb5-459e-8fcc-c5c9c331914b` | — |
| Tickers | `beb5483e-36e1-4688-b7f5-ea07361b26a8` | read/write |
| Mode | `beb5483e-36e1-4688-b7f5-ea07361b26a9` | read/write |
| Messages | `beb5483e-36e1-4688-b7f5-ea07361b26aa` | read/write |
| Command | `beb5483e-36e1-4688-b7f5-ea07361b26ab` | write |
| WiFi | `beb5483e-36e1-4688-b7f5-ea07361b26ac` | read/write (read returns SSID only) |
| API Key | `beb5483e-36e1-4688-b7f5-ea07361b26ad` | read/write |
| Locations | `beb5483e-36e1-4688-b7f5-ea07361b26ae` | read/write |

Payload formats:
- **Tickers:** comma-separated symbols — `AAPL,MSFT,GOOGL`
- **Mode:** `stocks`, `messages`, `weather`, or `all` (round-robin through the other three, one full pass of each per cycle)
- **Messages:** pipe-separated strings — `Take a break!|Drink water!|Stand up!` (max 511 bytes)
- **Locations:** pipe-separated zip codes or `City, State` strings — `Seattle, WA|98052|Redmond, WA`. The device geocodes each via Open-Meteo on first fetch and caches the result; when a query contains a trailing `, XX`, the `XX` is used as a state/region filter to disambiguate duplicate city names.
- **Command:** `reload` (force stock refresh) or `reset` (clear NVS, revert to `config.h` defaults)
- **WiFi:** `SSID|password` — updates credentials, saves to NVS, reconnects immediately
- **API Key:** plain string — Finnhub API key, saved to NVS, triggers immediate stock fetch

## iOS App

A SwiftUI + CoreBluetooth companion app lives in [`ios/`](ios/README.md).
It exposes the same controls as `tools/led.py` — WiFi, API key, tickers,
messages, display mode, reload, and reset — and reads current settings
back from the device on connect so the UI reflects actual state.

See [`ios/README.md`](ios/README.md) for build instructions, XcodeGen
setup, and signing configuration.

## Configuration

Tunables at the top of `src/main.cpp`:

| Define | Default | Description |
|--------|---------|-------------|
| `SCROLL_SPEED` | 50 | ms per frame (lower = faster) |
| `FETCH_INTERVAL_MS` | 5 min | Stock quote refresh interval |
| `MAX_DEVICES` | 4 | Number of 8x8 LED modules |
| `RGB_LED_PIN` | 48 | Onboard NeoPixel for fetch indicator |
