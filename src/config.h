#pragma once

// Default stock symbols — used to seed NVS on first boot.
// After that, update them via BLE: uv run tools/led.py tickers AAPL MSFT ...
const char *stockTickers[] = {"AAPL", "GOOG", "MSFT", "AMZN"};
const int stockTickerCount = sizeof(stockTickers) / sizeof(stockTickers[0]);

// Default weather locations — zip codes or "City, State/Region" strings,
// resolved to coordinates on-device via Open-Meteo's geocoding API.
// Used to seed NVS on first boot.
// After that, update them via BLE: uv run tools/led.py locations "Seattle, WA" 98052 ...
const char *defaultLocations[] = {"Redmond, WA", "Seattle, WA"};
const int defaultLocationCount = sizeof(defaultLocations) / sizeof(defaultLocations[0]);

// --- Display behavior ---
#define SCROLL_SPEED 60     // ms per scroll step (lower = faster)
#define DISPLAY_INTENSITY 2 // 0–15

// --- Time / NTP ---
// POSIX TZ string. Change TIMEZONE if not in US Pacific.
// The NYSE market-hours check in isMarketOpen() assumes the device clock is
// in PT and shifts by -3 to reach ET — if you change TIMEZONE you'll need to
// revisit those constants too.
#define TIMEZONE "PST8PDT,M3.2.0,M11.1.0"
#define NTP_SERVER "pool.ntp.org"

// How often loop() triggers a stocks+weather fetch (5 minutes by default).
#define FETCH_INTERVAL_MS (5 * 60 * 1000)
