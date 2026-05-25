#include <Arduino.h>
#include <MD_Parola.h>
#include <MD_MAX72xx.h>
#include <SPI.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <time.h>
#include <NimBLEDevice.h>
#include "config.h"
#include "version.h"

// ============================================================================
// Mode bits
// ============================================================================
// An active status sign overrides both modes until expiry/clear.
// 0x02 was once BIT_MESSAGES — tombstoned; legacy NVS masks are stripped
// via `& MASK_ALL` on load. When BIT_CLOCK is the only enabled bit, the
// display switches to a steady non-scrolling clock — see tickStaticClock().

enum
{
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

#define HARDWARE_TYPE MD_MAX72XX::FC16_HW
#define MAX_DEVICES 4
#define DIN_PIN 6
#define CLK_PIN 4
#define CS_PIN 5
#define RGB_LED_PIN 48

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

// ============================================================================
// NVS: WiFi credentials
// ============================================================================

#define WIFI_SSID_MAX 64
#define WIFI_PASS_MAX 64

char nvsWifiSsid[WIFI_SSID_MAX];
char nvsWifiPass[WIFI_PASS_MAX];

void saveWifiToNVS()
{
  prefs.begin("wifi", false);
  prefs.putString("ssid", nvsWifiSsid);
  prefs.putString("pass", nvsWifiPass);
  prefs.end();
}

void loadWifiFromNVS()
{
  prefs.begin("wifi", true);
  bool hasSsid = prefs.isKey("ssid");
  if (hasSsid)
  {
    prefs.getString("ssid", nvsWifiSsid, WIFI_SSID_MAX);
    prefs.getString("pass", nvsWifiPass, WIFI_PASS_MAX);
    Serial.printf("Loaded WiFi credentials from NVS (SSID: %s)\n", nvsWifiSsid);
  }
  else
  {
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

void saveApiKeyToNVS()
{
  prefs.begin("apikey", false);
  prefs.putString("key", nvsApiKey);
  prefs.end();
}

void loadApiKeyFromNVS()
{
  prefs.begin("apikey", true);
  bool hasKey = prefs.isKey("key");
  if (hasKey)
  {
    prefs.getString("key", nvsApiKey, MAX_APIKEY_LEN);
    Serial.println("Loaded API key from NVS");
  }
  else
  {
    nvsApiKey[0] = '\0';
    Serial.println("Finnhub API key not configured — use BLE to set it");
  }
  prefs.end();
}

bool apiKeyConfigured() { return nvsApiKey[0] != '\0'; }

// ============================================================================
// NVS: Stock tickers + in-RAM quote state
// ============================================================================

#define MAX_STOCKS 10
#define MAX_TICKER_LEN 16

char nvsTickers[MAX_STOCKS][MAX_TICKER_LEN];
int nvsTickerCount = 0;

struct StockQuote
{
  char symbol[MAX_TICKER_LEN];
  float price;
  float changePct;
};

StockQuote stockQuotes[MAX_STOCKS];
int stockCount = 0;
int currentStock = 0;

void saveTickersToNVS()
{
  prefs.begin("tickers", false);
  prefs.putInt("count", nvsTickerCount);
  for (int i = 0; i < nvsTickerCount; i++)
  {
    char key[8];
    snprintf(key, sizeof(key), "t%d", i);
    prefs.putString(key, nvsTickers[i]);
  }
  prefs.end();
}

void loadTickersFromNVS()
{
  prefs.begin("tickers", true);
  int count = prefs.getInt("count", 0);
  if (count > 0 && count <= MAX_STOCKS)
  {
    for (int i = 0; i < count; i++)
    {
      char key[8];
      snprintf(key, sizeof(key), "t%d", i);
      prefs.getString(key, nvsTickers[i], MAX_TICKER_LEN);
    }
    prefs.end();
    nvsTickerCount = count;
    Serial.printf("Loaded %d tickers from NVS\n", count);
  }
  else
  {
    prefs.end();
    // First boot: seed from config.h defaults
    for (int i = 0; i < stockTickerCount && i < MAX_STOCKS; i++)
    {
      strncpy(nvsTickers[i], stockTickers[i], MAX_TICKER_LEN - 1);
      nvsTickers[i][MAX_TICKER_LEN - 1] = '\0';
    }
    nvsTickerCount = stockTickerCount;
    saveTickersToNVS();
    Serial.printf("Seeded %d tickers from defaults\n", nvsTickerCount);
  }
}

// ============================================================================
// NVS: Weather locations + in-RAM resolution & readings
// ============================================================================

#define MAX_LOCATIONS 5
#define MAX_LOCATION_LEN 40 // user-entered "City, State" or zip
#define MAX_LOC_NAME_LEN 24 // canonical name from geocoder

char nvsLocations[MAX_LOCATIONS][MAX_LOCATION_LEN];
int nvsLocationCount = 0;

struct ResolvedLocation
{
  bool ok;
  float lat;
  float lon;
  char name[MAX_LOC_NAME_LEN];
};
ResolvedLocation resolved[MAX_LOCATIONS];

struct WeatherReading
{
  char name[MAX_LOC_NAME_LEN];
  float tempF;
};

WeatherReading weatherReadings[MAX_LOCATIONS];
int weatherCount = 0;
int currentWeather = 0;

void saveLocationsToNVS()
{
  prefs.begin("locs", false);
  prefs.putInt("count", nvsLocationCount);
  for (int i = 0; i < nvsLocationCount; i++)
  {
    char key[8];
    snprintf(key, sizeof(key), "l%d", i);
    prefs.putString(key, nvsLocations[i]);
  }
  prefs.end();
}

void loadLocationsFromNVS()
{
  prefs.begin("locs", true);
  int count = prefs.getInt("count", 0);
  if (count > 0 && count <= MAX_LOCATIONS)
  {
    for (int i = 0; i < count; i++)
    {
      char key[8];
      snprintf(key, sizeof(key), "l%d", i);
      prefs.getString(key, nvsLocations[i], MAX_LOCATION_LEN);
    }
    prefs.end();
    nvsLocationCount = count;
    Serial.printf("Loaded %d locations from NVS\n", count);
  }
  else
  {
    prefs.end();
    // First boot: seed from config.h defaults
    for (int i = 0; i < defaultLocationCount && i < MAX_LOCATIONS; i++)
    {
      strncpy(nvsLocations[i], defaultLocations[i], MAX_LOCATION_LEN - 1);
      nvsLocations[i][MAX_LOCATION_LEN - 1] = '\0';
    }
    nvsLocationCount = defaultLocationCount;
    saveLocationsToNVS();
    Serial.printf("Seeded %d locations from defaults\n", nvsLocationCount);
  }
  for (int i = 0; i < MAX_LOCATIONS; i++)
    resolved[i].ok = false;
}

// ============================================================================
// NVS: Enabled-category mask
// ============================================================================

uint8_t enabledMask = MASK_ALL;

void saveDisplayMaskToNVS()
{
  prefs.begin("display", false);
  prefs.putUChar("mask", enabledMask);
  prefs.end();
}

void loadDisplayMaskFromNVS()
{
  prefs.begin("display", true);
  if (prefs.isKey("mask"))
  {
    // mask=0 is now a valid persisted value ("none" mode — sign-only with
    // idle pixel between signs), so don't fall back to MASK_ALL here.
    enabledMask = prefs.getUChar("mask", MASK_ALL) & MASK_ALL;
  }
  prefs.end();
  Serial.printf("Display mask: 0x%02X\n", enabledMask);
}

// ============================================================================
// Setup-mode state
// ============================================================================
// When the user requests categories whose prereqs aren't satisfied, the
// display drops into MODE_SETUP and scrolls the BLE device name (e.g.
// "LED-Ticker-AB12") on a loop so the user can identify which device to
// connect to in the iOS app. `setupTargetMask` records what to resume into
// once prereqs are met (or after the inactivity timeout). No "Configure
// WiFi over BLE" hint — the audience already knows the BLE-config workflow.

#define SETUP_TIMEOUT_MS 60000

volatile unsigned long setupLastActivityMs = 0;
uint8_t setupTargetMask = MASK_ALL;

// ============================================================================
// Active-status state (sign mode)
// ============================================================================
// The "sign mode" override: when activeStatusText is non-empty and not yet
// expired, the loop renders it (steady if short, scrolling if long) in
// place of the normal ambient rotation. statusExpiresAt is a millis()
// target value (NOT a Unix epoch — relative timing works without WiFi/NTP);
// 0 means "no status active", UINT32_MAX means "indefinite (until cleared)".
// Wrap-safe via signed-delta comparison in checkStatusForRender. State is
// in-RAM only — a power cycle clears any active sign and the device
// resumes its ambient mode.

// Active-status text fits in MAX_STRING_LEN; up to 5 chars renders static,
// longer scrolls. See tickActiveStatus().
#define STATUS_MAX_LEN MAX_STRING_LEN
#define STATUS_STATIC_MAX_CHARS 5

char activeStatusText[STATUS_MAX_LEN] = {0};
uint32_t statusExpiresAt = 0;

void clearStatus()
{
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

void initDisplay()
{
  SPI.begin(CLK_PIN, -1, DIN_PIN, CS_PIN);
  display.begin();
  display.setIntensity(DISPLAY_INTENSITY);
  display.displayClear();
}

void scrollText(const char *msg)
{
  display.displayScroll(msg, PA_LEFT, PA_SCROLL_LEFT, SCROLL_SPEED);
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

void updateStatusLed()
{
  if (displayOff)
  {
    if (ledState)
    {
      neopixelWrite(RGB_LED_PIN, 0, 0, 0);
      ledState = false;
    }
    return;
  }
  if (fetching && !ledState)
  {
    neopixelWrite(RGB_LED_PIN, 0, 0, 20);
    ledState = true;
  }
  else if (!fetching && ledState)
  {
    neopixelWrite(RGB_LED_PIN, 0, 0, 0);
    ledState = false;
  }
}

// ============================================================================
// Time / Market hours
// ============================================================================

bool timeReady = false;

void initTime()
{
  // Only start SNTP once WiFi is actually up — otherwise lwIP's SNTP client
  // burns retries on DNS lookups that can't possibly succeed, and on
  // pre-IDF-5 builds (Arduino 2.0.14 here) those failures accumulate and
  // eventually wedge the device. See the "fresh-boot freeze after 10s of
  // minutes" symptom.
  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.println("Skipping NTP init — WiFi not connected");
    return;
  }

  // TIMEZONE / NTP_SERVER come from config.h. Note: isMarketOpen() assumes
  // the device clock is in PT and shifts by -3 to reach ET — if you change
  // TIMEZONE you'll need to revisit those constants too.
  configTzTime(TIMEZONE, NTP_SERVER);

  Serial.println("Syncing NTP...");
  for (int i = 0; i < 20; i++)
  {
    struct tm t;
    if (getLocalTime(&t, 100))
    {
      timeReady = true;
      Serial.printf("Time: %04d-%02d-%02d %02d:%02d ET\n",
                    t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min);
      return;
    }
    delay(500);
  }
  Serial.println("NTP sync failed, will fetch stocks anyway");
}

bool isMarketOpen()
{
  if (!timeReady)
    return true;

  struct tm t;
  if (!getLocalTime(&t, 100))
    return true;

  if (t.tm_wday == 0 || t.tm_wday == 6)
    return false;

  // NYSE 9:30–16:00 ET, expressed in Pacific (global TZ is PST8PDT).
  // DST drift between ET and PT cancels — both shift on the same Sundays.
  const int MARKET_OPEN = 6 * 60 + 30;
  const int MARKET_CLOSE = 13 * 60;
  int minutes = t.tm_hour * 60 + t.tm_min;
  return minutes >= MARKET_OPEN && minutes < MARKET_CLOSE;
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

static void commitStocks(const StockQuote *tmp, int count)
{
  xSemaphoreTake(dataMutex, portMAX_DELAY);
  for (int i = 0; i < count; i++)
    stockQuotes[i] = tmp[i];
  stockCount = count;
  currentStock = 0;
  xSemaphoreGive(dataMutex);
}

static void fetchStocksImpl(bool force)
{
  if (!apiKeyConfigured() || WiFi.status() != WL_CONNECTED)
    return;

  // Market closed: keep whatever we last fetched in RAM. Only hit the API
  // if we have no data at all (e.g. cold boot on a weekend).
  if (!force && !isMarketOpen() && stockCount > 0)
    return;

  StockQuote tmp[MAX_STOCKS];
  int count = 0;

  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(5000);
  http.setReuse(true); // keep TLS session across same-host requests

  for (int i = 0; i < nvsTickerCount && count < MAX_STOCKS; i++)
  {
    char url[256];
    snprintf(url, sizeof(url),
             "https://finnhub.io/api/v1/quote?symbol=%s&token=%s",
             nvsTickers[i], nvsApiKey);

    http.begin(url);

    int code = http.GET();
    if (code != 200)
    {
      Serial.printf("Stock HTTP error: %d for %s\n", code, nvsTickers[i]);
      http.end();
      continue;
    }

    String body = http.getString();
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, body))
      continue;

    float current = doc["c"];
    float change = doc["dp"];

    if (current == 0)
      continue;

    strncpy(tmp[count].symbol, nvsTickers[i], MAX_TICKER_LEN - 1);
    tmp[count].symbol[MAX_TICKER_LEN - 1] = '\0';
    tmp[count].price = current;
    tmp[count].changePct = change;
    count++;
  }

  if (count > 0)
  {
    commitStocks(tmp, count);
    Serial.printf("Loaded %d stock quotes\n", count);
  }
}

static void commitWeather(const WeatherReading *tmp, int count)
{
  xSemaphoreTake(dataMutex, portMAX_DELAY);
  for (int i = 0; i < count; i++)
    weatherReadings[i] = tmp[i];
  weatherCount = count;
  currentWeather = 0;
  xSemaphoreGive(dataMutex);
}

// URL-encodes spaces and commas — the only chars we expect in location queries.
static void urlEncodeLocation(const char *in, char *out, int outLen)
{
  int j = 0;
  for (int i = 0; in[i] && j < outLen - 4; i++)
  {
    unsigned char c = (unsigned char)in[i];
    if (c == ' ')
    {
      out[j++] = '%';
      out[j++] = '2';
      out[j++] = '0';
    }
    else if (c == ',')
    {
      out[j++] = '%';
      out[j++] = '2';
      out[j++] = 'C';
    }
    else
    {
      out[j++] = c;
    }
  }
  out[j] = '\0';
}

// Resolves a user-entered string ("98052" or "Redmond, WA") to lat/lon.
// If the query contains a trailing ", XX" we use it as an admin1 (state) filter.
static bool geocodeLocation(HTTPClient &http, const char *query, ResolvedLocation &out)
{
  char name[MAX_LOCATION_LEN];
  char region[MAX_LOCATION_LEN];
  strncpy(name, query, sizeof(name) - 1);
  name[sizeof(name) - 1] = '\0';
  region[0] = '\0';

  char *comma = strchr(name, ',');
  if (comma)
  {
    *comma = '\0';
    const char *r = comma + 1;
    while (*r == ' ')
      r++;
    strncpy(region, r, sizeof(region) - 1);
    region[sizeof(region) - 1] = '\0';
    int rlen = strlen(region);
    while (rlen > 0 && region[rlen - 1] == ' ')
      region[--rlen] = '\0';
  }

  char encoded[MAX_LOCATION_LEN * 3];
  urlEncodeLocation(name, encoded, sizeof(encoded));

  char url[256];
  snprintf(url, sizeof(url),
           "https://geocoding-api.open-meteo.com/v1/search?name=%s&count=5",
           encoded);

  http.begin(url);
  int code = http.GET();
  if (code != 200)
  {
    Serial.printf("Geocode HTTP error: %d for \"%s\"\n", code, query);
    http.end();
    return false;
  }

  String body = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, body))
    return false;

  JsonVariant results = doc["results"];
  if (results.isNull() || results.size() == 0)
  {
    Serial.printf("Geocode: no results for \"%s\"\n", query);
    return false;
  }

  JsonVariant pick;
  if (region[0] != '\0')
  {
    for (JsonVariant r : results.as<JsonArray>())
    {
      const char *admin1 = r["admin1"] | "";
      const char *ccode = r["country_code"] | "";
      if (strcasecmp(admin1, region) == 0 || strcasecmp(ccode, region) == 0)
      {
        pick = r;
        break;
      }
    }
  }
  if (pick.isNull())
    pick = results[0];

  out.lat = pick["latitude"];
  out.lon = pick["longitude"];
  const char *resolvedName = pick["name"] | name;
  strncpy(out.name, resolvedName, MAX_LOC_NAME_LEN - 1);
  out.name[MAX_LOC_NAME_LEN - 1] = '\0';
  out.ok = true;
  return true;
}

static void fetchWeatherImpl()
{
  if (WiFi.status() != WL_CONNECTED || nvsLocationCount == 0)
    return;

  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(5000);
  http.setReuse(true); // keep TLS session across same-host requests

  WeatherReading tmp[MAX_LOCATIONS];
  int count = 0;

  for (int i = 0; i < nvsLocationCount && count < MAX_LOCATIONS; i++)
  {
    if (!resolved[i].ok)
    {
      if (!geocodeLocation(http, nvsLocations[i], resolved[i]))
        continue;
      Serial.printf("Geocoded \"%s\" -> %s (%.4f,%.4f)\n",
                    nvsLocations[i], resolved[i].name,
                    resolved[i].lat, resolved[i].lon);
    }

    char url[256];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
             "&current=temperature_2m&temperature_unit=fahrenheit",
             resolved[i].lat, resolved[i].lon);

    http.begin(url);
    int code = http.GET();
    if (code != 200)
    {
      Serial.printf("Weather HTTP error: %d for %s\n", code, resolved[i].name);
      http.end();
      continue;
    }

    String body = http.getString();
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, body))
      continue;

    strncpy(tmp[count].name, resolved[i].name, MAX_LOC_NAME_LEN - 1);
    tmp[count].name[MAX_LOC_NAME_LEN - 1] = '\0';
    tmp[count].tempF = doc["current"]["temperature_2m"];
    count++;
  }

  if (count > 0)
  {
    commitWeather(tmp, count);
    Serial.printf("Loaded %d weather entries\n", count);
  }
}

