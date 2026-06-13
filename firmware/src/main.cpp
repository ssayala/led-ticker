#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <MD_MAX72xx.h>
#include <MD_Parola.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <SPI.h>
#include <WiFi.h>
#include <time.h>

#include "config.h"
#include "console.h"
#include "version.h"

// ============================================================================
// Mode bits
// ============================================================================
// An active status sign overrides both modes until expiry/clear.
// 0x02 was once BIT_MESSAGES — tombstoned; legacy NVS masks are stripped
// via `& MASK_ALL` on load. When BIT_CLOCK is the only enabled bit, the
// display switches to a steady non-scrolling clock — see tickStaticClock().

enum {
  MODE_CONTENT,
  MODE_SETUP,
  MODE_IDLE,
};
#define BIT_STOCKS 0x01
#define BIT_WEATHER 0x04
#define BIT_CLOCK 0x08
#define MASK_ALL (BIT_STOCKS | BIT_WEATHER | BIT_CLOCK)

// Parser sentinel for the explicit "none" mode (sign-only with idle pixel
// between signs). Distinct from 0 (which parseModePayload returns for
// invalid input) so applyPendingMode can tell them apart. Never stored in
// enabledMask — that holds 0 when "none" is the active selection.
#define MASK_NONE_REQUEST 0x80

// ============================================================================
// Hardware & display constants
// ============================================================================
// Pins, MAX_DEVICES, and HARDWARE_TYPE are the porting knobs — they live in
// config.h. HARDWARE_TYPE references MD_MAX72XX, which is in scope here
// because this file includes MD_MAX72xx.h above config.h's use site.

// Factory-reset hold timings. GPIO0 (BOOT) is only sampled at reset, so
// runtime polling is safe — but holding it while pressing RESET drops the
// chip into the ROM bootloader instead.
#define RESET_HOLD_MS 10000
#define RESET_HINT_AT_MS 2000

// Scroll buffer cell size. Also bounds STATUS_MAX_LEN.
#define MAX_STRING_LEN 96

// ============================================================================
// Forward decls of vars defined further down (so order-of-definition can
// follow the logical narrative rather than strict topological order).
// ============================================================================

// Defined in the BLE section; consumed earlier by showNextSetup().
extern char bleDeviceName[24];

// ============================================================================
// Preferences (NVS) — single shared instance, used on Core 1 only
// ============================================================================

Preferences prefs;

// Shared persistence for the string lists (tickers, locations): a "count"
// int plus indexed entries ("t0", … / "l0", …). `list` is a flat buffer
// with entry stride `entryLen`. Load seeds from defaults when NVS is empty
// or out of range, and returns the resulting count.

static void saveStringListToNVS(const char* ns, const char* keyPrefix,
                                const char* list, int entryLen, int count) {
  prefs.begin(ns, false);
  prefs.putInt("count", count);
  for (int i = 0; i < count; i++) {
    char key[8];
    snprintf(key, sizeof(key), "%s%d", keyPrefix, i);
    prefs.putString(key, list + i * entryLen);
  }
  prefs.end();
}

static int loadStringListFromNVS(const char* ns, const char* keyPrefix,
                                 char* list, int entryLen, int maxCount,
                                 const char* const* defaults, int defaultCount,
                                 const char* what) {
  prefs.begin(ns, true);
  int count = prefs.getInt("count", 0);
  if (count > 0 && count <= maxCount) {
    for (int i = 0; i < count; i++) {
      char key[8];
      snprintf(key, sizeof(key), "%s%d", keyPrefix, i);
      prefs.getString(key, list + i * entryLen, entryLen);
    }
    prefs.end();
    Serial.printf("Loaded %d %s from NVS\r\n", count, what);
    return count;
  }
  prefs.end();
  int seeded = defaultCount < maxCount ? defaultCount : maxCount;
  for (int i = 0; i < seeded; i++) {
    strncpy(list + i * entryLen, defaults[i], entryLen - 1);
    list[(i + 1) * entryLen - 1] = '\0';
  }
  saveStringListToNVS(ns, keyPrefix, list, entryLen, seeded);
  Serial.printf("Seeded %d %s from defaults\r\n", seeded, what);
  return seeded;
}

// ============================================================================
// NVS: WiFi credentials
// ============================================================================

#define WIFI_SSID_MAX 64
#define WIFI_PASS_MAX 64

char nvsWifiSsid[WIFI_SSID_MAX];
char nvsWifiPass[WIFI_PASS_MAX];

void saveWifiToNVS() {
  prefs.begin("wifi", false);
  prefs.putString("ssid", nvsWifiSsid);
  prefs.putString("pass", nvsWifiPass);
  prefs.end();
}

void loadWifiFromNVS() {
  prefs.begin("wifi", true);
  bool hasSsid = prefs.isKey("ssid");
  if (hasSsid) {
    prefs.getString("ssid", nvsWifiSsid, WIFI_SSID_MAX);
    prefs.getString("pass", nvsWifiPass, WIFI_PASS_MAX);
    Serial.printf("Loaded WiFi credentials from NVS (SSID: %s)\r\n", nvsWifiSsid);
  } else {
    nvsWifiSsid[0] = '\0';
    nvsWifiPass[0] = '\0';
    Serial.println("WiFi not configured — use BLE to set credentials");
  }
  prefs.end();
}

bool wifiConfigured() { return nvsWifiSsid[0] != '\0'; }

// ============================================================================
// NVS: Finnhub API key
// ============================================================================

#define MAX_APIKEY_LEN 64

char nvsApiKey[MAX_APIKEY_LEN];

void saveApiKeyToNVS() {
  prefs.begin("apikey", false);
  prefs.putString("key", nvsApiKey);
  prefs.end();
}

void loadApiKeyFromNVS() {
  prefs.begin("apikey", true);
  bool hasKey = prefs.isKey("key");
  if (hasKey) {
    prefs.getString("key", nvsApiKey, MAX_APIKEY_LEN);
    Serial.println("Loaded API key from NVS");
  } else {
    nvsApiKey[0] = '\0';
    Serial.println("Finnhub API key not configured — use BLE to set it");
  }
  prefs.end();
}

bool apiKeyConfigured() { return nvsApiKey[0] != '\0'; }

// ============================================================================
// NVS: BLE auth PIN
// ============================================================================
// 6-digit PIN gating writes while nvsPinEnforce is on (default). Recovery
// channels: MODE_SETUP scroll and serial at boot. `pin-enforce off` is the
// unauthenticated escape hatch.

#define PIN_LEN 6

char nvsPin[PIN_LEN + 1] = {0};
bool nvsPinEnforce = true;

void savePinToNVS() {
  prefs.begin("pin", false);
  prefs.putString("code", nvsPin);
  prefs.putBool("on", nvsPinEnforce);
  prefs.end();
}

// Persist only the enforce flag — used by `pin-enforce on/off` so toggling
// the gate doesn't rewrite the unchanged 6-byte PIN string back to flash.
void savePinEnforceToNVS() {
  prefs.begin("pin", false);
  prefs.putBool("on", nvsPinEnforce);
  prefs.end();
}

static void generateAndSavePin() {
  uint32_t n = esp_random() % 1000000UL;
  snprintf(nvsPin, sizeof(nvsPin), "%06lu", (unsigned long)n);
  savePinToNVS();
  Serial.printf("Generated new BLE auth PIN: %s\r\n", nvsPin);
}

void loadPinFromNVS() {
  prefs.begin("pin", true);
  bool hasCode = prefs.isKey("code");
  if (hasCode) {
    prefs.getString("code", nvsPin, sizeof(nvsPin));
    // Default to enforcing if the "on" key was never written (e.g. namespace
    // existed from an earlier build that only stored "code").
    nvsPinEnforce = prefs.getBool("on", true);
    Serial.printf("Loaded BLE auth PIN from NVS: %s (enforce=%d)\r\n", nvsPin,
                  nvsPinEnforce);
  } else {
    nvsPin[0] = '\0';
    nvsPinEnforce = true;
  }
  prefs.end();
}

// Separate from loadPinFromNVS so callers can defer until after initBLE():
// esp_random() has no HW entropy until the RF subsystem is up, and a PIN
// generated before that is predictable.
void ensurePinExists() {
  if (nvsPin[0]) return;
  generateAndSavePin();
}

// ============================================================================
// NVS: Stock tickers + in-RAM quote state
// ============================================================================

#define MAX_STOCKS 10
#define MAX_TICKER_LEN 16

char nvsTickers[MAX_STOCKS][MAX_TICKER_LEN];
int nvsTickerCount = 0;

struct StockQuote {
  char symbol[MAX_TICKER_LEN];
  float price;
  float changePct;
};

StockQuote stockQuotes[MAX_STOCKS];
int stockCount = 0;
int currentStock = 0;

void saveTickersToNVS() {
  saveStringListToNVS("tickers", "t", &nvsTickers[0][0], MAX_TICKER_LEN,
                      nvsTickerCount);
}

void loadTickersFromNVS() {
  nvsTickerCount = loadStringListFromNVS(
      "tickers", "t", &nvsTickers[0][0], MAX_TICKER_LEN, MAX_STOCKS,
      stockTickers, stockTickerCount, "tickers");
}

// ============================================================================
// NVS: Weather locations + in-RAM resolution & readings
// ============================================================================

#define MAX_LOCATIONS 5
#define MAX_LOCATION_LEN 48  // "lat,lon,label" entry from the client
#define MAX_LOC_NAME_LEN 24  // display label shown on the matrix

char nvsLocations[MAX_LOCATIONS][MAX_LOCATION_LEN];
int nvsLocationCount = 0;

struct ResolvedLocation {
  bool ok;
  float lat;
  float lon;
  char name[MAX_LOC_NAME_LEN];
};
ResolvedLocation resolved[MAX_LOCATIONS];

