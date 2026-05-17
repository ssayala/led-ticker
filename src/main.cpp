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

// --- Display Mode ---
// An active status sign overrides both modes until expiry/clear.
// 0x02 was once BIT_MESSAGES — tombstoned; legacy NVS masks are stripped
// via `& MASK_ALL` on load. When BIT_CLOCK is the only enabled bit, the
// display switches to a steady non-scrolling clock — see tickStaticClock().
enum {
  MODE_CONTENT,
  MODE_SETUP,
};
#define BIT_STOCKS   0x01
#define BIT_WEATHER  0x04
#define BIT_CLOCK    0x08
#define MASK_ALL     (BIT_STOCKS | BIT_WEATHER | BIT_CLOCK)
extern int currentMode;
extern uint8_t enabledMask;

// --- Hardware & Display Config ---

#define HARDWARE_TYPE MD_MAX72XX::FC16_HW
#define MAX_DEVICES 4
#define DIN_PIN 6
#define CLK_PIN 4
#define CS_PIN 5
#define SCROLL_SPEED 60
#define RGB_LED_PIN 48

Preferences prefs; // used on Core 1 only (setup + BLE apply handlers)

// --- WiFi Credentials ---

#define WIFI_SSID_MAX 64
#define WIFI_PASS_MAX 64

char nvsWifiSsid[WIFI_SSID_MAX];
char nvsWifiPass[WIFI_PASS_MAX];

void saveWifiToNVS() {
  prefs.begin("wifi", false);
  prefs.putString("ssid", nvsWifiSsid);
  prefs.putString("pass", nvsWifiPass);
  prefs.end();
  Serial.printf("WiFi credentials saved to NVS (SSID: %s)\n", nvsWifiSsid);
}

void loadWifiFromNVS() {
  prefs.begin("wifi", true);
  bool hasSsid = prefs.isKey("ssid");
  if (hasSsid) {
    prefs.getString("ssid", nvsWifiSsid, WIFI_SSID_MAX);
    prefs.getString("pass", nvsWifiPass, WIFI_PASS_MAX);
    Serial.printf("Loaded WiFi credentials from NVS (SSID: %s)\n", nvsWifiSsid);
  }
  else {
    nvsWifiSsid[0] = '\0';
    nvsWifiPass[0] = '\0';
    Serial.println("WiFi not configured — use BLE to set credentials");
  }
  prefs.end();
}

bool wifiConfigured() { return nvsWifiSsid[0] != '\0'; }

// --- Finnhub API Key ---

#define MAX_APIKEY_LEN 64

char nvsApiKey[MAX_APIKEY_LEN];

void saveApiKeyToNVS() {
  prefs.begin("apikey", false);
  prefs.putString("key", nvsApiKey);
  prefs.end();
  Serial.println("API key saved to NVS");
}

void loadApiKeyFromNVS() {
  prefs.begin("apikey", true);
  bool hasKey = prefs.isKey("key");
  if (hasKey) {
    prefs.getString("key", nvsApiKey, MAX_APIKEY_LEN);
    Serial.println("Loaded API key from NVS");
  }
  else {
    nvsApiKey[0] = '\0';
    Serial.println("Finnhub API key not configured — use BLE to set it");
  }
  prefs.end();
}

bool apiKeyConfigured() { return nvsApiKey[0] != '\0'; }

// --- Fetch Limits ---

#define MAX_STRING_LEN 96 // scroll buffer cell size
#define MAX_STOCKS 10
#define FETCH_INTERVAL_MS (5 * 60 * 1000)

// Active-status text fits in MAX_STRING_LEN; up to 5 chars renders static,
// longer scrolls. See tickActiveStatus().
#define STATUS_MAX_LEN MAX_STRING_LEN
#define STATUS_STATIC_MAX_CHARS 5

// --- Status LED ---

volatile bool fetching = false;
static bool ledState = false;

void updateStatusLed() {
  if (fetching && !ledState) {
    neopixelWrite(RGB_LED_PIN, 0, 0, 20);
    ledState = true;
  }
  else if (!fetching && ledState) {
    neopixelWrite(RGB_LED_PIN, 0, 0, 0);
    ledState = false;
  }
}

// --- Display ---

MD_Parola display = MD_Parola(HARDWARE_TYPE, CS_PIN, MAX_DEVICES);

void initDisplay() {
  SPI.begin(CLK_PIN, -1, DIN_PIN, CS_PIN);
  display.begin();
  display.setIntensity(2);
  display.displayClear();
}

void scrollText(const char* msg) {
  display.displayScroll(msg, PA_LEFT, PA_SCROLL_LEFT, SCROLL_SPEED);
}