static void fetchTask(void *)
{
  while (true)
  {
    uint32_t forceVal;
    xTaskNotifyWait(0, 0, &forceVal, portMAX_DELAY);
    // None mode (enabledMask == 0) → nothing to display, nothing to fetch.
    // Skip before flipping the blue-LED indicator. uint8_t reads are atomic
    // on ESP32; a racy read here at worst causes one wasted fetch cycle.
    if (enabledMask == 0)
      continue;
    fetching = true;
    unsigned long t0 = millis();
    Serial.printf("[fetch] start mask=0x%02X force=%lu\n",
                  enabledMask, (unsigned long)forceVal);
    fetchStocksImpl((bool)forceVal);
    fetchWeatherImpl();
    Serial.printf("[fetch] end took=%lums\n", millis() - t0);
    fetching = false;
  }
}

void triggerFetch(bool force = false)
{
  xTaskNotify(fetchTaskHandle, (uint32_t)force, eSetValueWithOverwrite);
}

// ============================================================================
// WiFi connect
// ============================================================================

void connectWifi()
{
  if (!wifiConfigured() || WiFi.status() == WL_CONNECTED)
    return;

  Serial.printf("Connecting to %s...\n", nvsWifiSsid);
  WiFi.begin(nvsWifiSsid, nvsWifiPass);

  for (int attempts = 0; WiFi.status() != WL_CONNECTED && attempts < 20; attempts++)
  {
    delay(500);
  }

  if (WiFi.status() == WL_CONNECTED)
  {
    IPAddress ip = WiFi.localIP();
    Serial.printf("Connected, IP: %u.%u.%u.%u\n", ip[0], ip[1], ip[2], ip[3]);
  }
  else
  {
    Serial.println("WiFi failed");
  }
}