// Parses a "lat,lon,label" entry into coordinates + display label. The client
// (iOS app, or a developer via the CLI) does the geocoding; the device never
// resolves place names itself. No network. Returns false on a malformed entry
// or out-of-range coordinates, leaving out.ok false so the fetch skips it.
static bool parseLocation(const char* entry, ResolvedLocation& out) {
  out.ok = false;
  char buf[MAX_LOCATION_LEN];
  strncpy(buf, entry, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';

  char* c1 = strchr(buf, ',');
  if (!c1) return false;
  *c1 = '\0';
  char* c2 = strchr(c1 + 1, ',');
  if (!c2) return false;
  *c2 = '\0';
  const char* label = c2 + 1;
  if (label[0] == '\0') return false;

  char* end;
  float lat = strtof(buf, &end);
  if (end == buf) return false;
  float lon = strtof(c1 + 1, &end);
  if (end == c1 + 1) return false;
  if (lat < -90.0f || lat > 90.0f || lon < -180.0f || lon > 180.0f)
    return false;

  out.lat = lat;
  out.lon = lon;
  strncpy(out.name, label, MAX_LOC_NAME_LEN - 1);
  out.name[MAX_LOC_NAME_LEN - 1] = '\0';
  out.ok = true;
  return true;
}

// Re-derives resolved[] from the stored location strings. Call after locations
// load or change — cheap, no network.
static void reparseLocations() {
  for (int i = 0; i < MAX_LOCATIONS; i++) {
    resolved[i].ok = false;
    if (i < nvsLocationCount) parseLocation(nvsLocations[i], resolved[i]);
  }
}

struct WeatherReading {
  char name[MAX_LOC_NAME_LEN];
  float tempF;
};

WeatherReading weatherReadings[MAX_LOCATIONS];
int weatherCount = 0;
int currentWeather = 0;

void saveLocationsToNVS() {
  saveStringListToNVS("locs", "l", &nvsLocations[0][0], MAX_LOCATION_LEN,
                      nvsLocationCount);
}

void loadLocationsFromNVS() {
  nvsLocationCount = loadStringListFromNVS(
      "locs", "l", &nvsLocations[0][0], MAX_LOCATION_LEN, MAX_LOCATIONS,
      defaultLocations, defaultLocationCount, "locations");
  reparseLocations();
}

// ============================================================================
// NVS: Enabled-category mask
// ============================================================================

uint8_t enabledMask = MASK_ALL;

void saveDisplayMaskToNVS() {
  prefs.begin("display", false);
  prefs.putUChar("mask", enabledMask);
  prefs.end();
}

void loadDisplayMaskFromNVS() {
  prefs.begin("display", true);
  if (prefs.isKey("mask")) {
    // mask=0 is now a valid persisted value ("none" mode — sign-only with
    // idle pixel between signs), so don't fall back to MASK_ALL here.
    enabledMask = prefs.getUChar("mask", MASK_ALL) & MASK_ALL;
  }
  prefs.end();
  Serial.printf("Display mask: 0x%02X\r\n", enabledMask);
}

// ============================================================================
// NVS: Display settings (brightness + scroll speed)
// ============================================================================
// BLE-settable (Display characteristic); clamped at the apply step, so NVS
// only ever holds valid values. Shares the "display" namespace with the mask.

uint8_t displayBrightness = DISPLAY_INTENSITY;
uint16_t scrollSpeedMs = SCROLL_SPEED;

void saveDisplaySettingsToNVS() {
  prefs.begin("display", false);
  prefs.putUChar("bright", displayBrightness);
  prefs.putUShort("scroll", scrollSpeedMs);
  prefs.end();
}

void loadDisplaySettingsFromNVS() {
  prefs.begin("display", true);
  displayBrightness = prefs.getUChar("bright", DISPLAY_INTENSITY);
  scrollSpeedMs = prefs.getUShort("scroll", SCROLL_SPEED);
  prefs.end();
  Serial.printf("Display settings: brightness=%u scroll=%ums\r\n",
                displayBrightness, (unsigned)scrollSpeedMs);
}

// ============================================================================
// NVS: Timezone
// ============================================================================
// POSIX TZ string for the display clock; BLE-settable (Timezone
// characteristic), config.h TIMEZONE is the fresh-flash default. Market
// hours are computed in ET from UTC and don't depend on this.

#define MAX_TZ_LEN 64

char nvsTimezone[MAX_TZ_LEN] = TIMEZONE;

void saveTimezoneToNVS() {
  prefs.begin("time", false);
  prefs.putString("tz", nvsTimezone);
  prefs.end();
}

void loadTimezoneFromNVS() {
  prefs.begin("time", true);
  if (prefs.isKey("tz")) {
    prefs.getString("tz", nvsTimezone, MAX_TZ_LEN);
  }
  prefs.end();
  Serial.printf("Timezone: %s\r\n", nvsTimezone);
}

// ============================================================================
// Setup-mode state
// ============================================================================
// Unmet prereqs drop the display into MODE_SETUP, scrolling the BLE device
// name on a loop. `setupTargetMask` records what to resume into once
// prereqs are met (or after the inactivity timeout).

#define SETUP_TIMEOUT_MS 60000

volatile unsigned long setupLastActivityMs = 0;
uint8_t setupTargetMask = MASK_ALL;

// ============================================================================
// Active-status state (sign mode)
// ============================================================================
// Sign-mode override: a non-empty, unexpired activeStatusText renders in
// place of ambient. statusExpiresAt is a millis() target (not epoch — works
// without NTP); 0 = none, UINT32_MAX = indefinite. Wrap-safe via signed
// delta in checkStatusForRender. RAM-only — power cycle clears the sign.

// ≤5 chars renders static, longer scrolls. See tickActiveStatus().
#define STATUS_MAX_LEN MAX_STRING_LEN
#define STATUS_STATIC_MAX_CHARS 5

char activeStatusText[STATUS_MAX_LEN] = {0};
uint32_t statusExpiresAt = 0;

void clearStatus() {
  activeStatusText[0] = '\0';
  statusExpiresAt = 0;
}

// ============================================================================
// Display
// ============================================================================

MD_Parola display = MD_Parola(HARDWARE_TYPE, CS_PIN, MAX_DEVICES);

// MD_Parola stores the pointer passed to displayScroll, not a copy.
// Shared across all scrolling content (stocks/weather/clock/status/setup) —
// only one is ever active, and we overwrite only when starting the next scroll.
static char scrollBuf[MAX_STRING_LEN + 1];

void initDisplay() {
  SPI.begin(CLK_PIN, -1, DIN_PIN, CS_PIN);
  display.begin();
  display.setIntensity(displayBrightness);
  display.displayClear();
}

void scrollText(const char* msg) {
  display.displayScroll(msg, PA_LEFT, PA_SCROLL_LEFT, scrollSpeedMs);
}

// ============================================================================
// Status LED (onboard NeoPixel)
// ============================================================================
// Lit blue during network fetches. Driven from loop() via a volatile flag
// set by the fetch task on Core 0 — neopixelWrite from another core hangs.

volatile bool fetching = false;
static bool ledState = false;
// Display power: when true, suppress all rendering and force the NeoPixel
// dark. RAM-only; set via the Power BLE characteristic in the section
// further down. Lives up here so updateStatusLed() (just below) can read it.
static bool displayOff = false;

void updateStatusLed() {
  if (displayOff) {
    if (ledState) {
      neopixelWrite(RGB_LED_PIN, 0, 0, 0);
      ledState = false;
    }
    return;
  }
  if (fetching && !ledState) {
    neopixelWrite(RGB_LED_PIN, 0, 0, 20);
    ledState = true;
  } else if (!fetching && ledState) {
    neopixelWrite(RGB_LED_PIN, 0, 0, 0);
    ledState = false;
  }
}

// ============================================================================
// Time / Market hours
// ============================================================================

bool timeReady = false;

void initTime() {
  // SNTP without WiFi wedges the device on this Arduino core (failed DNS
  // retries accumulate) — the "fresh-boot freeze after tens of minutes" bug.
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Skipping NTP init — WiFi not connected");
    return;
  }

  configTzTime(nvsTimezone, NTP_SERVER_1, NTP_SERVER_2);

  Serial.println("Syncing NTP...");
  for (int i = 0; i < 20; i++) {
    struct tm t;
    if (getLocalTime(&t, 100)) {
      timeReady = true;
      Serial.printf("Time: %04d-%02d-%02d %02d:%02d local\r\n", t.tm_year + 1900,
                    t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min);
      return;
    }
    delay(500);
  }
  Serial.println("NTP sync failed, will fetch stocks anyway");
}

// True if US Eastern Time observes DST at the given UTC time (second Sunday
// of March 2:00 EST → first Sunday of November 2:00 EDT).
static bool usEasternInDst(const struct tm& u) {
  int m = u.tm_mon + 1;
  if (m < 3 || m > 11) return false;
  if (m > 3 && m < 11) return true;
  int wdayFirst = (u.tm_wday - (u.tm_mday - 1) % 7 + 7) % 7;
  if (m == 3) {
    int secondSunday = 1 + (7 - wdayFirst) % 7 + 7;
    if (u.tm_mday != secondSunday) return u.tm_mday > secondSunday;
    return u.tm_hour >= 7;  // 2:00 EST == 07:00 UTC
  }
  int firstSunday = 1 + (7 - wdayFirst) % 7;
  if (u.tm_mday != firstSunday) return u.tm_mday < firstSunday;
  return u.tm_hour < 6;  // 2:00 EDT == 06:00 UTC
}

// NYSE 9:30–16:00 ET, weekdays. Derived from UTC so the user-configurable
// display timezone has no effect here.
bool isMarketOpen() {
  if (!timeReady) return true;

  time_t now = time(nullptr);
  struct tm utc;
  gmtime_r(&now, &utc);

  time_t etEpoch = now + (usEasternInDst(utc) ? -4 : -5) * 3600;
  struct tm et;
  gmtime_r(&etEpoch, &et);

  if (et.tm_wday == 0 || et.tm_wday == 6) return false;
  int minutes = et.tm_hour * 60 + et.tm_min;
  return minutes >= 9 * 60 + 30 && minutes < 16 * 60;
}

// ============================================================================
// Network fetch (stocks + weather)
// ============================================================================
// fetchTask runs on Core 0 alongside the WiFi/BLE stack. loop() on Core 1
// triggers it via xTaskNotify; the task sets the `fetching` flag, hits both
// APIs in sequence, then clears it. commitStocks/Weather take dataMutex to
// hand off into the in-RAM arrays read by the display renderers on Core 1.

#define FETCH_TASK_STACK 8192

static SemaphoreHandle_t dataMutex = nullptr;
static TaskHandle_t fetchTaskHandle = nullptr;

static void commitStocks(const StockQuote* tmp, int count) {
  xSemaphoreTake(dataMutex, portMAX_DELAY);
  for (int i = 0; i < count; i++) stockQuotes[i] = tmp[i];
  stockCount = count;
  currentStock = 0;
  xSemaphoreGive(dataMutex);
}

static void fetchStocksImpl(bool force) {
  if (!apiKeyConfigured() || WiFi.status() != WL_CONNECTED) return;

  // Market closed: keep whatever we last fetched in RAM. Only hit the API
  // if we have no data at all (e.g. cold boot on a weekend).
  if (!force && !isMarketOpen() && stockCount > 0) return;

  StockQuote tmp[MAX_STOCKS];
  int count = 0;

  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(5000);
  // HTTP/1.0 disables chunked encoding — getStream() is the raw socket and
  // doesn't decode chunks, so ArduinoJson needs a plain body. Costs
  // keep-alive; fine at this call rate.
  http.useHTTP10(true);

  for (int i = 0; i < nvsTickerCount && count < MAX_STOCKS; i++) {
    char url[256];
    snprintf(url, sizeof(url),
             "https://finnhub.io/api/v1/quote?symbol=%s&token=%s",
             nvsTickers[i], nvsApiKey);

    http.begin(url);

    int code = http.GET();
    if (code != 200) {
      Serial.printf("Stock HTTP error: %d for %s\r\n", code, nvsTickers[i]);
      http.end();
      continue;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getStream());
    http.end();
    if (err) {
      Serial.printf("Stock JSON parse error: %s for %s\r\n", err.c_str(),
                    nvsTickers[i]);
      continue;
    }

    float current = doc["c"];
    float change = doc["dp"];

    if (current == 0) continue;

    strncpy(tmp[count].symbol, nvsTickers[i], MAX_TICKER_LEN - 1);
    tmp[count].symbol[MAX_TICKER_LEN - 1] = '\0';
    tmp[count].price = current;
    tmp[count].changePct = change;
    count++;
  }

  if (count > 0) {
    commitStocks(tmp, count);
    Serial.printf("Loaded %d stock quotes\r\n", count);
  }
}

static void commitWeather(const WeatherReading* tmp, int count) {
  xSemaphoreTake(dataMutex, portMAX_DELAY);
  for (int i = 0; i < count; i++) weatherReadings[i] = tmp[i];
  weatherCount = count;
  currentWeather = 0;
  xSemaphoreGive(dataMutex);
}

static void fetchWeatherImpl(bool force) {
  if (WiFi.status() != WL_CONNECTED || nvsLocationCount == 0) return;

  // Weather updates ~hourly upstream — no reason to poll at the stock rate.
  // force (config change) and cold boot (no data yet) bypass the throttle;
  // a fully failed fetch leaves weatherCount at 0 so the next tick retries.
  static unsigned long lastWeatherFetchMs = 0;
  if (!force && weatherCount > 0 &&
      millis() - lastWeatherFetchMs < WEATHER_INTERVAL_MS)
    return;
  lastWeatherFetchMs = millis();

  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(5000);
  // See fetchStocksImpl — HTTP/1.0 keeps getStream() chunk-free.
  http.useHTTP10(true);

  WeatherReading tmp[MAX_LOCATIONS];
  int count = 0;

  for (int i = 0; i < nvsLocationCount && count < MAX_LOCATIONS; i++) {
    if (!resolved[i].ok) continue;  // malformed "lat,lon,label" entry

    // MET Norway: global, no key. Coordinates capped at 4 decimals per MET's
    // cache rules (we already store them rounded).
    char url[160];
    snprintf(url, sizeof(url),
             "https://api.met.no/weatherapi/locationforecast/2.0/compact"
             "?lat=%.4f&lon=%.4f",
             resolved[i].lat, resolved[i].lon);

    http.begin(url);
    // MET 403s a generic UA. addHeader() ignores "User-Agent" (handled by the
    // HTTPClient itself), so it must be set via setUserAgent().
    http.setUserAgent(WEATHER_USER_AGENT);
    int code = http.GET();
    if (code != 200) {
      Serial.printf("Weather HTTP error: %d for %s\r\n", code, resolved[i].name);
      http.end();
      continue;
    }

    // The response carries ~90 timesteps; filter to just the current instant's
    // temperature so the parse stays small.
    JsonDocument filter;
    filter["properties"]["timeseries"][0]["data"]["instant"]["details"]
          ["air_temperature"] = true;
    JsonDocument doc;
    DeserializationError err = deserializeJson(
        doc, http.getStream(), DeserializationOption::Filter(filter));
    http.end();
    if (err) {
      Serial.printf("Weather JSON parse error: %s for %s\r\n", err.c_str(),
                    resolved[i].name);
      continue;
    }

    // timeseries[0] is the current instant (not a forecast period). MET reports
    // °C; the display works in °F.
    JsonVariant tempC = doc["properties"]["timeseries"][0]["data"]["instant"]
                           ["details"]["air_temperature"];
    if (tempC.isNull()) {
      Serial.printf("Weather: no temperature for %s\r\n", resolved[i].name);
      continue;
    }

    strncpy(tmp[count].name, resolved[i].name, MAX_LOC_NAME_LEN - 1);
    tmp[count].name[MAX_LOC_NAME_LEN - 1] = '\0';
    tmp[count].tempF = tempC.as<float>() * 9.0f / 5.0f + 32.0f;
    count++;
  }

  if (count > 0) {
    commitWeather(tmp, count);
    Serial.printf("Loaded %d weather entries\r\n", count);
  }
}

static void fetchTask(void*) {
  while (true) {
    uint32_t forceVal;
    xTaskNotifyWait(0, 0, &forceVal, portMAX_DELAY);
    // None mode (enabledMask == 0) → nothing to display, nothing to fetch.
    // Skip before flipping the blue-LED indicator. uint8_t reads are atomic
    // on ESP32; a racy read here at worst causes one wasted fetch cycle.
    if (enabledMask == 0) continue;
    fetching = true;
    unsigned long t0 = millis();
    Serial.printf("[fetch] start mask=0x%02X force=%lu\r\n", enabledMask,
                  (unsigned long)forceVal);
    fetchStocksImpl((bool)forceVal);
    fetchWeatherImpl((bool)forceVal);
    Serial.printf("[fetch] end took=%lums\r\n", millis() - t0);
    fetching = false;
  }
}

void triggerFetch(bool force = false) {
  xTaskNotify(fetchTaskHandle, (uint32_t)force, eSetValueWithOverwrite);
}

// ============================================================================
// WiFi connect
// ============================================================================