// --- Active Status ---
// The "sign mode" override: when activeStatusText is non-empty and not yet
// expired, the loop renders it (steady if short, scrolling if long) in
// place of the normal ambient rotation. statusExpiresAt is a Unix epoch
// second; 0 means "no status active", UINT32_MAX means "indefinite (until
// cleared)". State is in-RAM only — a power cycle clears any active sign
// and the device resumes its ambient mode.
char activeStatusText[STATUS_MAX_LEN] = {0};
uint32_t statusExpiresAt = 0;

void clearStatus() {
  activeStatusText[0] = '\0';
  statusExpiresAt = 0;
}

// --- Stocks ---

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

// Fetch task — runs on Core 0 alongside the WiFi/BLE stack
#define FETCH_TASK_STACK 8192
static SemaphoreHandle_t dataMutex = nullptr;
static TaskHandle_t fetchTaskHandle = nullptr;
// MD_Parola stores the pointer passed to displayScroll, not a copy.
// Shared across all scrolling content (stocks/weather/clock/status/setup) —
// only one is ever active, and we overwrite only when starting the next scroll.
static char scrollBuf[MAX_STRING_LEN + 1];

void saveTickersToNVS() {
  prefs.begin("tickers", false);
  prefs.putInt("count", nvsTickerCount);
  for (int i = 0; i < nvsTickerCount; i++) {
    char key[8];
    snprintf(key, sizeof(key), "t%d", i);
    prefs.putString(key, nvsTickers[i]);
  }
  prefs.end();
}

void loadTickersFromNVS() {
  prefs.begin("tickers", true);
  int count = prefs.getInt("count", 0);
  if (count > 0 && count <= MAX_STOCKS) {
    for (int i = 0; i < count; i++) {
      char key[8];
      snprintf(key, sizeof(key), "t%d", i);
      prefs.getString(key, nvsTickers[i], MAX_TICKER_LEN);
    }
    prefs.end();
    nvsTickerCount = count;
    Serial.printf("Loaded %d tickers from NVS\n", count);
  }
  else {
    prefs.end();
    // First boot: seed from config.h defaults
    for (int i = 0; i < stockTickerCount && i < MAX_STOCKS; i++) {
      strncpy(nvsTickers[i], stockTickers[i], MAX_TICKER_LEN - 1);
      nvsTickers[i][MAX_TICKER_LEN - 1] = '\0';
    }
    nvsTickerCount = stockTickerCount;
    saveTickersToNVS();
    Serial.printf("Seeded %d tickers from defaults\n", nvsTickerCount);
  }
}

// --- Weather ---

#define MAX_LOCATIONS 5
#define MAX_LOCATION_LEN 40 // user-entered "City, State" or zip
#define MAX_LOC_NAME_LEN 24 // canonical name from geocoder

char nvsLocations[MAX_LOCATIONS][MAX_LOCATION_LEN];
int nvsLocationCount = 0;

struct ResolvedLocation {
  bool ok;
  float lat;
  float lon;
  char name[MAX_LOC_NAME_LEN];
};
ResolvedLocation resolved[MAX_LOCATIONS];

struct WeatherReading {
  char name[MAX_LOC_NAME_LEN];
  float tempF;
};

WeatherReading weatherReadings[MAX_LOCATIONS];
int weatherCount = 0;
int currentWeather = 0;

void saveLocationsToNVS() {
  prefs.begin("locs", false);
  prefs.putInt("count", nvsLocationCount);
  for (int i = 0; i < nvsLocationCount; i++) {
    char key[8];
    snprintf(key, sizeof(key), "l%d", i);
    prefs.putString(key, nvsLocations[i]);
  }
  prefs.end();
}

