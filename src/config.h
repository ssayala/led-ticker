#pragma once

// Default stock symbols — used to seed NVS on first boot.
// After that, update them via BLE: uv run tools/led.py tickers AAPL MSFT ...
const char* stockTickers[] = {"AAPL", "GOOG", "MSFT", "AMZN"};
const int stockTickerCount = sizeof(stockTickers) / sizeof(stockTickers[0]);

// Default weather locations — zip codes or "City, State/Region" strings,
// resolved to coordinates on-device via Open-Meteo's geocoding API.
// Used to seed NVS on first boot.
// After that, update them via BLE: uv run tools/led.py locations "Seattle, WA"
// 98052 ...
const char* defaultLocations[] = {"Redmond, WA", "Seattle, WA"};
const int defaultLocationCount =
    sizeof(defaultLocations) / sizeof(defaultLocations[0]);

// --- Hardware / pins (edit when porting to a different board) ---
// HARDWARE_TYPE expands at its use site in main.cpp (which includes
// MD_MAX72xx.h), so config.h doesn't need that header itself.
#define HARDWARE_TYPE MD_MAX72XX::FC16_HW
#define MAX_DEVICES 4
#define DIN_PIN 6  // MOSI
#define CLK_PIN 4  // SCK
#define CS_PIN 5
#define RGB_LED_PIN 48  // Freenove onboard WS2812
// Factory-reset button. BOOT button on most ESP32-S3 devkits is GPIO 0.
// GPIO0 is sampled by the bootloader only at hardware-reset time, so polling
// it at runtime is safe. (Hold timings live in main.cpp — they're behavior,
// not a porting knob.)
#define BUTTON_PIN 0

// --- Display behavior ---
#define SCROLL_SPEED 70  // ms per scroll step (lower = faster)
// Setup-mode scroll runs slower so the BLE device name + PIN are easier to
// read off the matrix while the user is pairing. Reverts to SCROLL_SPEED
// once the device leaves MODE_SETUP.
#define SETUP_SCROLL_SPEED 100
#define DISPLAY_INTENSITY 2  // 0–15

// Subtle brightness "breath" on static (non-scrolling) signs. The MAX7219
// is integer-only with logarithmic perception, so the wide range keeps
// individual steps as small relative jumps. Floor at 1 — intensity 0 reads
// as too dim for an at-a-glance sign. ~4 s full breath at 400 ms/step is
// close to a natural slow breath rate. Tune the three values together;
// changing one in isolation tends to lose the "breath" feel.
#define SIGN_BREATH_MIN_INTENSITY 1
#define SIGN_BREATH_MAX_INTENSITY 6
#define SIGN_BREATH_STEP_MS 400

// --- Timer mode (countdown sign) ---
// Whole-minute countdown rendered as MM:SS, then the end-of-timer animation.
#define TIMER_MAX_MINUTES 99
// End animation (non-blocking, frame-stepped like the idle pixel): a single
// looped "explosion" — a detonation flash, an expanding thick shockwave, and
// diagonal debris rays — fired repeatedly from center until the run ends.
#define ANIM_FRAME_MS 80                         // ms per animation frame
#define ANIM_CENTER_COL ((8 * MAX_DEVICES) / 2)  // 16 on a 4-module matrix
#define ANIM_CENTER_ROW 3
#define EXPLOSION_FRAMES 60         // total frames (~4.8 s at ANIM_FRAME_MS)
#define EXPLOSION_CADENCE 6         // frames between successive detonations
#define EXPLOSION_MAX_R 20          // shockwave radius that clears the matrix
#define EXPLOSION_FLASH_INTENSITY 8 // panel brightness pop on each detonation

// --- Time / NTP ---
// POSIX TZ string. Change TIMEZONE if not in US Pacific.
// The NYSE market-hours check in isMarketOpen() assumes the device clock is
// in PT and shifts by -3 to reach ET — if you change TIMEZONE you'll need to
// revisit those constants too.
#define TIMEZONE "PST8PDT,M3.2.0,M11.1.0"
#define NTP_SERVER "pool.ntp.org"

// How often loop() triggers a stocks+weather fetch (5 minutes by default).
#define FETCH_INTERVAL_MS (5 * 60 * 1000)