void connectWifi() {
  if (!wifiConfigured() || WiFi.status() == WL_CONNECTED) return;

  Serial.printf("Connecting to %s...\r\n", nvsWifiSsid);
  WiFi.begin(nvsWifiSsid, nvsWifiPass);

  for (int attempts = 0; WiFi.status() != WL_CONNECTED && attempts < 20;
       attempts++) {
    delay(500);
  }

  if (WiFi.status() == WL_CONNECTED) {
    IPAddress ip = WiFi.localIP();
    Serial.printf("Connected, IP: %u.%u.%u.%u\r\n", ip[0], ip[1], ip[2], ip[3]);
  } else {
    Serial.println("WiFi failed");
  }
}

// ============================================================================
// Display rotation & rendering
// ============================================================================
// MODE_CONTENT cycles enabled categories; MODE_SETUP scrolls the BLE name
// while prereqs are missing; MODE_IDLE is the quiet bouncing pixel after a
// sign clears with prereqs still unmet. Sign mode overrides all three.

int currentMode = MODE_CONTENT;

// Which category bit is currently scrolling within MODE_CONTENT. Always one
// of BIT_STOCKS / BIT_WEATHER / BIT_CLOCK. Advances when the active category
// wraps so each enabled category gets a full pass per cycle.
uint8_t currentBit = BIT_STOCKS;

// --- Per-category renderers ---

void showNextStock() {
  StockQuote q;
  xSemaphoreTake(dataMutex, portMAX_DELAY);
  q = stockQuotes[currentStock];
  currentStock = (currentStock + 1) % stockCount;
  xSemaphoreGive(dataMutex);

  const char* arrow = q.changePct >= 0 ? "\x18" : "\x19";
  snprintf(scrollBuf, sizeof(scrollBuf), "%s $%.2f %s", q.symbol, q.price,
           arrow);
  scrollText(scrollBuf);
}

void showNextWeather() {
  WeatherReading w;
  xSemaphoreTake(dataMutex, portMAX_DELAY);
  w = weatherReadings[currentWeather];
  currentWeather = (currentWeather + 1) % weatherCount;
  xSemaphoreGive(dataMutex);

  snprintf(scrollBuf, sizeof(scrollBuf),
           "%s %.0f\xB0"
           "F",
           w.name, w.tempF);
  scrollText(scrollBuf);
}

// 12-hour HH:MM AM/PM. Used by the scrolling rotation when BIT_CLOCK shares
// the mask with other categories. Single-clock mode uses tickStaticClock().
void showNextClock() {
  struct tm t;
  if (!getLocalTime(&t, 50)) {
    scrollText("Loading time...");
    return;
  }
  int h = t.tm_hour;
  const char* ampm = (h >= 12) ? "PM" : "AM";
  int h12 = h % 12;
  if (h12 == 0) h12 = 12;
  snprintf(scrollBuf, sizeof(scrollBuf), "%d:%02d %s", h12, t.tm_min, ampm);
  scrollText(scrollBuf);
}

// Steady "H:MM", only when enabledMask == BIT_CLOCK alone. Drives the
// display directly, bypassing the scroll pump in loop(). File-scope cache
// so a timezone change can force an immediate repaint (same-minute hour
// jumps would otherwise sit stale for up to a minute).
static int staticClockLastMin = -1;

void tickStaticClock() {
  struct tm t;
  if (!getLocalTime(&t, 0)) return;
  if (staticClockLastMin == t.tm_min) return;

  int h12 = t.tm_hour % 12;
  if (h12 == 0) h12 = 12;

  snprintf(scrollBuf, sizeof(scrollBuf), "%d:%02d", h12, t.tm_min);

  display.displayClear();
  display.displayText(scrollBuf, PA_CENTER, 0, 0, PA_PRINT, PA_NO_EFFECT);
  display.displayAnimate();

  staticClockLastMin = t.tm_min;
}

// MODE_IDLE: a single pixel bouncing Pong-style around the matrix — quiet
// "alive" signal when there's nothing to show.
#define IDLE_STEP_MS 150
#define IDLE_COL_MAX (8 * MAX_DEVICES - 1)
#define IDLE_ROW_MAX 7

static int idlePixelCol = 0;
static int idlePixelRow = 0;
static int idlePixelDirX = 1;
static int idlePixelDirY = 1;
static unsigned long idleLastStepMs = 0;
static bool idleNeedsFirstPaint = false;

void tickIdle() {
  MD_MAX72XX* mx = display.getGraphicObject();
  unsigned long now = millis();

  if (idleNeedsFirstPaint) {
    mx->clear();
    mx->setPoint(idlePixelRow, idlePixelCol, true);
    idleLastStepMs = now;
    idleNeedsFirstPaint = false;
    return;
  }

  if (now - idleLastStepMs < IDLE_STEP_MS) return;
  idleLastStepMs = now;

  mx->setPoint(idlePixelRow, idlePixelCol, false);

  idlePixelCol += idlePixelDirX;
  if (idlePixelCol >= IDLE_COL_MAX) {
    idlePixelCol = IDLE_COL_MAX;
    idlePixelDirX = -1;
  } else if (idlePixelCol <= 0) {
    idlePixelCol = 0;
    idlePixelDirX = 1;
  }

  idlePixelRow += idlePixelDirY;
  if (idlePixelRow >= IDLE_ROW_MAX) {
    idlePixelRow = IDLE_ROW_MAX;
    idlePixelDirY = -1;
  } else if (idlePixelRow <= 0) {
    idlePixelRow = 0;
    idlePixelDirY = 1;
  }

  mx->setPoint(idlePixelRow, idlePixelCol, true);
}

// --- Category rotation helpers ---

static bool stocksAvailable() {
  return wifiConfigured() && apiKeyConfigured() && stockCount > 0;
}

static bool weatherAvailable() { return wifiConfigured() && weatherCount > 0; }

static bool bitHasData(uint8_t b) {
  if (b == BIT_STOCKS) return stocksAvailable();
  if (b == BIT_WEATHER) return weatherAvailable();
  if (b == BIT_CLOCK) return timeReady;  // NTP must have synced at least once
  return false;
}

static uint8_t nextBit(uint8_t b) {
  if (b == BIT_STOCKS) return BIT_WEATHER;
  if (b == BIT_WEATHER) return BIT_CLOCK;
  return BIT_STOCKS;
}

// Advance currentBit to the next enabled bit that has data. If no enabled
// bit has data, leave currentBit unchanged so showNext can show a loading hint.
static void advanceCategory() {
  uint8_t start = currentBit;
  for (int i = 0; i < 3; i++) {
    currentBit = nextBit(currentBit);
    if ((enabledMask & currentBit) && bitHasData(currentBit)) {
      if (currentBit == BIT_STOCKS)
        currentStock = 0;
      else if (currentBit == BIT_WEATHER)
        currentWeather = 0;
      // BIT_CLOCK has no per-item counter — one item per pass.
      return;
    }
  }
  currentBit = start;
}

static uint8_t firstActiveBit() {
  const uint8_t order[] = {BIT_STOCKS, BIT_WEATHER, BIT_CLOCK};
  for (uint8_t b : order)
    if ((enabledMask & b) && bitHasData(b)) return b;
  for (uint8_t b : order)
    if (enabledMask & b) return b;
  return BIT_CLOCK;
}

// --- Mode transitions ---

// Every enter*() calls resetDisplay() and re-asserts displayBrightness (the
// breath/flash animations may have left another level). Re-entering IDLE
// while already idle preserves pixel position — no snap-back to col 0.

// displayClear() blanks the LED buffer but does NOT reset MD_Parola's
// animation FSM — a mid-flight scroll would resume the *previous* message
// after a mode switch. Resetting the FSM against an empty static string
// makes the next displayAnimate() complete immediately so fresh content
// lands on the next tick. ("" must outlive the call; MD_Parola keeps the
// pointer.)
static void resetDisplay() {
  display.displayClear();
  display.setTextBuffer("");
  display.displayReset();
}

void enterContent() {
  currentMode = MODE_CONTENT;
  currentStock = 0;
  currentWeather = 0;
  currentBit = firstActiveBit();
  display.setIntensity(displayBrightness);
  resetDisplay();
}

bool maskPrereqsReady(uint8_t mask) {
  if ((mask & BIT_STOCKS) && (!wifiConfigured() || !apiKeyConfigured()))
    return false;
  if ((mask & BIT_WEATHER) && !wifiConfigured()) return false;
  if ((mask & BIT_CLOCK) && !wifiConfigured()) return false;
  return true;
}

void enterSetup(uint8_t targetMask) {
  currentMode = MODE_SETUP;
  setupTargetMask = targetMask ? targetMask : MASK_ALL;
  setupLastActivityMs = millis();
  display.setIntensity(displayBrightness);
  resetDisplay();
}

void enterIdle() {
  // Re-entry keeps pixel position but still clears + repaints, wiping any
  // sign that overrode idle and just cleared.
  bool wasIdle = (currentMode == MODE_IDLE);
  currentMode = MODE_IDLE;
  if (!wasIdle) {
    idlePixelCol = 0;
    idlePixelRow = 0;
    idlePixelDirX = 1;
    idlePixelDirY = 1;
  }
  idleLastStepMs = 0;
  idleNeedsFirstPaint = true;
  display.setIntensity(displayBrightness);
  resetDisplay();
}

// Mode string for BLE reads/logs: "setup", "none", "all", or a comma list.
// MODE_IDLE reports the underlying enabledMask (the categories that will
// rotate once prereqs are met); "none" is the explicit persisted selection.
static int formatModeName(char* buf, size_t bufLen) {
  if (currentMode == MODE_SETUP) return snprintf(buf, bufLen, "setup");
  if (enabledMask == 0) return snprintf(buf, bufLen, "none");
  if (enabledMask == MASK_ALL) return snprintf(buf, bufLen, "all");
  int len = 0;
  const char* sep = "";
  if (enabledMask & BIT_STOCKS) {
    len += snprintf(buf + len, bufLen - len, "%sstocks", sep);
    sep = ",";
  }
  if (enabledMask & BIT_WEATHER) {
    len += snprintf(buf + len, bufLen - len, "%sweather", sep);
    sep = ",";
  }
  if (enabledMask & BIT_CLOCK) {
    len += snprintf(buf + len, bufLen - len, "%sclock", sep);
    sep = ",";
  }
  return len;
}

void exitSetupIfReady() {
  // SETUP resumes into setupTargetMask, IDLE into enabledMask, once prereqs
  // are met. Explicit mask=0 ("none") in IDLE is sticky — never exits.
  if (currentMode == MODE_SETUP) {
    if (!maskPrereqsReady(setupTargetMask)) return;
    enabledMask = setupTargetMask;
    saveDisplayMaskToNVS();
  } else if (currentMode == MODE_IDLE) {
    if (enabledMask == 0 || !maskPrereqsReady(enabledMask)) return;
  } else {
    return;
  }
  enterContent();
  char buf[64];
  formatModeName(buf, sizeof(buf));
  Serial.printf("Prereqs satisfied, exiting to %s\r\n", buf);
}

void showNextSetup() {
  // The setup scroll is the PIN's recovery channel; 3+3 digit grouping
  // (assumes PIN_LEN 6). Static buf — MD_Parola keeps the pointer.
  static char buf[64];
  static char lastBuiltPin[PIN_LEN + 1] = {0};
  if (strncmp(lastBuiltPin, nvsPin, sizeof(lastBuiltPin)) != 0) {
    if (nvsPin[0]) {
      snprintf(buf, sizeof(buf), "%s  v%s  PIN %.3s %.3s", bleDeviceName,
               FW_VERSION, nvsPin, nvsPin + 3);
    } else {
      snprintf(buf, sizeof(buf), "%s  v%s", bleDeviceName, FW_VERSION);
    }
    strncpy(lastBuiltPin, nvsPin, sizeof(lastBuiltPin) - 1);
    lastBuiltPin[sizeof(lastBuiltPin) - 1] = '\0';
  }
  // Bypasses scrollText() to use the slower SETUP_SCROLL_SPEED.
  display.displayScroll(buf, PA_LEFT, PA_SCROLL_LEFT, SETUP_SCROLL_SPEED);
}

void showNext() {
  if (currentMode == MODE_SETUP) {
    showNextSetup();
    return;
  }
  // MODE_IDLE renders via tickIdle() in loop(), never through the scroll
  // pump. Bail so a stray showNext() doesn't clobber the bouncing pixel.
  if (currentMode == MODE_IDLE) return;

  // Defensive: an out-of-mask currentBit can occur after a mask change.
  if (!(enabledMask & currentBit)) currentBit = firstActiveBit();

  // If the current bit has no data right now (pre-first-fetch, WiFi drop),
  // slide to an enabled bit that does. If none do, show a loading hint.
  if (!bitHasData(currentBit)) {
    advanceCategory();
    if (!bitHasData(currentBit)) {
      if (currentBit == BIT_STOCKS)
        scrollText("Loading stocks...");
      else if (currentBit == BIT_WEATHER)
        scrollText("Loading weather...");
      else if (currentBit == BIT_CLOCK)
        scrollText("Loading time...");
      return;
    }
  }

  if (currentBit == BIT_STOCKS) {
    showNextStock();
    if (currentStock == 0) advanceCategory();
  } else if (currentBit == BIT_WEATHER) {
    showNextWeather();
    if (currentWeather == 0) advanceCategory();
  } else if (currentBit == BIT_CLOCK) {
    showNextClock();
    advanceCategory();  // one item per pass — always rotate after showing
  }
}

// --- Active-status rendering ---
// ≤ STATUS_STATIC_MAX_CHARS renders steady, longer loops a scroll.
// `statusShown` caches what's on the matrix to skip redundant redraws;
// anything that wipes the display out-of-band must call
// invalidateStatusRender() or the next tick no-ops on stale equality.

static char statusShown[STATUS_MAX_LEN] = "";
static bool statusShownIsScroll = false;

