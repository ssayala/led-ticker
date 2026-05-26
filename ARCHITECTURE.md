# Architecture

Deep-dive notes for the ESP32-S3 LED ticker firmware. [`CLAUDE.md`](CLAUDE.md) has the essentials; this file has the details you need when actually editing `src/main.cpp`.

## Mode state

`enabledMask` is a subset of `BIT_STOCKS | BIT_WEATHER | BIT_CLOCK` = `MASK_ALL` (`0x0D`). Empty mask `0` means sign-only (`MODE_IDLE`).

- Bit `0x02` is reserved (was `BIT_MESSAGES`). Stripped from any legacy NVS mask on load so old configs don't resurrect a dead category.
- `parseModePayload` returns `MASK_NONE_REQUEST = 0x80` as a sentinel for `"none"`, to distinguish from invalid input (which returns `0`). `applyPendingMode` translates the sentinel back to `enabledMask = 0`.
- Mode characteristic accepts `"all"`, `"none"`, a single token, or a comma-joined subset like `"stocks,weather"`. `"messages"` is rejected. Reads return canonical form (`"all"`, `"none"`, or comma-joined).
- Persisted in NVS namespace `display`, key `mask` (one byte).

Within `MODE_CONTENT`, `currentBit` tracks the scrolling category. `advanceCategory()` rotates STOCKS → WEATHER → CLOCK, skipping bits that are disabled or have no data. A single-bit mask never advances.

## Display modes

### MODE_CONTENT
Normal ambient rotation. `showNext()` picks content based on `currentBit`.

### MODE_SETUP
Pre-config mode. Scrolls the BLE device name (`LED-Ticker-XXXX`) so the user can find which device to connect to. `setupTargetMask` records the mask to resume into.

Entered when:
- Boot with saved mask whose prereqs aren't met
- User writes a mode whose prereqs are missing
- After `cmd=reset`

Exits via `exitSetupIfReady()` (called from `applyPendingWifi`/`applyPendingApiKey`), or via an explicit `mode=` write.

**Timeout:** `SETUP_TIMEOUT_MS = 60s` of no BLE activity. Falls through to `MODE_CONTENT` *only if* `wifiConfigured()` — unmet-prereq bits then show as `"Loading X..."`. Without WiFi the timeout no-ops (and reschedules) since every category needs WiFi. Every BLE characteristic `onWrite` resets the activity timer.

### MODE_IDLE
Single pixel bouncing diagonally Pong-style around the matrix at `IDLE_STEP_MS = 150` ms/step, intensity 0, via `MD_MAX72xx::setPoint()` direct pixel access.

Entered in three cases:
1. `resumeAmbient()` after a status sign clears, when `enabledMask == 0` or `maskPrereqsReady(enabledMask)` is false
2. Boot path when `enabledMask == 0` ("none" mode persisted)
3. `applyPendingMode()` when the user writes `mode=none`

Exits via `exitSetupIfReady()` (idle resumes into existing `enabledMask`, but `enabledMask == 0` is sticky and never exits this way), via `applyPendingMode()` (any non-"none" write transitions out), or via a fresh status write (override).

`enterIdle()` is idempotent w.r.t. pixel position — re-entry preserves col/row/dir but clears the display and forces a first-paint so leftover sign content doesn't ghost.

### Static clock fast path
When `enabledMask == BIT_CLOCK` AND `timeReady` AND no status sign is active, `loop()` calls `tickStaticClock()` instead of the scroll pump. Renders steady `"H:MM"` (no colon blink), redraws only on minute change.

Before NTP completes, this branch is skipped and the scroll path renders `"Loading time..."`. In any mixed mode (`BIT_CLOCK` + another bit), clock scrolls as `"H:MM AM/PM"` via `showNextClock()` and `advanceCategory()` rotates off it immediately.

## Sign mode (Status characteristic)

UUID `...26af`. Overrides ambient when `activeStatusText` is non-empty and not expired.

- `STATUS_STATIC_MAX_CHARS = 5`. ≤5 chars renders steady via `displayText(PA_PRINT, PA_NO_EFFECT)`. Longer text scrolls on a loop.
- `statusShown` / `statusShownIsScroll` are file-scope caches so static path is one-shot per text change. Out-of-band wipes (clear/expiry/reset) go through `invalidateStatusRender()` so the next tick repaints.
- `clearActiveStatusAndResume()` calls `resumeAmbient()`, which picks content vs idle based on prereqs.

### Expiry sentinel values
- `statusExpiresAt == 0` → no status active
- `statusExpiresAt == UINT32_MAX` → indefinite (until cleared)
- Otherwise: `millis()` target value

Uses `millis()` not `time(NULL)` so timed signs work on no-WiFi devices. Compare via `(int32_t)(millis() - statusExpiresAt) < 0` for wrap safety. Computed targets that collide with sentinels (0 or UINT32_MAX) are bumped to 1.

`checkStatusForRender()` is the combined gate-plus-expiry function called once per loop iteration. Returns `false` and clears in-place if expiry has passed.

### Sign-only inference
`applyPendingStatus()` auto-persists `enabledMask = 0` on the first sign write when `!wifiConfigured() && enabledMask != 0`. Strong signal of sign-only intent — after this, power cycles land in `MODE_IDLE` instead of nag-scrolling the BLE name.