void loadLocationsFromNVS() {
  prefs.begin("locs", true);
  int count = prefs.getInt("count", 0);
  if (count > 0 && count <= MAX_LOCATIONS) {
    for (int i = 0; i < count; i++) {
      char key[8];
      snprintf(key, sizeof(key), "l%d", i);
      prefs.getString(key, nvsLocations[i], MAX_LOCATION_LEN);
    }
    prefs.end();
    nvsLocationCount = count;
    Serial.printf("Loaded %d locations from NVS\n", count);
  }
  else {
    prefs.end();
    // First boot: seed from config.h defaults
    for (int i = 0; i < defaultLocationCount && i < MAX_LOCATIONS; i++) {
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

// --- Display Mask (persisted) ---
uint8_t enabledMask = MASK_ALL;

void saveDisplayMaskToNVS() {
  prefs.begin("display", false);
  prefs.putUChar("mask", enabledMask);
  prefs.end();
}

void loadDisplayMaskFromNVS() {
  prefs.begin("display", true);
  if (prefs.isKey("mask")) {
    uint8_t m = prefs.getUChar("mask", MASK_ALL) & MASK_ALL;
    if (m != 0)
      enabledMask = m;
  }
  prefs.end();
  Serial.printf("Display mask: 0x%02X\n", enabledMask);
}

// --- Setup Mode ---
// When the user requests categories whose prereqs aren't satisfied, the display
// drops into MODE_SETUP showing a hint. `setupTargetMask` records what to
// resume into once prereqs are met (or after the inactivity timeout).
volatile unsigned long setupLastActivityMs = 0;
unsigned int setupFrame = 0;
uint8_t setupTargetMask = MASK_ALL;
#define SETUP_TIMEOUT_MS 60000

char bleDeviceName[24];

// --- BLE ---

#define BLE_DEVICE_NAME "LED-Ticker"
#define BLE_SERVICE_UUID "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define BLE_TICKER_CHAR_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define BLE_MODE_CHAR_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a9"
// 26aa was once "Messages". The characteristic is gone; the UUID is left
// here as a tombstone comment so future additions don't reuse it and
// confuse old clients that probe for it.
#define BLE_CMD_CHAR_UUID "beb5483e-36e1-4688-b7f5-ea07361b26ab"
#define BLE_WIFI_CHAR_UUID "beb5483e-36e1-4688-b7f5-ea07361b26ac"
#define BLE_APIKEY_CHAR_UUID "beb5483e-36e1-4688-b7f5-ea07361b26ad"
#define BLE_LOCS_CHAR_UUID "beb5483e-36e1-4688-b7f5-ea07361b26ae"
#define BLE_STATUS_CHAR_UUID "beb5483e-36e1-4688-b7f5-ea07361b26af"
#define BLE_TICKER_BUF_LEN (MAX_STOCKS * (MAX_TICKER_LEN + 1))
#define BLE_WIFI_BUF_LEN (WIFI_SSID_MAX + WIFI_PASS_MAX + 1)
#define BLE_LOCS_BUF_LEN (MAX_LOCATIONS * (MAX_LOCATION_LEN + 1))
// "text|<uint32 seconds>" — text up to STATUS_MAX_LEN, plus '|', up to 10
// digits, plus NUL.
#define BLE_STATUS_BUF_LEN (STATUS_MAX_LEN + 12)

volatile bool tickerUpdatePending = false;
volatile bool modeUpdatePending = false;
volatile bool cmdPending = false;
volatile bool wifiUpdatePending = false;
volatile bool apiKeyUpdatePending = false;
volatile bool locsUpdatePending = false;
volatile bool statusUpdatePending = false;

char pendingTickerStr[BLE_TICKER_BUF_LEN];
// Holds the comma-joined mode payload; size = longest valid mode string,
// "stocks,weather,clock" (20 chars + NUL), plus slack.
char pendingModeStr[64];
char pendingCmd[16];
char pendingWifiStr[BLE_WIFI_BUF_LEN];
char pendingApiKey[MAX_APIKEY_LEN];
char pendingLocsStr[BLE_LOCS_BUF_LEN];
char pendingStatusStr[BLE_STATUS_BUF_LEN];

// Minimum ms between writes that trigger network activity
#define BLE_FETCH_COOLDOWN_MS 10000
volatile unsigned long lastBLEFetchMs = 0;

class TickerCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pChar) override {
    setupLastActivityMs = millis();
    if (millis() - lastBLEFetchMs < BLE_FETCH_COOLDOWN_MS) {
      Serial.println("BLE tickers: cooldown, ignoring");
      return;
    }
    std::string val = pChar->getValue();
    if (val.length() > 0 && val.length() < BLE_TICKER_BUF_LEN) {
      memcpy(pendingTickerStr, val.c_str(), val.length());
      pendingTickerStr[val.length()] = '\0';
      tickerUpdatePending = true;
      lastBLEFetchMs = millis();
    }
  }

  void onRead(NimBLECharacteristic* pChar) override {
    char buf[BLE_TICKER_BUF_LEN];
    int len = 0;
    for (int i = 0; i < nvsTickerCount && len < (int)sizeof(buf) - 1; i++) {
      if (i > 0)
        buf[len++] = ',';
      int remaining = sizeof(buf) - 1 - len;
      int tlen = strnlen(nvsTickers[i], remaining);
      memcpy(buf + len, nvsTickers[i], tlen);
      len += tlen;
    }
    buf[len] = '\0';
    pChar->setValue((uint8_t*)buf, len);
  }
};

static int formatModeName(char* buf, size_t bufLen) {
  if (currentMode == MODE_SETUP)
    return snprintf(buf, bufLen, "setup");
  if (enabledMask == MASK_ALL)
    return snprintf(buf, bufLen, "all");
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

class ModeCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pChar) override {
    setupLastActivityMs = millis();
    std::string val = pChar->getValue();
    if (val.length() > 0 && val.length() < sizeof(pendingModeStr)) {
      memcpy(pendingModeStr, val.c_str(), val.length());
      pendingModeStr[val.length()] = '\0';
      modeUpdatePending = true;
    }
  }

  void onRead(NimBLECharacteristic* pChar) override {
    char buf[64];
    int len = formatModeName(buf, sizeof(buf));
    pChar->setValue((uint8_t*)buf, len);
  }
};