static void invalidateStatusRender() { statusShown[0] = '\0'; }

// After a sign clears: content if a mask is enabled and prereqs are met,
// else idle (never back into the setup scroll or a "Loading…" loop).
static void resumeAmbient() {
  if (enabledMask != 0 && maskPrereqsReady(enabledMask))
    enterContent();
  else
    enterIdle();
}

// Returns true if the status should render now; an expired timed status is
// cleared in-place (caller falls through to the normal render path).
bool checkStatusForRender() {
  if (activeStatusText[0] == '\0') return false;
  if (statusExpiresAt == 0 || statusExpiresAt == UINT32_MAX)
    return true;  // 0 is defensive (shouldn't happen w/ text non-empty);
                  // UINT32_MAX = indefinite
  // Signed-delta is wrap-safe: if statusExpiresAt is still in the future,
  // (millis() - statusExpiresAt) underflows to a large unsigned which cast
  // to int32_t is negative.
  if ((int32_t)(millis() - statusExpiresAt) < 0) return true;

  Serial.printf("Status: \"%s\" expired, clearing\r\n", activeStatusText);
  clearStatus();
  invalidateStatusRender();
  resumeAmbient();  // enter*() helpers clear the display on transition
  return false;
}

static void clearActiveStatusAndResume() {
  clearStatus();
  invalidateStatusRender();
  resumeAmbient();  // enter*() helpers clear the display on transition
}

// Static signs "breathe": intensity dips up to SIGN_BREATH_AMPLITUDE below
// displayBrightness and recovers, one step per SIGN_BREATH_STEP_MS.
// Scrolling signs already have motion — steady brightness.
static int signBreathLevel = DISPLAY_INTENSITY;
static int signBreathDir = -1;
static unsigned long signBreathStepMs = 0;

void tickActiveStatus() {
  if (strcmp(statusShown, activeStatusText) != 0) {
    strncpy(statusShown, activeStatusText, sizeof(statusShown) - 1);
    statusShown[sizeof(statusShown) - 1] = '\0';
    statusShownIsScroll = strlen(activeStatusText) > STATUS_STATIC_MAX_CHARS;
    // Restore steady brightness in case the previous sign was mid-breath.
    display.setIntensity(displayBrightness);
    display.displayClear();
    if (statusShownIsScroll) {
      strncpy(scrollBuf, activeStatusText, sizeof(scrollBuf) - 1);
      scrollBuf[sizeof(scrollBuf) - 1] = '\0';
      display.displayScroll(scrollBuf, PA_LEFT, PA_SCROLL_LEFT, scrollSpeedMs);
    } else {
      display.displayText(activeStatusText, PA_CENTER, 0, 0, PA_PRINT,
                          PA_NO_EFFECT);
      display.displayAnimate();
      // Fresh sign: start the breath at full brightness, dipping first.
      signBreathLevel = displayBrightness;
      signBreathDir = -1;
      signBreathStepMs = millis();
    }
  }

  if (statusShownIsScroll) {
    // Scroll path: just pump the animation. No breathing.
    if (display.displayAnimate())
      display.displayReset();  // loop the same scroll
    return;
  }

  // Static path: step the breathing intensity when the timer elapses.
  unsigned long now = millis();
  if (now - signBreathStepMs < SIGN_BREATH_STEP_MS) return;
  signBreathStepMs = now;
  // Bounds recomputed each step so a mid-sign brightness change re-clamps.
  int breathTop = displayBrightness;
  int breathFloor =
      breathTop > SIGN_BREATH_AMPLITUDE ? breathTop - SIGN_BREATH_AMPLITUDE : 0;
  signBreathLevel += signBreathDir;
  if (signBreathLevel >= breathTop) {
    signBreathLevel = breathTop;
    signBreathDir = -1;
  } else if (signBreathLevel <= breathFloor) {
    signBreathLevel = breathFloor;
    signBreathDir = 1;
  }
  display.setIntensity(signBreathLevel);
}

// --- Timer mode (countdown sign) ---
// Minute-granular countdown: MM:SS, then the explosion animation and a
// blank hold (EXPLOSION_END_HOLD_MS) before ambient resumes. Mutually
// exclusive with the text sign — each cancels the other. RAM-only.
enum TimerPhase { TIMER_OFF, TIMER_RUN, TIMER_ANIM };
static TimerPhase timerPhase = TIMER_OFF;
static uint32_t timerEndAt = 0;     // millis() target for 0:00
static int lastShownTimerSec = -1;  // redraw cache (avoid per-tick repaint)

static int animFrame = -1;  // -1 = needs first paint
static unsigned long animStepMs = 0;

// Clears any text sign so the post-timer resume goes to ambient.
static void startTimer(uint32_t minutes) {
  if (minutes < 1) minutes = 1;
  if (minutes > TIMER_MAX_MINUTES) minutes = TIMER_MAX_MINUTES;
  activeStatusText[0] = '\0';
  statusExpiresAt = 0;
  invalidateStatusRender();
  timerEndAt = millis() + minutes * 60UL * 1000UL;
  lastShownTimerSec = -1;
  timerPhase = TIMER_RUN;
  display.displayClear();
  display.setIntensity(displayBrightness);
  Serial.printf("Timer: started for %lu min\r\n", (unsigned long)minutes);
}

static void cancelTimer() {
  timerPhase = TIMER_OFF;
  resumeAmbient();  // enter*() helpers clear the display on transition
}

// Draw a hollow diamond ring (Manhattan distance == r) — the shockwave front.
static void drawRing(MD_MAX72XX* mx, int r) {
  for (int row = 0; row <= IDLE_ROW_MAX; row++)
    for (int col = 0; col <= IDLE_COL_MAX; col++) {
      int d = abs(col - ANIM_CENTER_COL) + abs(row - ANIM_CENTER_ROW);
      if (d == r) mx->setPoint(row, col, true);
    }
}

// Draw a filled diamond (Manhattan distance <= r) — the detonation core flash.
static void drawDiamondFill(MD_MAX72XX* mx, int r) {
  for (int row = 0; row <= IDLE_ROW_MAX; row++)
    for (int col = 0; col <= IDLE_COL_MAX; col++)
      if (abs(col - ANIM_CENTER_COL) + abs(row - ANIM_CENTER_ROW) <= r)
        mx->setPoint(row, col, true);
}

// Diagonal debris riding just ahead of the shockwave, giving the blast its
// spiky star shape (the 4 cardinal points already sit on the diamond ring).
static void drawDebris(MD_MAX72XX* mx, int dist) {
  static const int8_t dx[4] = {1, 1, -1, -1};
  static const int8_t dy[4] = {1, -1, 1, -1};
  for (int i = 0; i < 4; i++) {
    int px = ANIM_CENTER_COL + dx[i] * dist;
    int py = ANIM_CENTER_ROW + dy[i] * dist;
    if (px >= 0 && px <= IDLE_COL_MAX && py >= 0 && py <= IDLE_ROW_MAX)
      mx->setPoint(py, px, true);
  }
}

// One looped explosion: detonations fire from center every EXPLOSION_CADENCE
// frames; each throws a 2-px-thick diamond shockwave plus diagonal debris
// outward, led by a bright core flash. The panel brightness pops on each
// detonation frame for punch. Fully deterministic — no randomness.
static void drawExplosion(MD_MAX72XX* mx, int f) {
  mx->clear();
  bool detonating = false;

  // Latest frame a blast may start and still expand fully before the run
  // ends — the last shockwave clears the matrix instead of hard-cutting.
  // Clamp to 0 so a too-short run still fires the opening blast.
  int lastBlastStart = EXPLOSION_FRAMES - 1 - EXPLOSION_MAX_R;
  if (lastBlastStart < 0) lastBlastStart = 0;

  for (int start = 0; start <= f && start <= lastBlastStart;
       start += EXPLOSION_CADENCE) {
    int age = f - start;
    if (age == 0) {  // detonation: bright filled core + panel flash
      drawDiamondFill(mx, 3);
      detonating = true;
    } else if (age == 1) {
      drawDiamondFill(mx, 1);
    }
    if (age >= 1 && age <= EXPLOSION_MAX_R) {  // thick expanding shockwave
      drawRing(mx, age);
      drawRing(mx, age - 1);
      drawDebris(mx, age + 1);  // debris leads the wave
    }
  }

  display.setIntensity(detonating ? EXPLOSION_FLASH_INTENSITY
                                  : displayBrightness);
}

// End-animation pump. Frame-stepped via millis() like tickIdle() — no delay().
// Plays the looped explosion, holds a blank matrix for EXPLOSION_END_HOLD_MS,
// then resumes ambient.
static void tickEndAnim() {
  MD_MAX72XX* mx = display.getGraphicObject();
  unsigned long now = millis();

  // Hold phase: animStepMs was last reset when the final frame landed, so
  // it doubles as the hold-start timestamp.
  if (animFrame >= EXPLOSION_FRAMES) {
    if (now - animStepMs < EXPLOSION_END_HOLD_MS) return;
    timerPhase = TIMER_OFF;
    resumeAmbient();
    return;
  }

  if (animFrame < 0) {
    display.displayClear();  // reset Parola zones before raw setPoint() use
    display.setIntensity(displayBrightness);
    mx->clear();
    animFrame = 0;
    animStepMs = now;
  } else {
    if (now - animStepMs < ANIM_FRAME_MS) return;
    animStepMs = now;
    animFrame++;
    if (animFrame >= EXPLOSION_FRAMES) {
      // Blank explicitly — a stray debris pixel would sit lit all pause.
      mx->clear();
      return;
    }
  }

  drawExplosion(mx, animFrame);
}

// Render pump for an active timer. Called from loop() with top precedence
// (timer overrides text sign and ambient) whenever timerPhase != TIMER_OFF.
void tickTimer() {
  if (timerPhase == TIMER_ANIM) {
    tickEndAnim();
    return;
  }

  // TIMER_RUN
  unsigned long now = millis();
  int32_t remainMs = (int32_t)(timerEndAt - now);  // wrap-safe signed delta
  if (remainMs <= 0) {
    timerPhase = TIMER_ANIM;
    animFrame = -1;
    Serial.println("Timer: done, playing explosion");
    return;
  }

  // Ceil to whole seconds so a fresh N-minute timer shows "N:00" and the
  // last visible value is "0:01" (we switch to explosion at 0).
  int totalSec = (remainMs + 999) / 1000;
  if (totalSec == lastShownTimerSec) return;  // redraw only on change
  lastShownTimerSec = totalSec;

  // static: MD_Parola keeps the pointer — a stack-local would dangle and
  // re-render as garbage on the next FSM step.
  static char buf[6];  // "MM:SS" + NUL, max "99:00"
  snprintf(buf, sizeof(buf), "%d:%02d", totalSec / 60, totalSec % 60);
  display.displayText(buf, PA_CENTER, 0, 0, PA_PRINT, PA_NO_EFFECT);
  display.displayAnimate();
}

// ============================================================================
// BLE
// ============================================================================
// Wire protocol documented in BLE_PROTOCOL.md. Each BLE characteristic uses
// the deferred-apply pattern: the onWrite callback (Core 0 / NimBLE task)
// stashes the payload into a pending* buffer and sets a *UpdatePending flag;
// loop() (Core 1) consumes those flags via applyPending*() so heavy work
// stays out of the callback context.

#define BLE_DEVICE_NAME "LED-Ticker"
#define BLE_SERVICE_UUID "4fafc201-1fb5-459e-8fcc-c5c9c331914b"

// Minimum ms between writes that trigger network activity (tickers/locations
// updates, reload, reset). Status writes are not gated — they need to feel
// immediate.
#define BLE_FETCH_COOLDOWN_MS 10000
static volatile unsigned long lastBLEFetchMs = 0;

char bleDeviceName[24];

// Suffix MAC bytes so multiple units on the same bench are distinguishable.
void buildDeviceName() {
  uint64_t mac = ESP.getEfuseMac();
  snprintf(bleDeviceName, sizeof(bleDeviceName), "%s-%02X%02X", BLE_DEVICE_NAME,
           (uint8_t)((mac >> 8) & 0xFF), (uint8_t)(mac & 0xFF));
}

// ----------------------------------------------------------------------------
// BLE auth: per-connection PIN gate
// ----------------------------------------------------------------------------
// Per-connection auth slots; a slot turns authed via bonding or a correct
// Auth-characteristic PIN write. isConnAuthed() short-circuits to true
// while enforcement is off. 5 wrong PINs → 5 s lockout on that slot.

#define AUTH_MAX_CONNS 4
#define AUTH_FAIL_THRESHOLD 5
#define AUTH_LOCKOUT_MS 5000

struct AuthSlot {
  uint16_t handle;
  bool inUse;
  bool authed;
  uint8_t failCount;
  uint32_t lockoutUntilMs;
};
static AuthSlot authSlots[AUTH_MAX_CONNS];

static AuthSlot* findSlot(uint16_t handle) {
  for (int i = 0; i < AUTH_MAX_CONNS; i++) {
    if (authSlots[i].inUse && authSlots[i].handle == handle)
      return &authSlots[i];
  }
  return nullptr;
}

static AuthSlot* allocSlot(uint16_t handle) {
  for (int i = 0; i < AUTH_MAX_CONNS; i++) {
    if (!authSlots[i].inUse) {
      authSlots[i] = {handle, true, false, 0, 0};
      return &authSlots[i];
    }
  }
  return nullptr;
}

bool isConnAuthed(uint16_t handle) {
  if (!nvsPinEnforce) return true;
  AuthSlot* s = findSlot(handle);
  return s && s->authed;
}