// ============================================================================
// Display rotation & rendering
// ============================================================================
// MODE_CONTENT cycles through enabled categories. MODE_SETUP scrolls the BLE
// device name when prereqs are missing (pre-config affordance). MODE_IDLE is
// a quiet bouncing-pixel state entered after a sign clears on a device whose
// prereqs are still unmet — the user has already discovered the device, so
// re-scrolling the name would be noise. Active-status (sign mode) is an
// orthogonal override layered on top by checkStatusForRender()/
// tickActiveStatus().

int currentMode = MODE_CONTENT;

// Which category bit is currently scrolling within MODE_CONTENT. Always one
// of BIT_STOCKS / BIT_WEATHER / BIT_CLOCK. Advances when the active category
// wraps so each enabled category gets a full pass per cycle.
uint8_t currentBit = BIT_STOCKS;

// --- Per-category renderers ---

void showNextStock()
{
  StockQuote q;
  xSemaphoreTake(dataMutex, portMAX_DELAY);
  q = stockQuotes[currentStock];
  currentStock = (currentStock + 1) % stockCount;
  xSemaphoreGive(dataMutex);

  const char *arrow = q.changePct >= 0 ? "\x18" : "\x19";
  snprintf(scrollBuf, sizeof(scrollBuf),
           "%s $%.2f %s", q.symbol, q.price, arrow);
  scrollText(scrollBuf);
}