// Status payload formats:
//   Write "text|N"        — set status for N seconds (N=0 = indefinite)
//   Write ""              — clear status
//   Read returns "text|M" — M seconds remaining (0 if indefinite),
//                           or empty string if no active status
class StatusCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pChar) override {
    setupLastActivityMs = millis();
    std::string val = pChar->getValue();
    if (val.length() >= BLE_STATUS_BUF_LEN)
      return;
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
    uint32_t remaining = 0; // 0 means "indefinite" on the read side
    if (statusExpiresAt != 0 && statusExpiresAt != UINT32_MAX) {
      uint32_t now = (uint32_t)time(NULL);
      remaining = (now < statusExpiresAt) ? (statusExpiresAt - now) : 1;
    }
    int len = snprintf(buf, sizeof(buf), "%s|%u", activeStatusText, remaining);
    if (len < 0)
      len = 0;
    if (len >= (int)sizeof(buf))
      len = sizeof(buf) - 1;
    pChar->setValue((uint8_t*)buf, len);
  }
};

class WifiCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pChar) override {
    setupLastActivityMs = millis();
    std::string val = pChar->getValue();
    if (val.length() > 0 && val.length() < BLE_WIFI_BUF_LEN) {
      memcpy(pendingWifiStr, val.c_str(), val.length());
      pendingWifiStr[val.length()] = '\0';
      wifiUpdatePending = true;
    }
  }

  void onRead(NimBLECharacteristic* pChar) override {
    // Return SSID only — never expose the password over BLE
    pChar->setValue((uint8_t*)nvsWifiSsid, strlen(nvsWifiSsid));
  }
};

class ApiKeyCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pChar) override {
    setupLastActivityMs = millis();
    std::string val = pChar->getValue();
    if (val.length() > 0 && val.length() < MAX_APIKEY_LEN) {
      memcpy(pendingApiKey, val.c_str(), val.length());
      pendingApiKey[val.length()] = '\0';
      apiKeyUpdatePending = true;
    }
  }

  void onRead(NimBLECharacteristic* pChar) override {
    pChar->setValue((uint8_t*)nvsApiKey, strlen(nvsApiKey));
  }
};

class LocsCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pChar) override {
    setupLastActivityMs = millis();
    if (millis() - lastBLEFetchMs < BLE_FETCH_COOLDOWN_MS) {
      Serial.println("BLE locations: cooldown, ignoring");
      return;
    }
    std::string val = pChar->getValue();
    if (val.length() > 0 && val.length() < BLE_LOCS_BUF_LEN) {
      memcpy(pendingLocsStr, val.c_str(), val.length());
      pendingLocsStr[val.length()] = '\0';
      locsUpdatePending = true;
      lastBLEFetchMs = millis();
    }
  }

  void onRead(NimBLECharacteristic* pChar) override {
    char buf[BLE_LOCS_BUF_LEN];
    int len = 0;
    for (int i = 0; i < nvsLocationCount && len < (int)sizeof(buf) - 1; i++) {
      if (i > 0 && len < (int)sizeof(buf) - 1)
        buf[len++] = '|';
      int remaining = sizeof(buf) - 1 - len;
      int llen = strnlen(nvsLocations[i], remaining);
      memcpy(buf + len, nvsLocations[i], llen);
      len += llen;
    }
    buf[len] = '\0';
    pChar->setValue((uint8_t*)buf, len);
  }
};

class CmdCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pChar) override {
    setupLastActivityMs = millis();
    std::string val = pChar->getValue();
    if (val.length() == 0 || val.length() >= sizeof(pendingCmd))
      return;