// NimBLE stops advertising on connect and does NOT auto-resume on
// disconnect — onDisconnect must restart it or the device goes invisible
// until reboot. Each connect requests encryption; bonded peers auth
// silently, decliners stay connected but unauthed (PIN/Auth path).
class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer*, ble_gap_conn_desc* desc) override {
    AuthSlot* s = allocSlot(desc->conn_handle);
    Serial.printf("BLE: client connected (handle=%u, slot=%s, encrypted=%d)\r\n",
                  desc->conn_handle, s ? "ok" : "FULL",
                  desc->sec_state.encrypted);
    if (s && desc->sec_state.encrypted) {
      s->authed = true;  // returning bonded peer
    } else {
      // Non-blocking; outcome arrives via onAuthenticationComplete. A
      // central that refuses simply stays unauthed.
      NimBLEDevice::startSecurity(desc->conn_handle);
    }
  }
  void onDisconnect(NimBLEServer*, ble_gap_conn_desc* desc) override {
    AuthSlot* s = findSlot(desc->conn_handle);
    if (s) s->inUse = false;
    Serial.println("BLE: client disconnected, resuming advertising");
    NimBLEDevice::startAdvertising();
  }
  void onAuthenticationComplete(ble_gap_conn_desc* desc) override {
    Serial.printf("BLE auth: pairing complete (handle=%u, encrypted=%d)\r\n",
                  desc->conn_handle, desc->sec_state.encrypted);
    AuthSlot* s = findSlot(desc->conn_handle);
    if (desc->sec_state.encrypted) {
      if (s) s->authed = true;
      return;
    }
    // Pairing failed/cancelled: drop the connection so the iOS app retries
    // with a fresh pair prompt, instead of silently eating every write.
    // Exception: a client already authed via the PIN path stays.
    if (s && s->authed) return;
    Serial.printf(
        "BLE auth: pairing not completed and no PIN auth — "
        "dropping conn=%u\r\n",
        desc->conn_handle);
    NimBLEDevice::getServer()->disconnect(desc->conn_handle);
  }
  uint32_t onPassKeyRequest() override {
    // SMP compares this against what the user typed on iOS.
    uint32_t key = (uint32_t)strtoul(nvsPin, nullptr, 10);
    Serial.printf("BLE auth: passkey requested, serving %06u\r\n", (unsigned)key);
    return key;
  }
};

// Copy a write payload into the pending* buffer and flip its flag. Rejects
// empty or overrunning payloads (>= bufLen leaves room for the NUL);
// returns true on stash so callers can track post-stash state.
static bool stashBleWrite(NimBLECharacteristic* pChar, char* buf, size_t bufLen,
                          volatile bool& pendingFlag) {
  std::string val = pChar->getValue();
  if (val.length() == 0 || val.length() >= bufLen) return false;
  memcpy(buf, val.c_str(), val.length());
  buf[val.length()] = '\0';
  pendingFlag = true;
  return true;
}

// Shared onWrite (auth gate + activity stamp + stash) for characteristics
// with no extra write policy. Ones that need more — cooldown (Tickers,
// Locations), empty-as-clear (Status), payload-dependent (Cmd), Auth —
// hand-roll their callbacks and must gate manually.
class GatedStashCallbacks : public NimBLECharacteristicCallbacks {
 public:
  GatedStashCallbacks(const char* label, char* buf, size_t bufLen,
                      volatile bool& pendingFlag)
      : label(label), buf(buf), bufLen(bufLen), pendingFlag(pendingFlag) {}

  void onWrite(NimBLECharacteristic* pChar, ble_gap_conn_desc* desc) override {
    if (!isConnAuthed(desc->conn_handle)) {
      Serial.printf("BLE %s: unauthed, ignoring\r\n", label);
      return;
    }
    setupLastActivityMs = millis();
    stashBleWrite(pChar, buf, bufLen, pendingFlag);
  }

 private:
  const char* label;
  char* buf;
  size_t bufLen;
  volatile bool& pendingFlag;
};

// ----------------------------------------------------------------------------
// BLE: WiFi
// ----------------------------------------------------------------------------

#define BLE_WIFI_CHAR_UUID "beb5483e-36e1-4688-b7f5-ea07361b26ac"
#define BLE_WIFI_BUF_LEN (WIFI_SSID_MAX + WIFI_PASS_MAX + 1)

volatile bool wifiUpdatePending = false;
char pendingWifiStr[BLE_WIFI_BUF_LEN];

class WifiCallbacks : public GatedStashCallbacks {
 public:
  WifiCallbacks()
      : GatedStashCallbacks("wifi", pendingWifiStr, BLE_WIFI_BUF_LEN,
                            wifiUpdatePending) {}

  void onRead(NimBLECharacteristic* pChar) override {
    // Return SSID only — never expose the password over BLE
    pChar->setValue((uint8_t*)nvsWifiSsid, strlen(nvsWifiSsid));
  }
};

void applyPendingWifi() {
  wifiUpdatePending = false;

  // Split on first '|' — password may contain '|'. A missing separator means
  // bare SSID with no password (open network), e.g. "MyNet" == "MyNet|".
  char* sep = strchr(pendingWifiStr, '|');
  const char* ssid = pendingWifiStr;
  const char* pass = "";
  if (sep) {
    *sep = '\0';
    pass = sep + 1;
  }

  if (strlen(ssid) == 0 || strlen(ssid) >= WIFI_SSID_MAX) {
    Serial.println("BLE wifi: invalid SSID, ignoring");
    return;
  }

  strncpy(nvsWifiSsid, ssid, WIFI_SSID_MAX - 1);
  nvsWifiSsid[WIFI_SSID_MAX - 1] = '\0';
  strncpy(nvsWifiPass, pass, WIFI_PASS_MAX - 1);
  nvsWifiPass[WIFI_PASS_MAX - 1] = '\0';
  saveWifiToNVS();

  Serial.printf("BLE wifi: reconnecting to \"%s\"\r\n", nvsWifiSsid);
  WiFi.disconnect();
  connectWifi();
  // initTime() guards on WiFi.status() — no-op if the reconnect didn't take.
  if (!timeReady) initTime();
  exitSetupIfReady();
}

// ----------------------------------------------------------------------------
// BLE: Finnhub API key
// ----------------------------------------------------------------------------

#define BLE_APIKEY_CHAR_UUID "beb5483e-36e1-4688-b7f5-ea07361b26ad"

volatile bool apiKeyUpdatePending = false;
char pendingApiKey[MAX_APIKEY_LEN];

class ApiKeyCallbacks : public GatedStashCallbacks {
 public:
  ApiKeyCallbacks()
      : GatedStashCallbacks("apikey", pendingApiKey, MAX_APIKEY_LEN,
                            apiKeyUpdatePending) {}

  void onRead(NimBLECharacteristic* pChar) override {
    pChar->setValue((uint8_t*)nvsApiKey, strlen(nvsApiKey));
  }
};

void applyPendingApiKey() {
  apiKeyUpdatePending = false;
  strncpy(nvsApiKey, pendingApiKey, MAX_APIKEY_LEN - 1);
  nvsApiKey[MAX_APIKEY_LEN - 1] = '\0';
  saveApiKeyToNVS();
  Serial.println("BLE apikey: saved, fetching stocks");
  triggerFetch(true);
  exitSetupIfReady();
}

// ----------------------------------------------------------------------------
// BLE: Tickers
// ----------------------------------------------------------------------------

#define BLE_TICKER_CHAR_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define BLE_TICKER_BUF_LEN (MAX_STOCKS * (MAX_TICKER_LEN + 1))

volatile bool tickerUpdatePending = false;
char pendingTickerStr[BLE_TICKER_BUF_LEN];

class TickerCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pChar, ble_gap_conn_desc* desc) override {
    if (!isConnAuthed(desc->conn_handle)) {
      Serial.println("BLE tickers: unauthed, ignoring");
      return;
    }
    setupLastActivityMs = millis();
    if (millis() - lastBLEFetchMs < BLE_FETCH_COOLDOWN_MS) {
      Serial.println("BLE tickers: cooldown, ignoring");
      return;
    }
    if (stashBleWrite(pChar, pendingTickerStr, BLE_TICKER_BUF_LEN,
                      tickerUpdatePending)) {
      lastBLEFetchMs = millis();
    }
  }

  void onRead(NimBLECharacteristic* pChar) override {
    char buf[BLE_TICKER_BUF_LEN];
    int len = 0;
    for (int i = 0; i < nvsTickerCount && len < (int)sizeof(buf) - 1; i++) {
      if (i > 0) buf[len++] = ',';
      int remaining = sizeof(buf) - 1 - len;
      int tlen = strnlen(nvsTickers[i], remaining);
      memcpy(buf + len, nvsTickers[i], tlen);
      len += tlen;
    }
    buf[len] = '\0';
    pChar->setValue((uint8_t*)buf, len);
  }
};

void applyPendingTickers() {
  char buf[BLE_TICKER_BUF_LEN];
  strncpy(buf, pendingTickerStr, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';
  tickerUpdatePending = false;

  char tmp[MAX_STOCKS][MAX_TICKER_LEN];
  int count = 0;

  char* token = strtok(buf, ",");
  while (token && count < MAX_STOCKS) {
    while (*token == ' ') token++;
    int len = strlen(token);
    while (len > 0 && token[len - 1] == ' ') len--;
    token[len] = '\0';

    if (len > 0 && len < MAX_TICKER_LEN) {
      strncpy(tmp[count], token, MAX_TICKER_LEN - 1);
      tmp[count][MAX_TICKER_LEN - 1] = '\0';
      for (int j = 0; tmp[count][j]; j++)
        tmp[count][j] = toupper((unsigned char)tmp[count][j]);
      count++;
    }
    token = strtok(nullptr, ",");
  }

  if (count == 0) {
    Serial.println("BLE: no valid tickers, ignoring");
    return;
  }

  for (int i = 0; i < count; i++)
    strncpy(nvsTickers[i], tmp[i], MAX_TICKER_LEN);
  nvsTickerCount = count;
  saveTickersToNVS();

  triggerFetch(true);
}

// ----------------------------------------------------------------------------
// BLE: Locations
// ----------------------------------------------------------------------------

#define BLE_LOCS_CHAR_UUID "beb5483e-36e1-4688-b7f5-ea07361b26ae"
#define BLE_LOCS_BUF_LEN (MAX_LOCATIONS * (MAX_LOCATION_LEN + 1))

volatile bool locsUpdatePending = false;
char pendingLocsStr[BLE_LOCS_BUF_LEN];

class LocsCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pChar, ble_gap_conn_desc* desc) override {
    if (!isConnAuthed(desc->conn_handle)) {
      Serial.println("BLE locations: unauthed, ignoring");
      return;
    }
    setupLastActivityMs = millis();
    if (millis() - lastBLEFetchMs < BLE_FETCH_COOLDOWN_MS) {
      Serial.println("BLE locations: cooldown, ignoring");
      return;
    }
    if (stashBleWrite(pChar, pendingLocsStr, BLE_LOCS_BUF_LEN,
                      locsUpdatePending)) {
      lastBLEFetchMs = millis();
    }
  }

  void onRead(NimBLECharacteristic* pChar) override {
    char buf[BLE_LOCS_BUF_LEN];
    int len = 0;
    for (int i = 0; i < nvsLocationCount && len < (int)sizeof(buf) - 1; i++) {
      if (i > 0 && len < (int)sizeof(buf) - 1) buf[len++] = '|';
      int remaining = sizeof(buf) - 1 - len;
      int llen = strnlen(nvsLocations[i], remaining);
      memcpy(buf + len, nvsLocations[i], llen);
      len += llen;
    }
    buf[len] = '\0';
    pChar->setValue((uint8_t*)buf, len);
  }
};