Setting WiFi creds later does **not** auto-restore categories. User must write `mode=all` (or similar) to re-enable ambient. `cmd=reset` clears the inference.

## Boot sequence (`setup()`)

```
initDisplay()
  → load NVS (wifi, apikey, tickers, locs, display)
  → buildDeviceName()
  → MODE_SETUP / MODE_IDLE / MODE_CONTENT based on mask + prereqs
  → showNext()           // display live before networking
  → connectWifi()
  → initTime()           // GATED on WL_CONNECTED
  → initBLE()
  → triggerFetch()
```

`initTime()` is **gated on `WiFi.status() == WL_CONNECTED`** and no-ops otherwise. Starting lwIP's SNTP daemon without WiFi wedges the device (pre-IDF-5 SNTP client accumulates state on failed DNS lookups and dies after ~10 minutes). When WiFi is configured later via BLE, `applyPendingWifi()` calls `initTime()` after the reconnect.

## Main loop (cooperative, no `delay()`)

```
apply pending BLE updates
  → updateStatusLed()
  → check setup timeout (gated on wifiConfigured())
  → render precedence:
      checkStatusForRender()  // sign overrides everything
      tickIdle()              // bouncing pixel
      static clock fast path
      scroll pump             // MODE_SETUP via showNextSetup(), MODE_CONTENT via showNext()
  → fetch every FETCH_INTERVAL_MS (5 min) if WiFi up
```

## BLE write → deferred apply pattern

Each characteristic callback copies the payload into a `pending*` buffer and sets a `*UpdatePending` volatile flag. `loop()` consumes these via `applyPending*()` handlers.

**Don't do heavy work (network, display) inside callbacks.**

## Cross-core safety

The fetch task runs on Core 0 but must not call `neopixelWrite()` directly. It sets the volatile `fetching` flag; `loop()` on Core 1 consumes it via `updateStatusLed()`. Same deferred pattern as BLE callbacks.

## Fetch cooldown

`BLE_FETCH_COOLDOWN_MS = 10s` gate on writes that trigger network (ticker update, locations update, `reload`, `reset`). Prevents API hammering on client retries. Status writes are **not** gated — they need to feel immediate.

## Display gating (`showNext()`)

The cycle skips category bits whose data isn't ready:
- Stocks: WiFi + API key configured AND `stockCount > 0`
- Weather: WiFi configured AND `weatherCount > 0`
- Clock: `timeReady` (NTP synced at least once)

If no enabled bit has data yet, `showNext()` shows `"Loading <category>..."` for `currentBit`.

Mode writes whose prereqs are missing get diverted to `MODE_SETUP` by `applyPendingMode`, but the mask is still saved so the device resumes into the user's selection once prereqs are met.

## Reset semantics (`cmd=reset`)

Clears *all* NVS namespaces (`wifi`, `apikey`, `tickers`, `locs`, `display`, plus tombstones `msgs` and `status`). Then:

- Zeroes in-memory `nvsWifiSsid` / `nvsWifiPass` / `nvsApiKey` / `activeStatusText` (otherwise `wifiConfigured()` / `apiKeyConfigured()` keep returning true and active sign keeps overriding until reboot)
- `WiFi.disconnect()` to drop live session
- Re-seeds tickers and locations from `config.h`
- `enabledMask = MASK_ALL`
- `invalidateStatusRender()`
- Enters `MODE_SETUP(MASK_ALL)`

After reset the device needs WiFi and API key reconfigured over BLE.

## RAM-only fetched data

Stocks (`stockQuotes[]`, raw price + changePct) and weather (`weatherReadings[]`, raw tempF) live only in RAM. Display format (`\x18`/`\x19` arrows, `°F`) is computed on every scroll in `showNextStock()` / `showNextWeather()`, so format changes take effect immediately without a reload.

## Market hours

`isMarketOpen()` (Mon-Fri 9:30-16:00 ET) gates fetches only, not display. When closed, last-fetched quotes stay in RAM and the API call is skipped. Cold boot with no data fetches once regardless.

## Geocoding

`fetchWeatherImpl()` resolves each location string to lat/lon via Open-Meteo's geocoding API on first use, caches in `resolved[]` (RAM). `applyPendingLocations()` invalidates the cache by setting `resolved[i].ok = false`. A trailing `", XX"` is used as `admin1` / `country_code` filter to disambiguate duplicate city names.

## Versioning

`FW_VERSION` is a `#define` in `src/version.h` (semver). Bumped manually per release, tagged in git. Surfaced three ways:
- Serial banner at boot (`LED-Ticker firmware vX.Y.Z`)
- Prefix on every `[hb]` heartbeat line (since USB-CDC enumeration timing means the boot banner can be missed)
- Read-only Version BLE characteristic (UUID `...26b0`)

iOS reads it on connect. **`.version` is in iOS `CharKind` but NOT in `requiredKinds`** — older firmware without the characteristic still connects cleanly (`firmwareVersion` ends up empty).

`tools/led.py get version` reads the same characteristic.

## Serial reliability

`Serial.setTxTimeoutMs(0)` immediately after `Serial.begin()`. Default 250ms blocking write was a latent matrix-stutter risk when running headless and the TX buffer filled with no host draining. `delay(2000)` wait-for-`!Serial` is only about catching the boot banner in `pio device monitor`, not correctness.