    // reload and reset trigger network activity — apply cooldown
    bool fetchCmd = (val == "reload" || val == "reset");
    if (fetchCmd && millis() - lastBLEFetchMs < BLE_FETCH_COOLDOWN_MS) {
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

// Suffix MAC bytes so multiple units on the same bench are distinguishable.
void buildDeviceName() {
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
class ServerCallbacks : public NimBLEServerCallbacks {
  void onDisconnect(NimBLEServer*) override {
    Serial.println("BLE: client disconnected, resuming advertising");
    NimBLEDevice::startAdvertising();
  }
};

void initBLE() {
  NimBLEDevice::init(bleDeviceName);
  NimBLEDevice::setMTU(512);
  NimBLEServer* pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());
  NimBLEService* pService = pServer->createService(BLE_SERVICE_UUID);

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

  pService->start();
  NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
  pAdv->addServiceUUID(BLE_SERVICE_UUID);
  pAdv->start();
  Serial.println("BLE advertising as " BLE_DEVICE_NAME);
}

static void commitStocks(const StockQuote* tmp, int count) {
  xSemaphoreTake(dataMutex, portMAX_DELAY);
  for (int i = 0; i < count; i++)
    stockQuotes[i] = tmp[i];
  stockCount = count;
  currentStock = 0;
  xSemaphoreGive(dataMutex);
}

// --- Time / Market Hours ---

bool timeReady = false;

void initTime() {
  // Only start SNTP once WiFi is actually up — otherwise lwIP's SNTP client
  // burns retries on DNS lookups that can't possibly succeed, and on
  // pre-IDF-5 builds (Arduino 2.0.14 here) those failures accumulate and
  // eventually wedge the device. See the "fresh-boot freeze after 10s of
  // minutes" symptom.
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Skipping NTP init — WiFi not connected");
    return;
  }

  // Hardcoded to US Pacific. The clock displays in this zone, and
  // isMarketOpen() expects ET — so its hour constants are shifted by -3.
  configTzTime("PST8PDT,M3.2.0,M11.1.0", "pool.ntp.org");

  Serial.print("Syncing NTP");
  for (int i = 0; i < 20; i++) {
    struct tm t;
    if (getLocalTime(&t, 100)) {
      timeReady = true;
      Serial.printf("\nTime: %04d-%02d-%02d %02d:%02d ET\n",
        t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min);
      return;
    }
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nNTP sync failed, will fetch stocks anyway");
}

bool isMarketOpen() {
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

static void fetchStocksImpl(bool force) {
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

  for (int i = 0; i < nvsTickerCount && count < MAX_STOCKS; i++) {
    char url[256];
    snprintf(url, sizeof(url),
      "https://finnhub.io/api/v1/quote?symbol=%s&token=%s",
      nvsTickers[i], nvsApiKey);

    http.begin(url);

    int code = http.GET();
    if (code != 200) {
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
    Serial.printf("Stock: %s $%.2f %+.2f%%\n", tmp[count].symbol, current, change);
    count++;
  }

  if (count > 0) {
    commitStocks(tmp, count);
    Serial.printf("Loaded %d stock quotes\n", count);
  }
}

static void commitWeather(const WeatherReading* tmp, int count) {
  xSemaphoreTake(dataMutex, portMAX_DELAY);
  for (int i = 0; i < count; i++)
    weatherReadings[i] = tmp[i];
  weatherCount = count;
  currentWeather = 0;
  xSemaphoreGive(dataMutex);
}

// URL-encodes spaces and commas — the only chars we expect in location queries.
static void urlEncodeLocation(const char* in, char* out, int outLen) {
  int j = 0;
  for (int i = 0; in[i] && j < outLen - 4; i++) {
    unsigned char c = (unsigned char)in[i];
    if (c == ' ') {
      out[j++] = '%';
      out[j++] = '2';
      out[j++] = '0';
    }
    else if (c == ',') {
      out[j++] = '%';
      out[j++] = '2';
      out[j++] = 'C';
    }
    else {
      out[j++] = c;
    }
  }
  out[j] = '\0';
}

// Resolves a user-entered string ("98052" or "Redmond, WA") to lat/lon.
// If the query contains a trailing ", XX" we use it as an admin1 (state) filter.
static bool geocodeLocation(HTTPClient& http, const char* query, ResolvedLocation& out) {
  char name[MAX_LOCATION_LEN];
  char region[MAX_LOCATION_LEN];
  strncpy(name, query, sizeof(name) - 1);
  name[sizeof(name) - 1] = '\0';
  region[0] = '\0';

  char* comma = strchr(name, ',');
  if (comma) {
    *comma = '\0';
    const char* r = comma + 1;
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
  if (code != 200) {
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
  if (results.isNull() || results.size() == 0) {
    Serial.printf("Geocode: no results for \"%s\"\n", query);
    return false;
  }

  JsonVariant pick;
  if (region[0] != '\0') {
    for (JsonVariant r : results.as<JsonArray>()) {
      const char* admin1 = r["admin1"] | "";
      const char* ccode = r["country_code"] | "";
      if (strcasecmp(admin1, region) == 0 || strcasecmp(ccode, region) == 0) {
        pick = r;
        break;
      }
    }
  }
  if (pick.isNull())
    pick = results[0];

  out.lat = pick["latitude"];
  out.lon = pick["longitude"];
  const char* resolvedName = pick["name"] | name;
  strncpy(out.name, resolvedName, MAX_LOC_NAME_LEN - 1);
  out.name[MAX_LOC_NAME_LEN - 1] = '\0';
  out.ok = true;
  return true;
}

static void fetchWeatherImpl() {
  if (WiFi.status() != WL_CONNECTED || nvsLocationCount == 0)
    return;

  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(5000);
  http.setReuse(true); // keep TLS session across same-host requests

  WeatherReading tmp[MAX_LOCATIONS];
  int count = 0;

  for (int i = 0; i < nvsLocationCount && count < MAX_LOCATIONS; i++) {
    if (!resolved[i].ok) {
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
    if (code != 200) {
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
    Serial.printf("Weather: %s %.0fF\n",
      tmp[count].name, tmp[count].tempF);
    count++;
  }

  if (count > 0) {
    commitWeather(tmp, count);
    Serial.printf("Loaded %d weather entries\n", count);
  }
}

static void fetchTask(void*) {
  while (true) {
    uint32_t forceVal;
    xTaskNotifyWait(0, 0, &forceVal, portMAX_DELAY);
    fetching = true;
    fetchStocksImpl((bool)forceVal);
    fetchWeatherImpl();
    fetching = false;
  }
}

void triggerFetch(bool force = false) {
  xTaskNotify(fetchTaskHandle, (uint32_t)force, eSetValueWithOverwrite);
}

// --- Display Rotation ---

int currentMode = MODE_CONTENT;
void enterContent();
void enterSetup(uint8_t targetMask);


void showNextStock() {
  StockQuote q;
  xSemaphoreTake(dataMutex, portMAX_DELAY);
  q = stockQuotes[currentStock];
  currentStock = (currentStock + 1) % stockCount;
  xSemaphoreGive(dataMutex);

  const char* arrow = q.changePct >= 0 ? "\x18" : "\x19";
  snprintf(scrollBuf, sizeof(scrollBuf),
    "%s $%.2f %s", q.symbol, q.price, arrow);
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
void tickStaticClock() {
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

// Which category bit is currently scrolling within MODE_CONTENT. Always one
// of BIT_STOCKS / BIT_WEATHER / BIT_CLOCK. Advances when the active category
// wraps so each enabled category gets a full pass per cycle.
uint8_t currentBit = BIT_STOCKS;

static bool stocksAvailable() {
  return wifiConfigured() && apiKeyConfigured() && stockCount > 0;
}

static bool weatherAvailable() {
  return wifiConfigured() && weatherCount > 0;
}

extern bool timeReady;

static bool bitHasData(uint8_t b) {
  if (b == BIT_STOCKS)
    return stocksAvailable();
  if (b == BIT_WEATHER)
    return weatherAvailable();
  if (b == BIT_CLOCK)
    return timeReady; // NTP must have synced at least once
  return false;
}

static uint8_t nextBit(uint8_t b) {
  if (b == BIT_STOCKS)
    return BIT_WEATHER;
  if (b == BIT_WEATHER)
    return BIT_CLOCK;
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
  const uint8_t order[] = { BIT_STOCKS, BIT_WEATHER, BIT_CLOCK };
  for (uint8_t b : order)
    if ((enabledMask & b) && bitHasData(b))
      return b;
  for (uint8_t b : order)
    if (enabledMask & b)
      return b;
  return BIT_CLOCK;
}

void enterContent() {
  currentMode = MODE_CONTENT;
  currentStock = 0;
  currentWeather = 0;
  currentBit = firstActiveBit();
}

static bool maskPrereqsReady(uint8_t mask) {
  if ((mask & BIT_STOCKS) && (!wifiConfigured() || !apiKeyConfigured()))
    return false;
  if ((mask & BIT_WEATHER) && !wifiConfigured())
    return false;
  if ((mask & BIT_CLOCK) && !wifiConfigured())
    return false;
  return true;
}

// --- Setup Mode ---

void enterSetup(uint8_t targetMask) {
  currentMode = MODE_SETUP;
  setupTargetMask = targetMask ? targetMask : MASK_ALL;
  setupLastActivityMs = millis();
  setupFrame = 0;
}

void exitSetupIfReady() {
  if (currentMode != MODE_SETUP)
    return;
  if (!maskPrereqsReady(setupTargetMask))
    return;
  enabledMask = setupTargetMask;
  saveDisplayMaskToNVS();
  enterContent();
  char buf[64];
  formatModeName(buf, sizeof(buf));
  Serial.printf("Setup: prereqs satisfied, exiting to %s\n", buf);
}

void showNextSetup() {
  // Hint targets the first unsatisfied prereq in setupTargetMask.
  const char* hint;
  if (!wifiConfigured())
    hint = "Configure WiFi over BLE";
  else if ((setupTargetMask & BIT_STOCKS) && !apiKeyConfigured())
    hint = "Set Finnhub key";
  else if ((setupTargetMask & BIT_WEATHER))
    hint = "Weather needs WiFi";
  else
    hint = "Configure WiFi over BLE";
  scrollText((setupFrame++ % 2 == 0) ? hint : bleDeviceName);
}

void showNext() {
  if (currentMode == MODE_SETUP) {
    showNextSetup();
    return;
  }

  // Defensive: an out-of-mask currentBit can occur after a mask change.
  if (!(enabledMask & currentBit))
    currentBit = firstActiveBit();

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
    if (currentStock == 0)
      advanceCategory();
  }
  else if (currentBit == BIT_WEATHER) {
    showNextWeather();
    if (currentWeather == 0)
      advanceCategory();
  }
  else if (currentBit == BIT_CLOCK) {
    showNextClock();
    advanceCategory(); // one item per pass — always rotate after showing
  }
}

// --- Active Status Rendering ---
// Short text (≤ STATUS_STATIC_MAX_CHARS) renders steady — the text being lit
// is the signal; a hard blink reads as "alarm" rather than "state." Longer
// text scrolls on a loop. `statusShown` caches what's currently on the
// matrix so tickActiveStatus skips redraws when nothing changed; writers
// that wipe the display out-of-band call invalidateStatusRender() so the
// next tick repaints rather than no-oping on a stale equality.
static char statusShown[STATUS_MAX_LEN] = "";
static bool statusShownIsScroll = false;

static void invalidateStatusRender() {
  statusShown[0] = '\0';
}

// Combined gate + expiry check. Returns true if status should render right
// now; if a timed status has passed its expiry, clears and resumes ambient
// in-place (so the caller just falls through to the normal render path).
// One time(NULL) per call instead of the two we'd pay if expiry and the
// render gate were separate functions.
bool checkStatusForRender() {
  if (activeStatusText[0] == '\0')
    return false;
  if (statusExpiresAt == 0 || statusExpiresAt == UINT32_MAX)
    return true;
  // Timed status: can't trust expiry until NTP has set the clock.
  if (!timeReady)
    return false;
  if ((uint32_t)time(NULL) < statusExpiresAt)
    return true;

  Serial.printf("Status: \"%s\" expired, clearing\n", activeStatusText);
  clearStatus();
  invalidateStatusRender();
  display.displayClear();
  enterContent();
  return false;
}

static void clearActiveStatusAndResume() {
  clearStatus();
  invalidateStatusRender();
  display.displayClear();
  enterContent();
}

void tickActiveStatus() {
  if (strcmp(statusShown, activeStatusText) != 0) {
    strncpy(statusShown, activeStatusText, sizeof(statusShown) - 1);
    statusShown[sizeof(statusShown) - 1] = '\0';
    statusShownIsScroll = strlen(activeStatusText) > STATUS_STATIC_MAX_CHARS;
    display.displayClear();
    if (statusShownIsScroll) {
      strncpy(scrollBuf, activeStatusText, sizeof(scrollBuf) - 1);
      scrollBuf[sizeof(scrollBuf) - 1] = '\0';
      display.displayScroll(scrollBuf, PA_LEFT, PA_SCROLL_LEFT, SCROLL_SPEED);
    }
    else {
      display.displayText(activeStatusText, PA_CENTER, 0, 0, PA_PRINT, PA_NO_EFFECT);
      display.displayAnimate();
    }
  }

  // Only the scroll path needs the per-tick animation pump.
  if (statusShownIsScroll && display.displayAnimate())
    display.displayReset(); // loop the same scroll
}

// --- WiFi ---

void connectWifi() {
  if (!wifiConfigured() || WiFi.status() == WL_CONNECTED)
    return;

  Serial.printf("Connecting to %s", nvsWifiSsid);
  WiFi.begin(nvsWifiSsid, nvsWifiPass);

  for (int attempts = 0; WiFi.status() != WL_CONNECTED && attempts < 20; attempts++) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    IPAddress ip = WiFi.localIP();
    Serial.printf("\nConnected, IP: %u.%u.%u.%u\n", ip[0], ip[1], ip[2], ip[3]);
  }
  else {
    Serial.println("\nWiFi failed");
  }
}

// --- BLE Apply ---

void applyPendingApiKey() {
  apiKeyUpdatePending = false;
  strncpy(nvsApiKey, pendingApiKey, MAX_APIKEY_LEN - 1);
  nvsApiKey[MAX_APIKEY_LEN - 1] = '\0';
  saveApiKeyToNVS();
  Serial.println("BLE apikey: saved, fetching stocks");
  triggerFetch(true);
  exitSetupIfReady();
}

void applyPendingWifi() {
  wifiUpdatePending = false;

  // Split on first '|' — password may contain '|'
  char* sep = strchr(pendingWifiStr, '|');
  if (!sep) {
    Serial.println("BLE wifi: missing '|' separator, ignoring");
    return;
  }

  *sep = '\0';
  const char* ssid = pendingWifiStr;
  const char* pass = sep + 1;

  if (strlen(ssid) == 0 || strlen(ssid) >= WIFI_SSID_MAX) {
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

void applyPendingCmd() {
  cmdPending = false;

  if (strcmp(pendingCmd, "reload") == 0) {
    Serial.println("BLE cmd: reloading stocks");
    triggerFetch(true);
  }
  else if (strcmp(pendingCmd, "reset") == 0) {
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
  else {
    Serial.printf("BLE cmd: unknown command \"%s\"\n", pendingCmd);
  }
}

// Accepts "all" or a comma-separated subset of {stocks, weather, clock}.
// Returns 0 on unknown token, empty input, or empty mask after parse.
static uint8_t parseModePayload(const char* in) {
  if (strcmp(in, "all") == 0)
    return MASK_ALL;

  char buf[64];
  strncpy(buf, in, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';

  uint8_t mask = 0;
  char* tok = strtok(buf, ",");
  while (tok) {
    while (*tok == ' ' || *tok == '\t')
      tok++;
    char* end = tok + strlen(tok);
    while (end > tok && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' || end[-1] == '\r'))
      *--end = '\0';

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
    Serial.printf("BLE: unknown/empty mode \"%s\", ignoring\n", pendingModeStr);
    return;
  }
  enabledMask = mask;
  saveDisplayMaskToNVS();
  if (!maskPrereqsReady(mask))
    enterSetup(mask);
  else
    enterContent();
  char buf[64];
  formatModeName(buf, sizeof(buf));
  Serial.printf("BLE: mode -> %s\n", buf);
}

void applyPendingStatus() {
  char buf[BLE_STATUS_BUF_LEN];
  strncpy(buf, pendingStatusStr, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';
  statusUpdatePending = false;

  if (buf[0] == '\0') {
    if (activeStatusText[0])
      Serial.println("BLE status: cleared");
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

  while (*text == ' ')
    text++;
  int textLen = strlen(text);
  while (textLen > 0 && text[textLen - 1] == ' ')
    text[--textLen] = '\0';
  if (textLen == 0) {
    if (activeStatusText[0])
      Serial.println("BLE status: empty text, clearing");
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
    Serial.printf("BLE status: \"%s\" indefinite\n", activeStatusText);
  }
  else if (!timeReady) {
    // No clock yet — store as indefinite so we don't accidentally expire on
    // an uninitialized clock. User can re-send once NTP is up if they want
    // a timed sign during pre-NTP windows.
    statusExpiresAt = UINT32_MAX;
    Serial.printf("BLE status: \"%s\" (no NTP — treating as indefinite)\n",
                  activeStatusText);
  }
  else {
    statusExpiresAt = (uint32_t)time(NULL) + secs;
    Serial.printf("BLE status: \"%s\" for %lus (exp=%u)\n",
                  activeStatusText, secs, statusExpiresAt);
  }
  invalidateStatusRender();
  display.displayClear();
}

void applyPendingTickers() {
  char buf[BLE_TICKER_BUF_LEN];
  strncpy(buf, pendingTickerStr, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';
  tickerUpdatePending = false;

  char tmp[MAX_STOCKS][MAX_TICKER_LEN];
  int count = 0;

  char* token = strtok(buf, ",");
  while (token && count < MAX_STOCKS) {
    while (*token == ' ')
      token++;
    int len = strlen(token);
    while (len > 0 && token[len - 1] == ' ')
      len--;
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

void applyPendingLocations() {
  char buf[BLE_LOCS_BUF_LEN];
  strncpy(buf, pendingLocsStr, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';
  locsUpdatePending = false;

  char tmp[MAX_LOCATIONS][MAX_LOCATION_LEN];
  int count = 0;

  char* token = strtok(buf, "|");
  while (token && count < MAX_LOCATIONS) {
    while (*token == ' ')
      token++;
    int len = strlen(token);
    while (len > 0 && token[len - 1] == ' ')
      len--;
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
  for (int i = 0; i < MAX_LOCATIONS; i++)
    resolved[i].ok = false;
  saveLocationsToNVS();

  triggerFetch(true);
}

// --- Main ---

unsigned long lastFetch = 0;

void setup() {
  Serial.begin(115200);
  delay(500);

  dataMutex = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(fetchTask, "fetchStocks", FETCH_TASK_STACK, nullptr, 1, &fetchTaskHandle, 0);

  initDisplay();
  loadWifiFromNVS();
  loadApiKeyFromNVS();
  loadTickersFromNVS();
  loadLocationsFromNVS();
  loadDisplayMaskFromNVS();
  buildDeviceName();
  if (!maskPrereqsReady(enabledMask))
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

void loop() {
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

  updateStatusLed();

  if (currentMode == MODE_SETUP &&
      millis() - setupLastActivityMs > SETUP_TIMEOUT_MS) {
    if (wifiConfigured()) {
      Serial.println("Setup: 60s no activity, falling to content (mask unchanged)");
      enterContent();
    }
    else {
      // No WiFi means every category in the mask is dead — falling through
      // would just rotate "Loading X..." hints forever. The setup hint
      // ("Configure WiFi over BLE") is strictly better. Push the next
      // timeout check out so we don't re-evaluate this branch every loop.
      setupLastActivityMs = millis();
    }
  }

  if (checkStatusForRender()) {
    tickActiveStatus();
  }
  else if (currentMode == MODE_CONTENT && enabledMask == BIT_CLOCK && timeReady) {
    tickStaticClock();
  }
  else if (display.displayAnimate()) {
    display.displayReset();
    showNext();
  }

  if (millis() - lastFetch > FETCH_INTERVAL_MS) {
    lastFetch = millis();
    triggerFetch();
  }
}