void applyPendingLocations() {
  char buf[BLE_LOCS_BUF_LEN];
  strncpy(buf, pendingLocsStr, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';
  locsUpdatePending = false;

  char tmp[MAX_LOCATIONS][MAX_LOCATION_LEN];
  int count = 0;

  char* token = strtok(buf, "|");
  while (token && count < MAX_LOCATIONS) {
    while (*token == ' ') token++;
    int len = strlen(token);
    while (len > 0 && token[len - 1] == ' ') len--;
    token[len] = '\0';

    if (len > 0 && len < MAX_LOCATION_LEN) {
      strncpy(tmp[count], token, MAX_LOCATION_LEN - 1);
      tmp[count][MAX_LOCATION_LEN - 1] = '\0';
      count++;
    }
    token = strtok(nullptr, "|");
  }

  if (count == 0) {
    Serial.println("BLE: no valid locations, ignoring");
    return;
  }

  for (int i = 0; i < count; i++)
    strncpy(nvsLocations[i], tmp[i], MAX_LOCATION_LEN);
  nvsLocationCount = count;
  reparseLocations();
  saveLocationsToNVS();

  triggerFetch(true);
}

// ----------------------------------------------------------------------------
// BLE: Mode
// ----------------------------------------------------------------------------
// 26aa was once "Messages". The characteristic is gone; the UUID is left
// here as a tombstone comment so future additions don't reuse it and
// confuse old clients that probe for it.

#define BLE_MODE_CHAR_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a9"

volatile bool modeUpdatePending = false;
// Holds the comma-joined mode payload; size = longest valid mode string,
// "stocks,weather,clock" (20 chars + NUL), plus slack.
char pendingModeStr[64];

class ModeCallbacks : public GatedStashCallbacks {
 public:
  ModeCallbacks()
      : GatedStashCallbacks("mode", pendingModeStr, sizeof(pendingModeStr),
                            modeUpdatePending) {}

  void onRead(NimBLECharacteristic* pChar) override {
    char buf[64];
    int len = formatModeName(buf, sizeof(buf));
    pChar->setValue((uint8_t*)buf, len);
  }
};

// Accepts "all", "none", or a comma-separated subset of {stocks, weather,
// clock}. "none" returns the MASK_NONE_REQUEST sentinel (so applyPendingMode
// can distinguish it from invalid input). Returns 0 on unknown token, empty
// input, or empty mask after parse.
static uint8_t parseModePayload(const char* in) {
  if (strcmp(in, "all") == 0) return MASK_ALL;
  if (strcmp(in, "none") == 0) return MASK_NONE_REQUEST;

  char buf[64];
  strncpy(buf, in, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';

  uint8_t mask = 0;
  char* tok = strtok(buf, ",");
  while (tok) {
    while (*tok == ' ' || *tok == '\t') tok++;
    char* end = tok + strlen(tok);
    while (end > tok && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' ||
                         end[-1] == '\r')) {
      *--end = '\0';
    }

    if (strcmp(tok, "stocks") == 0)
      mask |= BIT_STOCKS;
    else if (strcmp(tok, "weather") == 0)
      mask |= BIT_WEATHER;
    else if (strcmp(tok, "clock") == 0)
      mask |= BIT_CLOCK;
    else
      return 0;
    tok = strtok(nullptr, ",");
  }
  return mask;
}

void applyPendingMode() {
  modeUpdatePending = false;
  uint8_t mask = parseModePayload(pendingModeStr);
  if (mask == 0) {
    Serial.printf("BLE: unknown/empty mode \"%s\", ignoring\r\n", pendingModeStr);
    return;
  }
  if (mask == MASK_NONE_REQUEST) {
    enabledMask = 0;
    saveDisplayMaskToNVS();
    enterIdle();
  } else {
    enabledMask = mask;
    saveDisplayMaskToNVS();
    if (!maskPrereqsReady(mask))
      enterSetup(mask);
    else {
      enterContent();
      // Fetch now (not at the next periodic tick); force=true bypasses the
      // market-hours gate — last close beats "Loading stocks...".
      triggerFetch(true);
    }
  }
  char buf[64];
  formatModeName(buf, sizeof(buf));
  Serial.printf("BLE: mode -> %s\r\n", buf);
}

// ----------------------------------------------------------------------------
// BLE: Status (sign mode)
// ----------------------------------------------------------------------------
// Write "text|N"        — set status for N seconds (N=0 = indefinite)
// Write ""              — clear status
// Read returns "text|M" — M seconds remaining (0 if indefinite),
//                         or empty string if no active status

#define BLE_STATUS_CHAR_UUID "beb5483e-36e1-4688-b7f5-ea07361b26af"
// "text|<uint32 seconds>" — text up to STATUS_MAX_LEN, plus '|', up to 10
// digits, plus NUL.
#define BLE_STATUS_BUF_LEN (STATUS_MAX_LEN + 12)

volatile bool statusUpdatePending = false;
char pendingStatusStr[BLE_STATUS_BUF_LEN];

class StatusCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pChar, ble_gap_conn_desc* desc) override {
    if (!isConnAuthed(desc->conn_handle)) {
      Serial.println("BLE status: unauthed, ignoring");
      return;
    }
    setupLastActivityMs = millis();
    std::string val = pChar->getValue();
    if (val.length() >= BLE_STATUS_BUF_LEN) return;
    memcpy(pendingStatusStr, val.c_str(), val.length());
    pendingStatusStr[val.length()] = '\0';
    statusUpdatePending = true;
  }

  void onRead(NimBLECharacteristic* pChar) override {
    char buf[BLE_STATUS_BUF_LEN];
    if (activeStatusText[0] == '\0') {
      pChar->setValue((uint8_t*)buf, 0);
      return;
    }
    uint32_t remaining = 0;  // 0 means "indefinite" on the read side
    if (statusExpiresAt != 0 && statusExpiresAt != UINT32_MAX) {
      int32_t deltaMs = (int32_t)(statusExpiresAt - millis());
      remaining = (deltaMs > 0) ? (uint32_t)(deltaMs / 1000) : 1;
    }
    int len = snprintf(buf, sizeof(buf), "%s|%u", activeStatusText, remaining);
    if (len < 0) len = 0;
    if (len >= (int)sizeof(buf)) len = sizeof(buf) - 1;
    pChar->setValue((uint8_t*)buf, len);
  }
};