void showNextWeather()
{
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
void showNextClock()
{
  struct tm t;
  if (!getLocalTime(&t, 50))
  {
    scrollText("Loading time...");
    return;
  }
  int h = t.tm_hour;
  const char *ampm = (h >= 12) ? "PM" : "AM";
  int h12 = h % 12;
  if (h12 == 0)
    h12 = 12;
  snprintf(scrollBuf, sizeof(scrollBuf), "%d:%02d %s", h12, t.tm_min, ampm);
  scrollText(scrollBuf);
}

// Static "H:MM" — steady, no blink. Drives the display directly via
// displayText()/displayAnimate() instead of the scroll pump in loop().
// Only invoked when enabledMask == BIT_CLOCK alone. The minute digits
// changing once a minute are the only "still alive" signal — same
// philosophy as the Status sign: information should sit still and be
// read, not animate for attention.
void tickStaticClock()
{
  static int lastMin = -1;

  struct tm t;
  if (!getLocalTime(&t, 0))
    return;
  if (lastMin == t.tm_min)
    return;

  int h12 = t.tm_hour % 12;
  if (h12 == 0)
    h12 = 12;

  snprintf(scrollBuf, sizeof(scrollBuf), "%d:%02d", h12, t.tm_min);

  display.displayClear();
  display.displayText(scrollBuf, PA_CENTER, 0, 0, PA_PRINT, PA_NO_EFFECT);
  display.displayAnimate();

  lastMin = t.tm_min;
}

// MODE_IDLE animation: a single pixel bounces diagonally around the 8x32
// matrix at ~150 ms/step, reflecting off all four walls Pong-style. Quiet
// "alive" signal shown after a sign clears on a device whose ambient prereqs
// aren't met — the user has already discovered this device (they sent the
// sign) so we don't need to identify it the way MODE_SETUP does.
#define IDLE_STEP_MS 150
#define IDLE_COL_MAX (8 * MAX_DEVICES - 1)
#define IDLE_ROW_MAX 7

// enterIdle() is in the mode-transitions group below alongside enterContent()
// and enterSetup(); these statics live here next to tickIdle() that consumes
// them.
static int idlePixelCol = 0;
static int idlePixelRow = 0;
static int idlePixelDirX = 1;
static int idlePixelDirY = 1;
static unsigned long idleLastStepMs = 0;
static bool idleNeedsFirstPaint = false;

void tickIdle()
{
  MD_MAX72XX *mx = display.getGraphicObject();
  unsigned long now = millis();

  if (idleNeedsFirstPaint)
  {
    mx->clear();
    mx->setPoint(idlePixelRow, idlePixelCol, true);
    idleLastStepMs = now;
    idleNeedsFirstPaint = false;
    return;
  }

  if (now - idleLastStepMs < IDLE_STEP_MS)
    return;
  idleLastStepMs = now;

  mx->setPoint(idlePixelRow, idlePixelCol, false);

  idlePixelCol += idlePixelDirX;
  if (idlePixelCol >= IDLE_COL_MAX)
  {
    idlePixelCol = IDLE_COL_MAX;
    idlePixelDirX = -1;
  }
  else if (idlePixelCol <= 0)
  {
    idlePixelCol = 0;
    idlePixelDirX = 1;
  }

  idlePixelRow += idlePixelDirY;
  if (idlePixelRow >= IDLE_ROW_MAX)
  {
    idlePixelRow = IDLE_ROW_MAX;
    idlePixelDirY = -1;
  }
  else if (idlePixelRow <= 0)
  {
    idlePixelRow = 0;
    idlePixelDirY = 1;
  }

  mx->setPoint(idlePixelRow, idlePixelCol, true);
}

// --- Category rotation helpers ---

static bool stocksAvailable()
{
  return wifiConfigured() && apiKeyConfigured() && stockCount > 0;
}

static bool weatherAvailable()
{
  return wifiConfigured() && weatherCount > 0;
}

static bool bitHasData(uint8_t b)
{
  if (b == BIT_STOCKS)
    return stocksAvailable();
  if (b == BIT_WEATHER)
    return weatherAvailable();
  if (b == BIT_CLOCK)
    return timeReady; // NTP must have synced at least once
  return false;
}

static uint8_t nextBit(uint8_t b)
{
  if (b == BIT_STOCKS)
    return BIT_WEATHER;
  if (b == BIT_WEATHER)
    return BIT_CLOCK;
  return BIT_STOCKS;
}

// Advance currentBit to the next enabled bit that has data. If no enabled
// bit has data, leave currentBit unchanged so showNext can show a loading hint.
static void advanceCategory()
{
  uint8_t start = currentBit;
  for (int i = 0; i < 3; i++)
  {
    currentBit = nextBit(currentBit);
    if ((enabledMask & currentBit) && bitHasData(currentBit))
    {
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

static uint8_t firstActiveBit()
{
  const uint8_t order[] = {BIT_STOCKS, BIT_WEATHER, BIT_CLOCK};
  for (uint8_t b : order)
    if ((enabledMask & b) && bitHasData(b))
      return b;
  for (uint8_t b : order)
    if (enabledMask & b)
      return b;
  return BIT_CLOCK;
}

// --- Mode transitions ---

// All three enter*() helpers call displayClear() so transitions between modes
// are clean — important specifically for IDLE→anything else, where tickIdle's
// raw setPoint() leaves a lit row-7 pixel that the scroll pump or static-clock
// path won't necessarily overwrite. enterIdle() is the exception when re-
// entered while already in IDLE (idempotent path): position state is preserved
// and no clear is issued, so the bouncing pixel doesn't snap back to col 0.
//
// Brightness also tracks the mode: MODE_IDLE drops to intensity 0 (the
// quietest non-off setting — one dim pixel says "alive" without distraction)
// and the other two modes restore DISPLAY_INTENSITY. tickActiveStatus()
// handles the sign-overrides-idle case by restoring intensity itself.

void enterContent()
{
  currentMode = MODE_CONTENT;
  currentStock = 0;
  currentWeather = 0;
  currentBit = firstActiveBit();
  display.setIntensity(DISPLAY_INTENSITY);
  display.displayClear();
}

bool maskPrereqsReady(uint8_t mask)
{
  if ((mask & BIT_STOCKS) && (!wifiConfigured() || !apiKeyConfigured()))
    return false;
  if ((mask & BIT_WEATHER) && !wifiConfigured())
    return false;
  if ((mask & BIT_CLOCK) && !wifiConfigured())
    return false;
  return true;
}

void enterSetup(uint8_t targetMask)
{
  currentMode = MODE_SETUP;
  setupTargetMask = targetMask ? targetMask : MASK_ALL;
  setupLastActivityMs = millis();
  display.setIntensity(DISPLAY_INTENSITY);
  display.displayClear();
}

void enterIdle()
{
  // Re-entry while already in MODE_IDLE preserves the bouncing-pixel
  // position (no col=0 snap-back) but still clears + flags a first paint
  // so any out-of-band display content — typically a sign that overrode
  // idle and just cleared — gets wiped before tickIdle resumes.
  bool wasIdle = (currentMode == MODE_IDLE);
  currentMode = MODE_IDLE;
  if (!wasIdle)
  {
    idlePixelCol = 0;
    idlePixelRow = 0;
    idlePixelDirX = 1;
    idlePixelDirY = 1;
  }
  idleLastStepMs = 0;
  idleNeedsFirstPaint = true;
  display.setIntensity(0);
  display.displayClear();
}

// Formats the current mode for BLE read responses and debug logs:
//   "setup" — in MODE_SETUP
//   "none"  — enabledMask == 0 (explicit sign-only / always-idle)
//   "all"   — MODE_CONTENT (or MODE_IDLE) with the full mask
//   "stocks,weather" etc. for any subset.
// MODE_IDLE on its own is a transient display state and reports the
// underlying enabledMask the same way MODE_CONTENT does — so clients see
// the categories that *will* rotate once prereqs are met. The "none"
// branch is different: it reflects the user's explicit "no categories"
// selection, which persists across reboots.
static int formatModeName(char *buf, size_t bufLen)
{
  if (currentMode == MODE_SETUP)
    return snprintf(buf, bufLen, "setup");
  if (enabledMask == 0)
    return snprintf(buf, bufLen, "none");
  if (enabledMask == MASK_ALL)
    return snprintf(buf, bufLen, "all");
  int len = 0;
  const char *sep = "";
  if (enabledMask & BIT_STOCKS)
  {
    len += snprintf(buf + len, bufLen - len, "%sstocks", sep);
    sep = ",";
  }
  if (enabledMask & BIT_WEATHER)
  {
    len += snprintf(buf + len, bufLen - len, "%sweather", sep);
    sep = ",";
  }
  if (enabledMask & BIT_CLOCK)
  {
    len += snprintf(buf + len, bufLen - len, "%sclock", sep);
    sep = ",";
  }
  return len;
}

void exitSetupIfReady()
{
  // Handles MODE_SETUP (resume into the saved target mask) and MODE_IDLE
  // (resume into the existing enabledMask). Both are "waiting for prereqs"
  // states; once prereqs are met for the relevant mask, drop into content.
  // An explicit mask=0 ("none") in MODE_IDLE is sticky — never exits.
  if (currentMode == MODE_SETUP)
  {
    if (!maskPrereqsReady(setupTargetMask))
      return;
    enabledMask = setupTargetMask;
    saveDisplayMaskToNVS();
  }
  else if (currentMode == MODE_IDLE)
  {
    if (enabledMask == 0 || !maskPrereqsReady(enabledMask))
      return;
  }
  else
  {
    return;
  }
  enterContent();
  char buf[64];
  formatModeName(buf, sizeof(buf));
  Serial.printf("Prereqs satisfied, exiting to %s\n", buf);
}

void showNextSetup()
{
  scrollText(bleDeviceName);
}

void showNext()
{
  if (currentMode == MODE_SETUP)
  {
    showNextSetup();
    return;
  }
  // MODE_IDLE renders via tickIdle() in loop(), never through the scroll
  // pump. Bail so a stray showNext() doesn't clobber the bouncing pixel.
  if (currentMode == MODE_IDLE)
    return;

  // Defensive: an out-of-mask currentBit can occur after a mask change.
  if (!(enabledMask & currentBit))
    currentBit = firstActiveBit();

  // If the current bit has no data right now (pre-first-fetch, WiFi drop),
  // slide to an enabled bit that does. If none do, show a loading hint.
  if (!bitHasData(currentBit))
  {
    advanceCategory();
    if (!bitHasData(currentBit))
    {
      if (currentBit == BIT_STOCKS)
        scrollText("Loading stocks...");
      else if (currentBit == BIT_WEATHER)
        scrollText("Loading weather...");
      else if (currentBit == BIT_CLOCK)
        scrollText("Loading time...");
      return;
    }
  }

  if (currentBit == BIT_STOCKS)
  {
    showNextStock();
    if (currentStock == 0)
      advanceCategory();
  }
  else if (currentBit == BIT_WEATHER)
  {
    showNextWeather();
    if (currentWeather == 0)
      advanceCategory();
  }
  else if (currentBit == BIT_CLOCK)
  {
    showNextClock();
    advanceCategory(); // one item per pass — always rotate after showing
  }
}

// --- Active-status rendering ---
// Short text (≤ STATUS_STATIC_MAX_CHARS) renders steady — the text being lit
// is the signal; a hard blink reads as "alarm" rather than "state." Longer
// text scrolls on a loop. `statusShown` caches what's currently on the
// matrix so tickActiveStatus skips redraws when nothing changed; writers
// that wipe the display out-of-band call invalidateStatusRender() so the
// next tick repaints rather than no-oping on a stale equality.

static char statusShown[STATUS_MAX_LEN] = "";
static bool statusShownIsScroll = false;

static void invalidateStatusRender()
{
  statusShown[0] = '\0';
}

// After a sign clears (manually or by expiry), pick the right ambient mode:
// content if prereqs are met and at least one category is enabled, otherwise
// MODE_IDLE (bouncing pixel) so we don't dump the user into a "Loading X..."
// spam loop, back into the setup-name scroll they've already seen, or out
// of an explicit "none" selection.
static void resumeAmbient()
{
  if (enabledMask != 0 && maskPrereqsReady(enabledMask))
    enterContent();
  else
    enterIdle();
}

// Combined gate + expiry check. Returns true if status should render right
// now; if a timed status has passed its expiry, clears and resumes ambient
// in-place (so the caller just falls through to the normal render path).
// Single millis() call per invocation instead of two we'd pay if expiry
// and the render gate were separate functions.
bool checkStatusForRender()
{
  if (activeStatusText[0] == '\0')
    return false;
  if (statusExpiresAt == 0 || statusExpiresAt == UINT32_MAX)
    return true;  // 0 is defensive (shouldn't happen w/ text non-empty); UINT32_MAX = indefinite
  // Signed-delta is wrap-safe: if statusExpiresAt is still in the future,
  // (millis() - statusExpiresAt) underflows to a large unsigned which cast
  // to int32_t is negative.
  if ((int32_t)(millis() - statusExpiresAt) < 0)
    return true;

  Serial.printf("Status: \"%s\" expired, clearing\n", activeStatusText);
  clearStatus();
  invalidateStatusRender();
  resumeAmbient();  // enter*() helpers clear the display on transition
  return false;
}

static void clearActiveStatusAndResume()
{
  clearStatus();
  invalidateStatusRender();
  resumeAmbient();  // enter*() helpers clear the display on transition
}

// Static signs sit still by default — give them subtle "breathing" by
// stepping the MAX7219 intensity between SIGN_BREATH_MIN/MAX_INTENSITY every
// SIGN_BREATH_STEP_MS (see config.h). Scrolling signs already have motion,
// so they keep the steady DISPLAY_INTENSITY.
static int signBreathLevel = DISPLAY_INTENSITY;
static int signBreathDir = 1;
static unsigned long signBreathStepMs = 0;

void tickActiveStatus()
{
  if (strcmp(statusShown, activeStatusText) != 0)
  {
    strncpy(statusShown, activeStatusText, sizeof(statusShown) - 1);
    statusShown[sizeof(statusShown) - 1] = '\0';
    statusShownIsScroll = strlen(activeStatusText) > STATUS_STATIC_MAX_CHARS;
    // Restore normal brightness in case we were dim from MODE_IDLE.
    display.setIntensity(DISPLAY_INTENSITY);
    display.displayClear();
    if (statusShownIsScroll)
    {
      strncpy(scrollBuf, activeStatusText, sizeof(scrollBuf) - 1);
      scrollBuf[sizeof(scrollBuf) - 1] = '\0';
      display.displayScroll(scrollBuf, PA_LEFT, PA_SCROLL_LEFT, SCROLL_SPEED);
    }
    else
    {
      display.displayText(activeStatusText, PA_CENTER, 0, 0, PA_PRINT, PA_NO_EFFECT);
      display.displayAnimate();
      // Reset breathing for a fresh start on the new sign.
      signBreathLevel = DISPLAY_INTENSITY;
      signBreathDir = 1;
      signBreathStepMs = millis();
    }
  }

  if (statusShownIsScroll)
  {
    // Scroll path: just pump the animation. No breathing.
    if (display.displayAnimate())
      display.displayReset(); // loop the same scroll
    return;
  }

  // Static path: step the breathing intensity when the timer elapses.
  unsigned long now = millis();
  if (now - signBreathStepMs < SIGN_BREATH_STEP_MS)
    return;
  signBreathStepMs = now;
  signBreathLevel += signBreathDir;
  if (signBreathLevel >= SIGN_BREATH_MAX_INTENSITY)
  {
    signBreathLevel = SIGN_BREATH_MAX_INTENSITY;
    signBreathDir = -1;
  }
  else if (signBreathLevel <= SIGN_BREATH_MIN_INTENSITY)
  {
    signBreathLevel = SIGN_BREATH_MIN_INTENSITY;
    signBreathDir = 1;
  }
  display.setIntensity(signBreathLevel);
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
void buildDeviceName()
{
  uint64_t mac = ESP.getEfuseMac();
  snprintf(bleDeviceName, sizeof(bleDeviceName), "%s-%02X%02X",
           BLE_DEVICE_NAME,
           (uint8_t)((mac >> 8) & 0xFF),
           (uint8_t)(mac & 0xFF));
}

// NimBLE-Arduino stops advertising on connect and does NOT auto-resume on
// disconnect. Without this, a single connection (even a brief or accidental
// one) makes the device invisible to subsequent scans until reboot — the
// classic "iPhone can't see my LED-Ticker anymore" symptom.
class ServerCallbacks : public NimBLEServerCallbacks
{
  void onDisconnect(NimBLEServer *) override
  {
    Serial.println("BLE: client disconnected, resuming advertising");
    NimBLEDevice::startAdvertising();
  }
};

// ----------------------------------------------------------------------------
// BLE: WiFi
// ----------------------------------------------------------------------------

#define BLE_WIFI_CHAR_UUID "beb5483e-36e1-4688-b7f5-ea07361b26ac"
#define BLE_WIFI_BUF_LEN (WIFI_SSID_MAX + WIFI_PASS_MAX + 1)

volatile bool wifiUpdatePending = false;
char pendingWifiStr[BLE_WIFI_BUF_LEN];

class WifiCallbacks : public NimBLECharacteristicCallbacks
{
  void onWrite(NimBLECharacteristic *pChar) override
  {
    setupLastActivityMs = millis();
    std::string val = pChar->getValue();
    if (val.length() > 0 && val.length() < BLE_WIFI_BUF_LEN)
    {
      memcpy(pendingWifiStr, val.c_str(), val.length());
      pendingWifiStr[val.length()] = '\0';
      wifiUpdatePending = true;
    }
  }

  void onRead(NimBLECharacteristic *pChar) override
  {
    // Return SSID only — never expose the password over BLE
    pChar->setValue((uint8_t *)nvsWifiSsid, strlen(nvsWifiSsid));
  }
};

void applyPendingWifi()
{
  wifiUpdatePending = false;

  // Split on first '|' — password may contain '|'
  char *sep = strchr(pendingWifiStr, '|');
  if (!sep)
  {
    Serial.println("BLE wifi: missing '|' separator, ignoring");
    return;
  }

  *sep = '\0';
  const char *ssid = pendingWifiStr;
  const char *pass = sep + 1;

  if (strlen(ssid) == 0 || strlen(ssid) >= WIFI_SSID_MAX)
  {
    Serial.println("BLE wifi: invalid SSID, ignoring");
    return;
  }

  strncpy(nvsWifiSsid, ssid, WIFI_SSID_MAX - 1);
  nvsWifiSsid[WIFI_SSID_MAX - 1] = '\0';
  strncpy(nvsWifiPass, pass, WIFI_PASS_MAX - 1);
  nvsWifiPass[WIFI_PASS_MAX - 1] = '\0';
  saveWifiToNVS();

  Serial.printf("BLE wifi: reconnecting to \"%s\"\n", nvsWifiSsid);
  WiFi.disconnect();
  connectWifi();
  // Defer NTP/SNTP startup until WiFi is up, so we don't kick off a daemon
  // that would just retry-loop without network. initTime() guards on
  // WiFi.status() so this is a no-op if the reconnect didn't take.
  if (!timeReady)
    initTime();
  exitSetupIfReady();
}

// ----------------------------------------------------------------------------
// BLE: Finnhub API key
// ----------------------------------------------------------------------------

#define BLE_APIKEY_CHAR_UUID "beb5483e-36e1-4688-b7f5-ea07361b26ad"

volatile bool apiKeyUpdatePending = false;
char pendingApiKey[MAX_APIKEY_LEN];

class ApiKeyCallbacks : public NimBLECharacteristicCallbacks
{
  void onWrite(NimBLECharacteristic *pChar) override
  {
    setupLastActivityMs = millis();
    std::string val = pChar->getValue();
    if (val.length() > 0 && val.length() < MAX_APIKEY_LEN)
    {
      memcpy(pendingApiKey, val.c_str(), val.length());
      pendingApiKey[val.length()] = '\0';
      apiKeyUpdatePending = true;
    }
  }

  void onRead(NimBLECharacteristic *pChar) override
  {
    pChar->setValue((uint8_t *)nvsApiKey, strlen(nvsApiKey));
  }
};

void applyPendingApiKey()
{
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

class TickerCallbacks : public NimBLECharacteristicCallbacks
{
  void onWrite(NimBLECharacteristic *pChar) override
  {
    setupLastActivityMs = millis();
    if (millis() - lastBLEFetchMs < BLE_FETCH_COOLDOWN_MS)
    {
      Serial.println("BLE tickers: cooldown, ignoring");
      return;
    }
    std::string val = pChar->getValue();
    if (val.length() > 0 && val.length() < BLE_TICKER_BUF_LEN)
    {
      memcpy(pendingTickerStr, val.c_str(), val.length());
      pendingTickerStr[val.length()] = '\0';
      tickerUpdatePending = true;
      lastBLEFetchMs = millis();
    }
  }

  void onRead(NimBLECharacteristic *pChar) override
  {
    char buf[BLE_TICKER_BUF_LEN];
    int len = 0;
    for (int i = 0; i < nvsTickerCount && len < (int)sizeof(buf) - 1; i++)
    {
      if (i > 0)
        buf[len++] = ',';
      int remaining = sizeof(buf) - 1 - len;
      int tlen = strnlen(nvsTickers[i], remaining);
      memcpy(buf + len, nvsTickers[i], tlen);
      len += tlen;
    }
    buf[len] = '\0';
    pChar->setValue((uint8_t *)buf, len);
  }
};

void applyPendingTickers()
{
  char buf[BLE_TICKER_BUF_LEN];
  strncpy(buf, pendingTickerStr, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';
  tickerUpdatePending = false;

  char tmp[MAX_STOCKS][MAX_TICKER_LEN];
  int count = 0;

  char *token = strtok(buf, ",");
  while (token && count < MAX_STOCKS)
  {
    while (*token == ' ')
      token++;
    int len = strlen(token);
    while (len > 0 && token[len - 1] == ' ')
      len--;
    token[len] = '\0';

    if (len > 0 && len < MAX_TICKER_LEN)
    {
      strncpy(tmp[count], token, MAX_TICKER_LEN - 1);
      tmp[count][MAX_TICKER_LEN - 1] = '\0';
      for (int j = 0; tmp[count][j]; j++)
        tmp[count][j] = toupper((unsigned char)tmp[count][j]);
      count++;
    }
    token = strtok(nullptr, ",");
  }

  if (count == 0)
  {
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

class LocsCallbacks : public NimBLECharacteristicCallbacks
{
  void onWrite(NimBLECharacteristic *pChar) override
  {
    setupLastActivityMs = millis();
    if (millis() - lastBLEFetchMs < BLE_FETCH_COOLDOWN_MS)
    {
      Serial.println("BLE locations: cooldown, ignoring");
      return;
    }
    std::string val = pChar->getValue();
    if (val.length() > 0 && val.length() < BLE_LOCS_BUF_LEN)
    {
      memcpy(pendingLocsStr, val.c_str(), val.length());
      pendingLocsStr[val.length()] = '\0';
      locsUpdatePending = true;
      lastBLEFetchMs = millis();
    }
  }

  void onRead(NimBLECharacteristic *pChar) override
  {
    char buf[BLE_LOCS_BUF_LEN];
    int len = 0;
    for (int i = 0; i < nvsLocationCount && len < (int)sizeof(buf) - 1; i++)
    {
      if (i > 0 && len < (int)sizeof(buf) - 1)
        buf[len++] = '|';
      int remaining = sizeof(buf) - 1 - len;
      int llen = strnlen(nvsLocations[i], remaining);
      memcpy(buf + len, nvsLocations[i], llen);
      len += llen;
    }
    buf[len] = '\0';
    pChar->setValue((uint8_t *)buf, len);
  }
};

void applyPendingLocations()
{
  char buf[BLE_LOCS_BUF_LEN];
  strncpy(buf, pendingLocsStr, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';
  locsUpdatePending = false;

  char tmp[MAX_LOCATIONS][MAX_LOCATION_LEN];
  int count = 0;

  char *token = strtok(buf, "|");
  while (token && count < MAX_LOCATIONS)
  {
    while (*token == ' ')
      token++;
    int len = strlen(token);
    while (len > 0 && token[len - 1] == ' ')
      len--;
    token[len] = '\0';

    if (len > 0 && len < MAX_LOCATION_LEN)
    {
      strncpy(tmp[count], token, MAX_LOCATION_LEN - 1);
      tmp[count][MAX_LOCATION_LEN - 1] = '\0';
      count++;
    }
    token = strtok(nullptr, "|");
  }

  if (count == 0)
  {
    Serial.println("BLE: no valid locations, ignoring");
    return;
  }

  for (int i = 0; i < count; i++)
    strncpy(nvsLocations[i], tmp[i], MAX_LOCATION_LEN);
  nvsLocationCount = count;
  for (int i = 0; i < MAX_LOCATIONS; i++)
    resolved[i].ok = false;
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

class ModeCallbacks : public NimBLECharacteristicCallbacks
{
  void onWrite(NimBLECharacteristic *pChar) override
  {
    setupLastActivityMs = millis();
    std::string val = pChar->getValue();
    if (val.length() > 0 && val.length() < sizeof(pendingModeStr))
    {
      memcpy(pendingModeStr, val.c_str(), val.length());
      pendingModeStr[val.length()] = '\0';
      modeUpdatePending = true;
    }
  }

  void onRead(NimBLECharacteristic *pChar) override
  {
    char buf[64];
    int len = formatModeName(buf, sizeof(buf));
    pChar->setValue((uint8_t *)buf, len);
  }
};

// Accepts "all" or a comma-separated subset of {stocks, weather, clock}.
// Returns 0 on unknown token, empty input, or empty mask after parse.
static uint8_t parseModePayload(const char *in)
{
  if (strcmp(in, "all") == 0)
    return MASK_ALL;
  if (strcmp(in, "none") == 0)
    return MASK_NONE_REQUEST;

  char buf[64];
  strncpy(buf, in, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';

  uint8_t mask = 0;
  char *tok = strtok(buf, ",");
  while (tok)
  {
    while (*tok == ' ' || *tok == '\t')
      tok++;
    char *end = tok + strlen(tok);
    while (end > tok && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' || end[-1] == '\r'))
    {
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

void applyPendingMode()
{
  modeUpdatePending = false;
  uint8_t mask = parseModePayload(pendingModeStr);
  if (mask == 0)
  {
    Serial.printf("BLE: unknown/empty mode \"%s\", ignoring\n", pendingModeStr);
    return;
  }
  if (mask == MASK_NONE_REQUEST)
  {
    enabledMask = 0;
    saveDisplayMaskToNVS();
    enterIdle();
  }
  else
  {
    enabledMask = mask;
    saveDisplayMaskToNVS();
    if (!maskPrereqsReady(mask))
      enterSetup(mask);
    else
    {
      enterContent();
      // User just asked for these categories — fetch now so they don't
      // wait up to FETCH_INTERVAL_MS for the next periodic tick to fire.
      // force=true bypasses the market-hours gate (showing the last
      // close beats "Loading stocks..." when the market is shut).
      triggerFetch(true);
    }
  }
  char buf[64];
  formatModeName(buf, sizeof(buf));
  Serial.printf("BLE: mode -> %s\n", buf);
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

class StatusCallbacks : public NimBLECharacteristicCallbacks
{
  void onWrite(NimBLECharacteristic *pChar) override
  {
    setupLastActivityMs = millis();
    std::string val = pChar->getValue();
    if (val.length() >= BLE_STATUS_BUF_LEN)
      return;
    memcpy(pendingStatusStr, val.c_str(), val.length());
    pendingStatusStr[val.length()] = '\0';
    statusUpdatePending = true;
  }

  void onRead(NimBLECharacteristic *pChar) override
  {
    char buf[BLE_STATUS_BUF_LEN];
    if (activeStatusText[0] == '\0')
    {
      pChar->setValue((uint8_t *)buf, 0);
      return;
    }
    uint32_t remaining = 0; // 0 means "indefinite" on the read side
    if (statusExpiresAt != 0 && statusExpiresAt != UINT32_MAX)
    {
      int32_t deltaMs = (int32_t)(statusExpiresAt - millis());
      remaining = (deltaMs > 0) ? (uint32_t)(deltaMs / 1000) : 1;
    }
    int len = snprintf(buf, sizeof(buf), "%s|%u", activeStatusText, remaining);
    if (len < 0)
      len = 0;
    if (len >= (int)sizeof(buf))
      len = sizeof(buf) - 1;
    pChar->setValue((uint8_t *)buf, len);
  }
};

void applyPendingStatus()
{
  char buf[BLE_STATUS_BUF_LEN];
  strncpy(buf, pendingStatusStr, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';
  statusUpdatePending = false;

  if (buf[0] == '\0')
  {
    if (activeStatusText[0])
      Serial.println("BLE status: cleared");
    clearActiveStatusAndResume();
    return;
  }

  // strrchr (last '|') rather than strchr (first) because status text may
  // contain pipes in theory; the seconds tail never does.
  char *sep = strrchr(buf, '|');
  if (!sep)
  {
    Serial.println("BLE status: missing '|' separator, ignoring");
    return;
  }

  *sep = '\0';
  char *text = buf;
  const char *tail = sep + 1;

  while (*text == ' ')
    text++;
  int textLen = strlen(text);
  while (textLen > 0 && text[textLen - 1] == ' ')
    text[--textLen] = '\0';
  if (textLen == 0)
  {
    if (activeStatusText[0])
      Serial.println("BLE status: empty text, clearing");
    clearActiveStatusAndResume();
    return;
  }

  char *tailEnd = nullptr;
  unsigned long secs = strtoul(tail, &tailEnd, 10);
  if (tailEnd == tail)
  {
    Serial.println("BLE status: bad seconds value, ignoring");
    return;
  }

  strncpy(activeStatusText, text, STATUS_MAX_LEN - 1);
  activeStatusText[STATUS_MAX_LEN - 1] = '\0';

  if (secs == 0)
  {
    statusExpiresAt = UINT32_MAX;
    Serial.printf("BLE status: \"%s\" indefinite\n", activeStatusText);
  }
  else
  {
    // millis()-based: relative timing works without WiFi/NTP. Avoid the
    // 0 and UINT32_MAX sentinels in the unlikely target collision.
    uint32_t target = millis() + secs * 1000UL;
    if (target == 0 || target == UINT32_MAX)
      target = 1;
    statusExpiresAt = target;
    Serial.printf("BLE status: \"%s\" for %lus\n", activeStatusText, secs);
  }
  invalidateStatusRender();
  display.displayClear();

  // Sign-only inference: writing a sign on a no-WiFi device is a strong
  // signal that this unit is being used as a sign, not as an ambient
  // ticker. Persist that intent so the next boot lands in MODE_IDLE
  // (bouncing pixel) instead of MODE_SETUP (scrolling BLE name). Setting
  // WiFi credentials later doesn't auto-restore categories — the user
  // writes a real mode (`mode=all`, etc.) to re-enable ambient rotation.
  if (!wifiConfigured() && enabledMask != 0)
  {
    Serial.println("BLE status: no-WiFi + first sign — persisting mode=none");
    enabledMask = 0;
    saveDisplayMaskToNVS();
  }
}

// ----------------------------------------------------------------------------
// BLE: Power (display on/off toggle)
// ----------------------------------------------------------------------------
// Orthogonal to Mode and Status: a RAM-only boolean that, when true, makes
// the device visually inert (matrix dark, onboard NeoPixel dark, signs
// suppressed, fetches paused). Not persisted — power cycle returns to false.
// The `displayOff` flag itself is declared up next to fetching/ledState so
// updateStatusLed() can read it; this section owns everything else.

#define BLE_POWER_CHAR_UUID "beb5483e-36e1-4688-b7f5-ea07361b26b1"

NimBLECharacteristic *pPowerChar = nullptr;

volatile bool powerUpdatePending = false;
char pendingPowerStr[8]; // big enough for "on" / "off" with NUL

class PowerCallbacks : public NimBLECharacteristicCallbacks
{
  void onWrite(NimBLECharacteristic *pChar) override
  {
    std::string val = pChar->getValue();
    setupLastActivityMs = millis();
    if (val.length() > 0 && val.length() < sizeof(pendingPowerStr))
    {
      memcpy(pendingPowerStr, val.c_str(), val.length());
      pendingPowerStr[val.length()] = '\0';
      powerUpdatePending = true;
    }
  }
};

void setPower(bool off)
{
  if (off == displayOff) return; // idempotent

  displayOff = off;

  if (off)
  {
    display.displayClear();
    // Eager NeoPixel kill — updateStatusLed() only repaints on fetch-flag
    // transitions, so a mid-flight fetch would leave a stale blue dot lit.
    // Sync ledState too so the next updateStatusLed() tick doesn't re-issue
    // a redundant write through its own displayOff guard.
    neopixelWrite(RGB_LED_PIN, 0, 0, 0);
    ledState = false;
    Serial.println("Power: display OFF");
  }
  else
  {
    // Coming back on: any active sign needs to repaint (its render-cache
    // booleans are stale), and content modes deserve fresh data.
    invalidateStatusRender();
    triggerFetch();
    Serial.println("Power: display ON");
  }

  pPowerChar->setValue(off ? "off" : "on");
}

void applyPendingPower()
{
  char buf[sizeof(pendingPowerStr)];
  strncpy(buf, pendingPowerStr, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';
  powerUpdatePending = false;

  // Tolerant of " ON\n", "Off", etc. — iOS and led.py both send unadorned
  // tokens but a manual BLE GUI client may add trailing whitespace.
  char *p = buf;
  while (*p == ' ' || *p == '\t') p++;
  char tok[8];
  size_t i = 0;
  while (p[i] && p[i] != ' ' && p[i] != '\t' && p[i] != '\n' && p[i] != '\r' && i < sizeof(tok) - 1)
  {
    tok[i] = (p[i] >= 'A' && p[i] <= 'Z') ? (p[i] + 32) : p[i];
    i++;
  }
  tok[i] = '\0';

  if (strcmp(tok, "off") == 0)
  {
    setPower(true);
  }
  else if (strcmp(tok, "on") == 0)
  {
    setPower(false);
  }
  else
  {
    Serial.printf("BLE power: unknown value \"%s\", ignoring\n", pendingPowerStr);
  }
}

// ----------------------------------------------------------------------------
// BLE: Version (read-only)
// ----------------------------------------------------------------------------

#define BLE_VERSION_CHAR_UUID "beb5483e-36e1-4688-b7f5-ea07361b26b0"

class VersionCallbacks : public NimBLECharacteristicCallbacks
{
  void onRead(NimBLECharacteristic *pChar) override
  {
    pChar->setValue(FW_VERSION);
  }
};

// ----------------------------------------------------------------------------
// BLE: Cmd (reload / reset)
// ----------------------------------------------------------------------------

#define BLE_CMD_CHAR_UUID "beb5483e-36e1-4688-b7f5-ea07361b26ab"

volatile bool cmdPending = false;
char pendingCmd[16];

class CmdCallbacks : public NimBLECharacteristicCallbacks
{
  void onWrite(NimBLECharacteristic *pChar) override
  {
    setupLastActivityMs = millis();
    std::string val = pChar->getValue();
    if (val.length() == 0 || val.length() >= sizeof(pendingCmd))
      return;

    // reload and reset trigger network activity — apply cooldown
    bool fetchCmd = (val == "reload" || val == "reset");
    if (fetchCmd && millis() - lastBLEFetchMs < BLE_FETCH_COOLDOWN_MS)
    {
      Serial.println("BLE cmd: cooldown, ignoring");
      return;
    }

    memcpy(pendingCmd, val.c_str(), val.length());
    pendingCmd[val.length()] = '\0';
    cmdPending = true;
    if (fetchCmd)
      lastBLEFetchMs = millis();
  }
};

void applyPendingCmd()
{
  cmdPending = false;

  if (strcmp(pendingCmd, "reload") == 0)
  {
    Serial.println("BLE cmd: reloading stocks");
    triggerFetch(true);
  }
  else if (strcmp(pendingCmd, "reset") == 0)
  {
    Serial.println("BLE cmd: resetting to defaults");

    prefs.begin("wifi", false);
    prefs.clear();
    prefs.end();
    prefs.begin("apikey", false);
    prefs.clear();
    prefs.end();
    prefs.begin("tickers", false);
    prefs.clear();
    prefs.end();
    // "msgs" and "status" are tombstone namespaces — wipe any leftover data
    // so a downgrade-then-upgrade doesn't surface stale entries.
    prefs.begin("msgs", false);
    prefs.clear();
    prefs.end();
    prefs.begin("status", false);
    prefs.clear();
    prefs.end();
    prefs.begin("locs", false);
    prefs.clear();
    prefs.end();
    prefs.begin("display", false);
    prefs.clear();
    prefs.end();

    loadTickersFromNVS();   // re-seeds from config.h since NVS is now empty
    loadLocationsFromNVS(); // same — re-seeds default locations
    enabledMask = MASK_ALL;
    setPower(false); // ensure display is on so MODE_SETUP scroll is visible

    // wifiConfigured()/apiKeyConfigured() read these RAM copies, not NVS.
    nvsWifiSsid[0] = '\0';
    nvsWifiPass[0] = '\0';
    nvsApiKey[0] = '\0';
    WiFi.disconnect();

    activeStatusText[0] = '\0';
    statusExpiresAt = 0;
    invalidateStatusRender();
    stockCount = 0;
    weatherCount = 0;
    currentWeather = 0;
    enterSetup(MASK_ALL);
  }
  else
  {
    Serial.printf("BLE cmd: unknown command \"%s\"\n", pendingCmd);
  }
}

// ----------------------------------------------------------------------------
// BLE init
// ----------------------------------------------------------------------------

void initBLE()
{
  NimBLEDevice::init(bleDeviceName);
  NimBLEDevice::setMTU(512);
  NimBLEServer *pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());
  NimBLEService *pService = pServer->createService(BLE_SERVICE_UUID);

  pService->createCharacteristic(BLE_TICKER_CHAR_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE)
      ->setCallbacks(new TickerCallbacks());
  pService->createCharacteristic(BLE_MODE_CHAR_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE)
      ->setCallbacks(new ModeCallbacks());
  pService->createCharacteristic(BLE_CMD_CHAR_UUID, NIMBLE_PROPERTY::WRITE)
      ->setCallbacks(new CmdCallbacks());
  pService->createCharacteristic(BLE_WIFI_CHAR_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE)
      ->setCallbacks(new WifiCallbacks());
  pService->createCharacteristic(BLE_APIKEY_CHAR_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE)
      ->setCallbacks(new ApiKeyCallbacks());
  pService->createCharacteristic(BLE_LOCS_CHAR_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE)
      ->setCallbacks(new LocsCallbacks());
  pService->createCharacteristic(BLE_STATUS_CHAR_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE)
      ->setCallbacks(new StatusCallbacks());
  NimBLECharacteristic *pVersionChar =
      pService->createCharacteristic(BLE_VERSION_CHAR_UUID, NIMBLE_PROPERTY::READ);
  pVersionChar->setCallbacks(new VersionCallbacks());
  // Seed the value so the very first read after connect returns immediately
  // even if onRead hasn't fired yet on this peer.
  pVersionChar->setValue(FW_VERSION);

  pPowerChar =
      pService->createCharacteristic(BLE_POWER_CHAR_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);
  pPowerChar->setCallbacks(new PowerCallbacks());
  pPowerChar->setValue("on");

  pService->start();
  NimBLEAdvertising *pAdv = NimBLEDevice::getAdvertising();
  pAdv->addServiceUUID(BLE_SERVICE_UUID);
  pAdv->start();
  Serial.printf("BLE advertising as %s\n", bleDeviceName);
}

// ============================================================================
// Main
// ============================================================================

unsigned long lastFetch = 0;

void setup()
{
  Serial.begin(115200);
  // ESP32-S3 native USB-CDC defaults to a 250ms blocking write timeout —
  // when the device runs headless and the TX buffer fills, every Serial
  // print stalls the loop for up to 250ms (visible matrix stutter).
  // Setting to 0 drops bytes silently when no host is draining, so the
  // device behaves the same headless as it does with a monitor attached.
  Serial.setTxTimeoutMs(0);
  // Wait (up to 2s) for the USB host to enumerate so the version banner
  // actually lands in `pio device monitor`. Falls through after the
  // timeout so a headless boot isn't wedged here forever. (The TX
  // timeout above is what keeps later prints non-blocking; this wait is
  // only about the boot banner being visible.)
  unsigned long serialWaitStart = millis();
  while (!Serial && millis() - serialWaitStart < 2000)
    delay(10);
  Serial.printf("LED-Ticker firmware v%s\n", FW_VERSION);

  dataMutex = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(fetchTask, "fetchStocks", FETCH_TASK_STACK,
                          nullptr, 1, &fetchTaskHandle, 0);

  initDisplay();
  loadWifiFromNVS();
  loadApiKeyFromNVS();
  loadTickersFromNVS();
  loadLocationsFromNVS();
  loadDisplayMaskFromNVS();
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
  triggerFetch();
  lastFetch = millis();
}

void loop()
{
  // Diagnostic heartbeat: if the matrix freezes but heartbeats keep coming,
  // the SPI/Parola path is stuck. If heartbeats stop, the whole loop hung.
  // 30s cadence keeps the monitor quiet under normal use while still
  // giving a clear liveness signal.
  static unsigned long lastHeartbeatMs = 0;
  unsigned long nowMs = millis();
  if (nowMs - lastHeartbeatMs > 30000)
  {
    lastHeartbeatMs = nowMs;
    Serial.printf("[hb] v%s mode=%d mask=0x%02X fetching=%d heap=%u millis=%lu\n",
                  FW_VERSION, currentMode, enabledMask, fetching,
                  (unsigned)ESP.getFreeHeap(), nowMs);
  }

  if (wifiUpdatePending)
    applyPendingWifi();
  if (apiKeyUpdatePending)
    applyPendingApiKey();
  if (cmdPending)
    applyPendingCmd();
  if (modeUpdatePending)
    applyPendingMode();
  if (tickerUpdatePending)
    applyPendingTickers();
  if (locsUpdatePending)
    applyPendingLocations();
  if (statusUpdatePending)
    applyPendingStatus();
  if (powerUpdatePending)
    applyPendingPower();

  updateStatusLed();

  if (currentMode == MODE_SETUP &&
      millis() - setupLastActivityMs > SETUP_TIMEOUT_MS)
  {
    if (wifiConfigured())
    {
      Serial.println("Setup: 60s no activity, falling to content (mask unchanged)");
      enterContent();
    }
    else
    {
      // No WiFi means every category in the mask is dead — falling through
      // would just rotate "Loading X..." hints forever. The setup hint
      // ("Configure WiFi over BLE") is strictly better. Push the next
      // timeout check out so we don't re-evaluate this branch every loop.
      setupLastActivityMs = millis();
    }
  }

  if (displayOff)
  {
    // Display is off: skip all render AND fetch work. BLE writes still
    // flow through the pending-apply chain above so the user can turn
    // it back on.
    return;
  }

  if (checkStatusForRender())
  {
    tickActiveStatus();
  }
  else if (currentMode == MODE_IDLE)
  {
    tickIdle();
  }
  else if (currentMode == MODE_CONTENT && enabledMask == BIT_CLOCK && timeReady)
  {
    tickStaticClock();
  }
  else if (display.displayAnimate())
  {
    display.displayReset();
    showNext();
  }

  if (millis() - lastFetch > FETCH_INTERVAL_MS)
  {
    lastFetch = millis();
    triggerFetch();
  }
}
