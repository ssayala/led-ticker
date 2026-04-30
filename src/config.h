#pragma once

// Default stock symbols — used to seed NVS on first boot.
// After that, update them via BLE: uv run tools/led.py tickers AAPL MSFT ...
const char* stockTickers[] = { "AAPL", "GOOGL", "MSFT", "AMZN" };
const int   stockTickerCount = sizeof(stockTickers) / sizeof(stockTickers[0]);

// Default weather locations — zip codes or "City, State/Region" strings,
// resolved to coordinates on-device via Open-Meteo's geocoding API.
// Used to seed NVS on first boot.
// After that, update them via BLE: uv run tools/led.py locations "Seattle, WA" 98052 ...
const char* defaultLocations[] = { "Redmond, WA", "Seattle, WA" };
const int   defaultLocationCount = sizeof(defaultLocations) / sizeof(defaultLocations[0]);

// Fallback messages shown until you send your own via BLE:
//   uv run tools/led.py messages "msg1" "msg2" ...
const char* fallbackMessages[] = {
    "Hydrate Often",
    "Excercise Daily",
    "Be Positive",
    "Listen Actively",
    "Show Kindness"
};
const int fallbackCount = sizeof(fallbackMessages) / sizeof(fallbackMessages[0]);