void applyPendingStatus() {
  char buf[BLE_STATUS_BUF_LEN];
  strncpy(buf, pendingStatusStr, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';
  statusUpdatePending = false;

  if (buf[0] == '\0') {
    if (activeStatusText[0]) Serial.println("BLE status: cleared");
    clearActiveStatusAndResume();
    return;
  }

  // strrchr (last '|') rather than strchr (first) because status text may
  // contain pipes in theory; the seconds tail never does.
  char* sep = strrchr(buf, '|');
  if (!sep) {
    Serial.println("BLE status: missing '|' separator, ignoring");
    return;
  }

  *sep = '\0';
  char* text = buf;
  const char* tail = sep + 1;

  while (*text == ' ') text++;
  int textLen = strlen(text);
  while (textLen > 0 && text[textLen - 1] == ' ') text[--textLen] = '\0';
  if (textLen == 0) {
    if (activeStatusText[0]) Serial.println("BLE status: empty text, clearing");
    clearActiveStatusAndResume();
    return;
  }

  char* tailEnd = nullptr;
  unsigned long secs = strtoul(tail, &tailEnd, 10);
  if (tailEnd == tail) {
    Serial.println("BLE status: bad seconds value, ignoring");
    return;
  }

  strncpy(activeStatusText, text, STATUS_MAX_LEN - 1);
  activeStatusText[STATUS_MAX_LEN - 1] = '\0';

  if (secs == 0) {
    statusExpiresAt = UINT32_MAX;
    Serial.printf("BLE status: \"%s\" indefinite\r\n", activeStatusText);
  } else {
    // millis()-based: relative timing works without WiFi/NTP. Avoid the
    // 0 and UINT32_MAX sentinels in the unlikely target collision.
    uint32_t target = millis() + secs * 1000UL;
    if (target == 0 || target == UINT32_MAX) target = 1;
    statusExpiresAt = target;
    Serial.printf("BLE status: \"%s\" for %lus\r\n", activeStatusText, secs);
  }
  // A new text sign takes over the override slot from any running timer.
  if (timerPhase != TIMER_OFF) {
    timerPhase = TIMER_OFF;
    Serial.println("BLE status: new sign cancels active timer");
  }
  invalidateStatusRender();
  display.displayClear();

  // Sign-only inference: a sign on a no-WiFi device → persist mode=none so
  // the next boot lands in idle, not the setup scroll. Adding WiFi later
  // doesn't auto-restore categories — the user writes a real mode.
  if (!wifiConfigured() && enabledMask != 0) {
    Serial.println("BLE status: no-WiFi + first sign — persisting mode=none");
    enabledMask = 0;
    saveDisplayMaskToNVS();
  }
}

// ----------------------------------------------------------------------------
// BLE: Power (display on/off toggle)
// ----------------------------------------------------------------------------
// RAM-only visual-inert toggle, orthogonal to Mode and Status. The
// `displayOff` flag lives up by updateStatusLed(), which reads it.

#define BLE_POWER_CHAR_UUID "beb5483e-36e1-4688-b7f5-ea07361b26b1"

NimBLECharacteristic* pPowerChar = nullptr;

volatile bool powerUpdatePending = false;
char pendingPowerStr[8];  // big enough for "on" / "off" with NUL

// No callbacks class — no onRead (setPower() maintains the value), so
// initBLE() uses GatedStashCallbacks directly.

void setPower(bool off) {
  if (off == displayOff) return;  // idempotent

  displayOff = off;

  if (off) {
    display.displayClear();
    // Eager NeoPixel kill — updateStatusLed() only repaints on fetch-flag
    // transitions, so a mid-flight fetch would leave a stale blue dot lit.
    neopixelWrite(RGB_LED_PIN, 0, 0, 0);
    ledState = false;
    Serial.println("Power: display OFF");
  } else {
    // The sign's render cache is stale; content deserves fresh data.
    invalidateStatusRender();
    triggerFetch();
    Serial.println("Power: display ON");
  }

  // Explicit (byte-ptr, len) form: with a `const char*`, NimBLE's template
  // setValue<T> stores the *pointer*, not the string — reads return 4
  // bytes of garbage.
  if (off)
    pPowerChar->setValue((uint8_t*)"off", 3);
  else
    pPowerChar->setValue((uint8_t*)"on", 2);
}

void applyPendingPower() {
  char buf[sizeof(pendingPowerStr)];
  strncpy(buf, pendingPowerStr, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';
  powerUpdatePending = false;

  // Tolerant of " ON\n", "Off", etc. — iOS and led.py both send unadorned
  // tokens but a manual BLE GUI client may add trailing whitespace.
  char* p = buf;
  while (*p == ' ' || *p == '\t') p++;
  char tok[8];
  size_t i = 0;
  while (p[i] && p[i] != ' ' && p[i] != '\t' && p[i] != '\n' && p[i] != '\r' &&
         i < sizeof(tok) - 1) {
    tok[i] = (p[i] >= 'A' && p[i] <= 'Z') ? (p[i] + 32) : p[i];
    i++;
  }
  tok[i] = '\0';

  if (strcmp(tok, "off") == 0) {
    setPower(true);
  } else if (strcmp(tok, "on") == 0) {
    setPower(false);
  } else {
    Serial.printf("BLE power: unknown value \"%s\", ignoring\r\n",
                  pendingPowerStr);
  }
}

// ----------------------------------------------------------------------------
// BLE: Display settings (brightness + scroll speed)
// ----------------------------------------------------------------------------
// Payload "brightness|scroll_ms", e.g. "4|70". Out-of-range clamps; reads
// return current values in the same format.

#define BLE_DISPLAY_CHAR_UUID "beb5483e-36e1-4688-b7f5-ea07361b26b3"
#define BRIGHTNESS_MAX 15
#define SCROLL_SPEED_MIN_MS 20
#define SCROLL_SPEED_MAX_MS 500

volatile bool displayCfgUpdatePending = false;
char pendingDisplayCfgStr[16];  // "15|500" worst case fits comfortably

class DisplayCfgCallbacks : public GatedStashCallbacks {
 public:
  DisplayCfgCallbacks()
      : GatedStashCallbacks("display", pendingDisplayCfgStr,
                            sizeof(pendingDisplayCfgStr),
                            displayCfgUpdatePending) {}

  void onRead(NimBLECharacteristic* pChar) override {
    char buf[16];
    int n = snprintf(buf, sizeof(buf), "%u|%u", displayBrightness,
                     (unsigned)scrollSpeedMs);
    pChar->setValue((uint8_t*)buf, n);
  }
};

void applyPendingDisplayCfg() {
  char buf[sizeof(pendingDisplayCfgStr)];
  strncpy(buf, pendingDisplayCfgStr, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';
  displayCfgUpdatePending = false;

  char* sep = strchr(buf, '|');
  if (!sep) {
    Serial.printf("BLE display: malformed \"%s\", ignoring\r\n", buf);
    return;
  }
  *sep = '\0';
  char* endB;
  char* endS;
  long bright = strtol(buf, &endB, 10);
  long scroll = strtol(sep + 1, &endS, 10);
  if (endB == buf || endS == sep + 1) {
    Serial.printf("BLE display: non-numeric \"%s|%s\", ignoring\r\n", buf,
                  sep + 1);
    return;
  }

  if (bright < 0) bright = 0;
  if (bright > BRIGHTNESS_MAX) bright = BRIGHTNESS_MAX;
  if (scroll < SCROLL_SPEED_MIN_MS) scroll = SCROLL_SPEED_MIN_MS;
  if (scroll > SCROLL_SPEED_MAX_MS) scroll = SCROLL_SPEED_MAX_MS;

  displayBrightness = (uint8_t)bright;
  scrollSpeedMs = (uint16_t)scroll;
  saveDisplaySettingsToNVS();

  // setSpeed() retunes an in-flight scroll, but must not override setup
  // mode's fixed SETUP_SCROLL_SPEED.
  display.setIntensity(displayBrightness);
  if (currentMode != MODE_SETUP) display.setSpeed(scrollSpeedMs);
  Serial.printf("BLE display: brightness=%u scroll=%ums\r\n", displayBrightness,
                (unsigned)scrollSpeedMs);
}

// ----------------------------------------------------------------------------
// BLE: Timezone
// ----------------------------------------------------------------------------
// POSIX TZ string, e.g. "PST8PDT,M3.2.0,M11.1.0". Applied to the clock
// immediately; reads return the current string.

#define BLE_TZ_CHAR_UUID "beb5483e-36e1-4688-b7f5-ea07361b26b4"

volatile bool tzUpdatePending = false;
char pendingTzStr[MAX_TZ_LEN];

class TimezoneCallbacks : public GatedStashCallbacks {
 public:
  TimezoneCallbacks()
      : GatedStashCallbacks("timezone", pendingTzStr, MAX_TZ_LEN,
                            tzUpdatePending) {}

  void onRead(NimBLECharacteristic* pChar) override {
    pChar->setValue((uint8_t*)nvsTimezone, strlen(nvsTimezone));
  }
};

void applyPendingTimezone() {
  tzUpdatePending = false;

  // Light sanity check only — a full POSIX TZ parse isn't practical. A
  // malformed string falls back to UTC rendering, which is visible and
  // recoverable over BLE.
  if (!isalpha((unsigned char)pendingTzStr[0])) {
    Serial.printf("BLE timezone: rejecting \"%s\"\r\n", pendingTzStr);
    return;
  }

  strncpy(nvsTimezone, pendingTzStr, MAX_TZ_LEN - 1);
  nvsTimezone[MAX_TZ_LEN - 1] = '\0';
  saveTimezoneToNVS();

  // setenv+tzset (not configTzTime) so SNTP isn't restarted — the epoch is
  // UTC; TZ only affects rendering.
  setenv("TZ", nvsTimezone, 1);
  tzset();
  staticClockLastMin = -1;  // repaint the steady clock now, not next minute
  Serial.printf("BLE timezone: %s\r\n", nvsTimezone);
}

// ----------------------------------------------------------------------------
// BLE: Auth (write-only PIN gate)
// ----------------------------------------------------------------------------
// PIN write: match → connection authed; misses count toward the lockout.
// The PIN itself is never exposed back over BLE.

#define BLE_AUTH_CHAR_UUID "beb5483e-36e1-4688-b7f5-ea07361b26b2"

class AuthCallbacks : public NimBLECharacteristicCallbacks {
  void onRead(NimBLECharacteristic* pChar, ble_gap_conn_desc* desc) override {
    // "ok" iff this connection may write — lets the CLI verify its PIN
    // landed before firing a write that would be silently dropped.
    const char* v = isConnAuthed(desc->conn_handle) ? "ok" : "";
    pChar->setValue((uint8_t*)v, strlen(v));
  }
  void onWrite(NimBLECharacteristic* pChar, ble_gap_conn_desc* desc) override {
    AuthSlot* s = findSlot(desc->conn_handle);
    if (!s) return;
    if (s->lockoutUntilMs && (int32_t)(millis() - s->lockoutUntilMs) < 0) {
      // In lockout — drop silently so an attacker can't probe the boundary.
      return;
    }
    std::string val = pChar->getValue();
    // Trim trailing whitespace/newlines.
    while (!val.empty() && (val.back() == ' ' || val.back() == '\n' ||
                            val.back() == '\r' || val.back() == '\t')) {
      val.pop_back();
    }
    if (val == nvsPin) {
      s->authed = true;
      s->failCount = 0;
      s->lockoutUntilMs = 0;
      Serial.printf("BLE auth: conn=%u authenticated\r\n", desc->conn_handle);
    } else {
      s->failCount++;
      Serial.printf("BLE auth: bad PIN from conn=%u (fail %u/%u)\r\n",
                    desc->conn_handle, s->failCount, AUTH_FAIL_THRESHOLD);
      if (s->failCount >= AUTH_FAIL_THRESHOLD) {
        s->lockoutUntilMs = millis() + AUTH_LOCKOUT_MS;
        if (s->lockoutUntilMs == 0) s->lockoutUntilMs = 1;  // avoid sentinel
        s->failCount = 0;
        Serial.printf("BLE auth: conn=%u locked out for %ums\r\n",
                      desc->conn_handle, AUTH_LOCKOUT_MS);
      }
    }
  }
};

// ----------------------------------------------------------------------------
// BLE: Version (read-only)
// ----------------------------------------------------------------------------

#define BLE_VERSION_CHAR_UUID "beb5483e-36e1-4688-b7f5-ea07361b26b0"

class VersionCallbacks : public NimBLECharacteristicCallbacks {
  void onRead(NimBLECharacteristic* pChar) override {
    pChar->setValue(FW_VERSION);
  }
};

// ----------------------------------------------------------------------------
// BLE: Cmd (reload / reset)
// ----------------------------------------------------------------------------

#define BLE_CMD_CHAR_UUID "beb5483e-36e1-4688-b7f5-ea07361b26ab"

volatile bool cmdPending = false;
char pendingCmd[16];

class CmdCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pChar, ble_gap_conn_desc* desc) override {
    if (!isConnAuthed(desc->conn_handle)) {
      Serial.println("BLE cmd: unauthed, ignoring");
      return;
    }
    setupLastActivityMs = millis();
    std::string val = pChar->getValue();
    if (val.length() == 0 || val.length() >= sizeof(pendingCmd)) return;

    // reload and reset trigger network activity — apply cooldown
    bool fetchCmd = (val == "reload" || val == "reset");
    if (fetchCmd && millis() - lastBLEFetchMs < BLE_FETCH_COOLDOWN_MS) {
      Serial.println("BLE cmd: cooldown, ignoring");
      return;
    }

    memcpy(pendingCmd, val.c_str(), val.length());
    pendingCmd[val.length()] = '\0';
    cmdPending = true;
    if (fetchCmd) lastBLEFetchMs = millis();
  }
};

static void clearNvsNamespace(const char* ns) {
  prefs.begin(ns, false);
  prefs.clear();
  prefs.end();
}

// Every user-touchable NVS namespace plus NimBLE's bond store. Add new
// namespaces here.
static void wipeAllNvs() {
  clearNvsNamespace("wifi");
  clearNvsNamespace("apikey");
  clearNvsNamespace("tickers");
  // "msgs" and "status" are tombstone namespaces — wipe any leftover data
  // so a downgrade-then-upgrade doesn't surface stale entries.
  clearNvsNamespace("msgs");
  clearNvsNamespace("status");
  clearNvsNamespace("locs");
  clearNvsNamespace("display");
  clearNvsNamespace("time");
  clearNvsNamespace("pin");
  NimBLEDevice::deleteAllBonds();
}

// Shared by the BOOT-button hold and Command=reset: wipe NVS + bonds and
// reboot; the fresh boot reseeds defaults, generates a new PIN, and lands
// in setup mode.
static void factoryReset(const char* source) {
  Serial.printf("%s: factory reset — wiping NVS and rebooting\r\n", source);
  wipeAllNvs();
  delay(100);  // let the Serial print drain before restart
  ESP.restart();
}

void applyPendingCmd() {
  cmdPending = false;

  if (strcmp(pendingCmd, "reload") == 0) {
    Serial.println("BLE cmd: reloading stocks");
    triggerFetch(true);
  } else if (strcmp(pendingCmd, "reset") == 0) {
    // Deferred apply already ACKed the write; the client just sees the
    // connection drop on restart.
    factoryReset("BLE cmd");
  } else if (strncmp(pendingCmd, "pin-enforce ", 12) == 0) {
    const char* mode = pendingCmd + 12;
    bool on = strcmp(mode, "on") == 0;
    bool off = strcmp(mode, "off") == 0;
    if (!on && !off) {
      Serial.printf("BLE cmd: unknown pin-enforce mode \"%s\"\r\n", mode);
      return;
    }
    nvsPinEnforce = on;
    savePinEnforceToNVS();
    Serial.printf("BLE cmd: pin enforce %s\r\n", mode);
    // Connected slots keep their authed state — no surprise-kick when
    // enforcement flips on.
  } else if (strncmp(pendingCmd, "timer ", 6) == 0) {
    const char* arg = pendingCmd + 6;
    if (strcmp(arg, "cancel") == 0) {
      if (timerPhase != TIMER_OFF) {
        Serial.println("BLE cmd: timer cancel");
        cancelTimer();
      }
    } else {
      char* end = nullptr;
      long mins = strtol(arg, &end, 10);
      if (end == arg || mins < 1 || mins > TIMER_MAX_MINUTES) {
        Serial.printf("BLE cmd: bad timer arg \"%s\"\r\n", arg);
      } else {
        startTimer((uint32_t)mins);
      }
    }
  } else {
    Serial.printf("BLE cmd: unknown command \"%s\"\r\n", pendingCmd);
  }
}

// ----------------------------------------------------------------------------
// BLE init
// ----------------------------------------------------------------------------

void initBLE() {
  NimBLEDevice::init(bleDeviceName);
  NimBLEDevice::setMTU(512);
  // Passkey-entry bonding (bond + MITM + SC, DISPLAY_ONLY): iOS pops its
  // native PIN dialog, served by onPassKeyRequest.
  NimBLEDevice::setSecurityAuth(true, true, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);
  NimBLEServer* pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());
  NimBLEService* pService = pServer->createService(BLE_SERVICE_UUID);

  pService
      ->createCharacteristic(BLE_TICKER_CHAR_UUID,
                             NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE)
      ->setCallbacks(new TickerCallbacks());
  pService
      ->createCharacteristic(BLE_MODE_CHAR_UUID,
                             NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE)
      ->setCallbacks(new ModeCallbacks());
  pService->createCharacteristic(BLE_CMD_CHAR_UUID, NIMBLE_PROPERTY::WRITE)
      ->setCallbacks(new CmdCallbacks());
  pService
      ->createCharacteristic(BLE_WIFI_CHAR_UUID,
                             NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE)
      ->setCallbacks(new WifiCallbacks());
  pService
      ->createCharacteristic(BLE_APIKEY_CHAR_UUID,
                             NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE)
      ->setCallbacks(new ApiKeyCallbacks());
  pService
      ->createCharacteristic(BLE_LOCS_CHAR_UUID,
                             NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE)
      ->setCallbacks(new LocsCallbacks());
  pService
      ->createCharacteristic(BLE_STATUS_CHAR_UUID,
                             NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE)
      ->setCallbacks(new StatusCallbacks());
  NimBLECharacteristic* pVersionChar = pService->createCharacteristic(
      BLE_VERSION_CHAR_UUID, NIMBLE_PROPERTY::READ);
  pVersionChar->setCallbacks(new VersionCallbacks());
  // Seeded so the first read works even before onRead fires on this peer.
  pVersionChar->setValue(FW_VERSION);

  pPowerChar = pService->createCharacteristic(
      BLE_POWER_CHAR_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);
  pPowerChar->setCallbacks(new GatedStashCallbacks(
      "power", pendingPowerStr, sizeof(pendingPowerStr), powerUpdatePending));
  // See setPower() for why we use the explicit (uint8_t*, len) form.
  pPowerChar->setValue((uint8_t*)"on", 2);

  NimBLECharacteristic* pDisplayChar = pService->createCharacteristic(
      BLE_DISPLAY_CHAR_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);
  pDisplayChar->setCallbacks(new DisplayCfgCallbacks());
  // Seeded like Version; explicit (uint8_t*, len) form — see setPower().
  char displaySeed[16];
  int displaySeedLen = snprintf(displaySeed, sizeof(displaySeed), "%u|%u",
                                displayBrightness, (unsigned)scrollSpeedMs);
  pDisplayChar->setValue((uint8_t*)displaySeed, displaySeedLen);

  NimBLECharacteristic* pTzChar = pService->createCharacteristic(
      BLE_TZ_CHAR_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);
  pTzChar->setCallbacks(new TimezoneCallbacks());
  pTzChar->setValue((uint8_t*)nvsTimezone, strlen(nvsTimezone));

  pService
      ->createCharacteristic(BLE_AUTH_CHAR_UUID,
                             NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE)
      ->setCallbacks(new AuthCallbacks());

  pService->start();
  NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
  pAdv->addServiceUUID(BLE_SERVICE_UUID);
  pAdv->start();
  Serial.printf("BLE advertising as %s\r\n", bleDeviceName);
}

// ============================================================================
// Main
// ============================================================================

unsigned long lastFetch = 0;

// BOOT held RESET_HOLD_MS → factoryReset(). From RESET_HINT_AT_MS a
// countdown digit shows; releasing before commit aborts. Returns true
// while the countdown is on-screen so loop() skips its own render path
// (which would overwrite the digit next iteration).
static bool pollResetButton() {
  static unsigned long pressStartMs = 0;
  static int lastShownSeconds = -1;

  bool pressed = (digitalRead(BUTTON_PIN) == LOW);

  if (pressed) {
    if (pressStartMs == 0) pressStartMs = millis();
    unsigned long held = millis() - pressStartMs;

    if (held >= RESET_HOLD_MS) {
      factoryReset("button hold");
    }

    if (held >= RESET_HINT_AT_MS) {
      int secondsLeft = (RESET_HOLD_MS - held) / 1000 + 1;
      if (secondsLeft != lastShownSeconds) {
        // Just the digit — "RESET 8" doesn't fit 32 px. static: MD_Parola
        // keeps the pointer; a stack-local would dangle.
        static char buf[4];
        snprintf(buf, sizeof(buf), "%d", secondsLeft);
        display.displayClear();
        display.displayText(buf, PA_CENTER, 0, 0, PA_PRINT, PA_NO_EFFECT);
        display.displayAnimate();
        lastShownSeconds = secondsLeft;
      }
      return true;
    }
    return false;
  }

  if (lastShownSeconds >= 0) {
    // Released mid-hold — restore the normal display. IDLE needs an
    // explicit first-paint flip; other modes repaint via showNext().
    display.displayClear();
    invalidateStatusRender();
    if (currentMode == MODE_IDLE) {
      idleNeedsFirstPaint = true;
    } else {
      showNext();
    }
  }
  pressStartMs = 0;
  lastShownSeconds = -1;
  return false;
}

// ESP-IDF/ARDUHAL logs (the "[E][...]" lines) emit a bare '\n'; translate to
// '\r\n' so raw terminals (Wokwi) don't stair-step. Our own Serial.printf
// strings already carry '\r\n'.
static int crlfLogVprintf(const char* fmt, va_list args) {
  char buf[256];
  int n = vsnprintf(buf, sizeof(buf), fmt, args);
  int lim = (n < (int)sizeof(buf)) ? n : (int)sizeof(buf) - 1;
  for (int i = 0; i < lim; i++) {
    if (buf[i] == '\n') Serial.write('\r');
    Serial.write((uint8_t)buf[i]);
  }
  return n;
}

void setup() {
  Serial.begin(115200);
  // USB-CDC default is a 250 ms blocking write timeout — headless, every
  // print would stall the loop (visible matrix stutter). 0 = drop bytes.
  Serial.setTxTimeoutMs(0);
  // Wait up to 2 s for USB enumeration so the boot banner lands in the
  // monitor; falls through so a headless boot isn't wedged.
  unsigned long serialWaitStart = millis();
  while (!Serial && millis() - serialWaitStart < 2000) delay(10);
  esp_log_set_vprintf(crlfLogVprintf);  // CRLF for library [E]/[W]/[I] logs
  Serial.printf("LED-Ticker firmware v%s\r\n", FW_VERSION);
  // Boot diagnostic: categorize the last reset (POWERON/EXT=external,
  // SW=ESP.restart, PANIC=crash, INT_WDT/TASK_WDT=watchdog, BROWNOUT).
  {
    static const char* kRst[] = {"UNKNOWN", "POWERON",  "EXT",      "SW",
                                 "PANIC",   "INT_WDT",  "TASK_WDT", "WDT",
                                 "DEEPSLEEP", "BROWNOUT", "SDIO"};
    esp_reset_reason_t rr = esp_reset_reason();
    Serial.printf("[boot] reset reason: %s (%d)\r\n",
                  (rr < (int)(sizeof(kRst) / sizeof(kRst[0]))) ? kRst[rr]
                                                               : "?",
                  (int)rr);
  }

  dataMutex = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(fetchTask, "fetchStocks", FETCH_TASK_STACK, nullptr,
                          1, &fetchTaskHandle, 0);

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // Before initDisplay() so the first frame uses the persisted brightness.
  loadDisplaySettingsFromNVS();
  initDisplay();
  loadWifiFromNVS();
  loadApiKeyFromNVS();
  loadTickersFromNVS();
  loadLocationsFromNVS();
  loadDisplayMaskFromNVS();
  loadTimezoneFromNVS();
  loadPinFromNVS();
  buildDeviceName();
  if (enabledMask == 0)
    enterIdle();
  else if (!maskPrereqsReady(enabledMask))
    enterSetup(enabledMask);
  else
    enterContent();
  showNext();

  connectWifi();
  initTime();
  initBLE();
  // After initBLE() so esp_random() has RF-seeded entropy; no-op if a PIN
  // already exists.
  ensurePinExists();
  triggerFetch();
  lastFetch = millis();
}

// ----------------------------------------------------------------------------
// Serial console — dev/test input path mirroring the BLE control plane.
// Each verb writes the same pending* buffer + *UpdatePending flag the BLE
// callbacks use, so loop()'s applyPending*() does the work. Bypasses the PIN
// gate and the command cooldown: physical USB access already allows reflashing
// the chip, so the console grants no privilege an attacker wouldn't have.
// Runs in loop() on Core 1 — safe to set flags and read display globals; it
// never calls neopixelWrite().
// ----------------------------------------------------------------------------

static void consoleSetPending(char* dest, size_t destLen, const char* src,
                              volatile bool& flag) {
  strncpy(dest, src, destLen - 1);
  dest[destLen - 1] = '\0';
  flag = true;
}

static void consolePrintInfo() {
  Serial.printf("fw=v%s mode=%d mask=0x%02X\r\n", FW_VERSION, currentMode,
                enabledMask);
  bool up = WiFi.isConnected();
  Serial.printf("wifi=%s ip=%s\r\n", up ? "connected" : "disconnected",
                up ? WiFi.localIP().toString().c_str() : "-");
  Serial.printf("pin=%s enforce=%s\r\n", nvsPin, nvsPinEnforce ? "on" : "off");
  Serial.printf("bright=%u scroll=%ums timer=%s\r\n", displayBrightness,
                (unsigned)scrollSpeedMs,
                timerPhase != TIMER_OFF ? "running" : "off");
}

static void consolePrintHelp() {
  Serial.println(
      "cmds: wifi <ssid> [pass] | apikey <key> | tickers <csv> | "
      "locations <lat,lon,label;..> | mode <all|none|csv> | sign <text> | "
      "power <on|off> | bright <0-15> | scroll <ms> | tz <posix> | "
      "timer <min|cancel> | pin-enforce <on|off> | reload | reset | info | help");
}

static void dispatchConsoleCmd(const ConsoleCmd& cmd) {
  switch (cmd.verb) {
    case CONSOLE_NONE:
      return;
    case CONSOLE_UNKNOWN:
      Serial.println("error: unknown command (try 'help')");
      return;
    case CONSOLE_HELP:
      consolePrintHelp();
      return;
    case CONSOLE_INFO:
      consolePrintInfo();
      return;

    case CONSOLE_WIFI: {
      // applyPendingWifi() wants "ssid|pass". "wifi <ssid>" = open network
      // (empty password, e.g. Wokwi-GUEST); "wifi <ssid> <pass>" splits on the
      // first space, so the SSID can't contain a space but the password can.
      if (cmd.arg[0] == '\0') {
        Serial.println("usage: wifi <ssid> [pass]");
        return;
      }
      const char* sp = strchr(cmd.arg, ' ');
      size_t ssidLen = sp ? (size_t)(sp - cmd.arg) : strlen(cmd.arg);
      const char* pass = sp ? sp + 1 : "";
      char joined[BLE_WIFI_BUF_LEN];
      if (ssidLen >= sizeof(joined) - 2) {
        Serial.println("error: ssid too long");
        return;
      }
      memcpy(joined, cmd.arg, ssidLen);
      joined[ssidLen] = '|';
      strncpy(joined + ssidLen + 1, pass, sizeof(joined) - ssidLen - 2);
      joined[sizeof(joined) - 1] = '\0';
      consoleSetPending(pendingWifiStr, sizeof(pendingWifiStr), joined,
                        wifiUpdatePending);
      Serial.println("ok: wifi");
      return;
    }
    case CONSOLE_APIKEY:
      consoleSetPending(pendingApiKey, sizeof(pendingApiKey), cmd.arg,
                        apiKeyUpdatePending);
      Serial.println("ok: apikey");
      return;
    case CONSOLE_TICKERS:
      consoleSetPending(pendingTickerStr, sizeof(pendingTickerStr), cmd.arg,
                        tickerUpdatePending);
      Serial.println("ok: tickers");
      return;
    case CONSOLE_LOCATIONS:
      consoleSetPending(pendingLocsStr, sizeof(pendingLocsStr), cmd.arg,
                        locsUpdatePending);
      Serial.println("ok: locations");
      return;
    case CONSOLE_MODE:
      consoleSetPending(pendingModeStr, sizeof(pendingModeStr), cmd.arg,
                        modeUpdatePending);
      Serial.println("ok: mode");
      return;
    case CONSOLE_SIGN: {
      // applyPendingStatus() expects "text|seconds" (0 = indefinite); console
      // signs are indefinite. Empty text ("sign" with no arg) clears the sign.
      char buf[BLE_STATUS_BUF_LEN];
      int n = snprintf(buf, sizeof(buf), "%s|0", cmd.arg);
      if (n < 0 || n >= (int)sizeof(buf)) {
        Serial.println("error: sign text too long");
        return;
      }
      consoleSetPending(pendingStatusStr, sizeof(pendingStatusStr), buf,
                        statusUpdatePending);
      Serial.println("ok: sign");
      return;
    }
    case CONSOLE_POWER:
      consoleSetPending(pendingPowerStr, sizeof(pendingPowerStr), cmd.arg,
                        powerUpdatePending);
      Serial.println("ok: power");
      return;
    case CONSOLE_TZ:
      consoleSetPending(pendingTzStr, sizeof(pendingTzStr), cmd.arg,
                        tzUpdatePending);
      Serial.println("ok: tz");
      return;

    case CONSOLE_BRIGHT: {
      // applyPendingDisplayCfg() expects "bright|scroll"; keep current scroll.
      char buf[sizeof(pendingDisplayCfgStr)];
      int n = snprintf(buf, sizeof(buf), "%s|%u", cmd.arg, (unsigned)scrollSpeedMs);
      if (n < 0 || n >= (int)sizeof(buf)) {
        Serial.println("error: bright arg too long");
        return;
      }
      consoleSetPending(pendingDisplayCfgStr, sizeof(pendingDisplayCfgStr), buf,
                        displayCfgUpdatePending);
      Serial.println("ok: bright");
      return;
    }
    case CONSOLE_SCROLL: {
      char buf[sizeof(pendingDisplayCfgStr)];
      int n = snprintf(buf, sizeof(buf), "%u|%s", displayBrightness, cmd.arg);
      if (n < 0 || n >= (int)sizeof(buf)) {
        Serial.println("error: scroll arg too long");
        return;
      }
      consoleSetPending(pendingDisplayCfgStr, sizeof(pendingDisplayCfgStr), buf,
                        displayCfgUpdatePending);
      Serial.println("ok: scroll");
      return;
    }

    // Command-style verbs route through pendingCmd (16-byte buffer).
    case CONSOLE_TIMER: {
      char buf[sizeof(pendingCmd)];
      int n = snprintf(buf, sizeof(buf), "timer %s", cmd.arg);
      if (n < 0 || n >= (int)sizeof(buf)) {
        Serial.println("error: timer arg too long");
        return;
      }
      consoleSetPending(pendingCmd, sizeof(pendingCmd), buf, cmdPending);
      Serial.println("ok: timer");
      return;
    }
    case CONSOLE_PINENFORCE: {
      char buf[sizeof(pendingCmd)];
      int n = snprintf(buf, sizeof(buf), "pin-enforce %s", cmd.arg);
      if (n < 0 || n >= (int)sizeof(buf)) {
        Serial.println("error: pin-enforce arg too long");
        return;
      }
      consoleSetPending(pendingCmd, sizeof(pendingCmd), buf, cmdPending);
      Serial.println("ok: pin-enforce");
      return;
    }
    case CONSOLE_RELOAD:
      consoleSetPending(pendingCmd, sizeof(pendingCmd), "reload", cmdPending);
      Serial.println("ok: reload");
      return;
    case CONSOLE_RESET:
      consoleSetPending(pendingCmd, sizeof(pendingCmd), "reset", cmdPending);
      Serial.println("ok: reset");
      return;
  }
}

// Non-blocking: accumulate one line, then parse + dispatch. Buffer sized to the
// largest payload (locations CSV) plus the verb word and separators.
void pollSerialConsole() {
  static char line[BLE_LOCS_BUF_LEN + 32];
  static size_t len = 0;
  static bool overflow = false;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      // len == 0 here is a blank line or the paired \n of a \r\n — swallow it
      // (no echo) so a \r\n terminator doesn't print two newlines.
      if (overflow) {
        Serial.println();
        Serial.println("error: line too long");
        overflow = false;
        len = 0;
      } else if (len > 0) {
        Serial.println();  // echo the newline before printing the response
        line[len] = '\0';
        dispatchConsoleCmd(parseConsoleLine(line));
        len = 0;
      }
    } else if (c == '\b' || c == 0x7f) {  // backspace / DEL: erase last char
      if (!overflow && len > 0) {
        len--;
        Serial.print("\b \b");
      }
    } else if (overflow) {
      continue;  // swallow the rest of an over-long line
    } else if (len < sizeof(line) - 1) {
      line[len++] = c;
      Serial.write(c);  // local echo so the user sees what they type
    } else {
      overflow = true;
    }
  }
}

void loop() {
  // Heartbeat: matrix frozen but heartbeats coming → SPI/Parola stuck;
  // heartbeats stopped → whole loop hung.
  static unsigned long lastHeartbeatMs = 0;
  unsigned long nowMs = millis();
  if (nowMs - lastHeartbeatMs > 30000) {
    lastHeartbeatMs = nowMs;
    Serial.printf(
        "[hb] v%s mode=%d mask=0x%02X fetching=%d heap=%u min=%u millis=%lu\r\n",
        FW_VERSION, currentMode, enabledMask, fetching,
        (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(), nowMs);
  }

  pollSerialConsole();  // serial console feeds the same pending* flags below
  if (wifiUpdatePending) applyPendingWifi();
  if (apiKeyUpdatePending) applyPendingApiKey();
  if (cmdPending) applyPendingCmd();
  if (modeUpdatePending) applyPendingMode();
  if (tickerUpdatePending) applyPendingTickers();
  if (locsUpdatePending) applyPendingLocations();
  if (statusUpdatePending) applyPendingStatus();
  if (powerUpdatePending) applyPendingPower();
  if (displayCfgUpdatePending) applyPendingDisplayCfg();
  if (tzUpdatePending) applyPendingTimezone();

  updateStatusLed();
  if (pollResetButton()) {
    // Reset countdown owns the display this tick — skip the renderers.
    delay(1);
    return;
  }

  if (currentMode == MODE_SETUP &&
      millis() - setupLastActivityMs > SETUP_TIMEOUT_MS) {
    if (wifiConfigured()) {
      Serial.println(
          "Setup: 60s no activity, falling to content (mask unchanged)");
      enterContent();
    } else {
      // No WiFi → every category is dead; staying on the name scroll beats
      // a "Loading X..." loop. Reschedule the timeout check.
      setupLastActivityMs = millis();
    }
  }

  if (displayOff) {
    // Display is off: skip all render AND fetch work.
    delay(100);
    return;
  }

  if (timerPhase != TIMER_OFF) {
    tickTimer();
  } else if (checkStatusForRender()) {
    tickActiveStatus();
  } else if (currentMode == MODE_IDLE) {
    tickIdle();
  } else if (currentMode == MODE_CONTENT && enabledMask == BIT_CLOCK &&
             timeReady) {
    tickStaticClock();
  } else if (display.displayAnimate()) {
    display.displayReset();
    showNext();
  }

  if (millis() - lastFetch > FETCH_INTERVAL_MS) {
    lastFetch = millis();
    triggerFetch();
  }
}