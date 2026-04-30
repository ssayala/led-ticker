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
enum {
  MODE_STOCKS,
  MODE_MESSAGES,
  MODE_WEATHER,
  MODE_ALL,
};
// MODE_ALL cycles internally through the three content modes. allSubMode
// is restricted to {MODE_STOCKS, MODE_MESSAGES, MODE_WEATHER}.
#define SUB_MODE_COUNT 3
extern int currentMode;

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

#define MAX_STRING_LEN 96 // scroll buffer + message store cell size
#define MAX_MESSAGES 10
#define MAX_STOCKS 10
#define FETCH_INTERVAL_MS (5 * 60 * 1000)

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

// --- Messages ---

char messageStore[MAX_MESSAGES][MAX_STRING_LEN + 1];
int messageCount = 0;
int currentMsg = 0;

int getTotalMessages() {
  return messageCount > 0 ? messageCount : fallbackCount;
}

const char* getMessage(int idx) {
  if (messageCount > 0)
    return messageStore[idx % messageCount];
  return fallbackMessages[idx % fallbackCount];
}

void saveMessagesToNVS() {
  prefs.begin("msgs", false);
  prefs.putInt("count", messageCount);
  for (int i = 0; i < messageCount; i++) {
    char key[8];
    snprintf(key, sizeof(key), "m%d", i);
    prefs.putString(key, messageStore[i]);
  }
  prefs.end();
}

void loadMessagesFromNVS() {
  prefs.begin("msgs", true);
  int count = prefs.getInt("count", 0);
  if (count > 0 && count <= MAX_MESSAGES) {
    for (int i = 0; i < count; i++) {
      char key[8];
      snprintf(key, sizeof(key), "m%d", i);
      prefs.getString(key, messageStore[i], MAX_STRING_LEN);
    }
    prefs.end();
    messageCount = count;
    Serial.printf("Loaded %d messages from NVS\n", count);
  }
  else {
    prefs.end();
    Serial.println("No messages in NVS, using fallbacks");
  }
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
// Shared across stock/weather/message scrolls — only one is ever active,
// and we overwrite only when starting the next scroll.
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

// --- BLE ---

#define BLE_DEVICE_NAME "LED-Ticker"
#define BLE_SERVICE_UUID "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define BLE_TICKER_CHAR_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define BLE_MODE_CHAR_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a9"
#define BLE_MSGS_CHAR_UUID "beb5483e-36e1-4688-b7f5-ea07361b26aa"
#define BLE_CMD_CHAR_UUID "beb5483e-36e1-4688-b7f5-ea07361b26ab"
#define BLE_WIFI_CHAR_UUID "beb5483e-36e1-4688-b7f5-ea07361b26ac"
#define BLE_APIKEY_CHAR_UUID "beb5483e-36e1-4688-b7f5-ea07361b26ad"
#define BLE_LOCS_CHAR_UUID "beb5483e-36e1-4688-b7f5-ea07361b26ae"
#define BLE_TICKER_BUF_LEN (MAX_STOCKS * (MAX_TICKER_LEN + 1))
#define BLE_MSGS_BUF_LEN 512
#define BLE_WIFI_BUF_LEN (WIFI_SSID_MAX + WIFI_PASS_MAX + 1)
#define BLE_LOCS_BUF_LEN (MAX_LOCATIONS * (MAX_LOCATION_LEN + 1))

volatile bool tickerUpdatePending = false;
volatile bool modeUpdatePending = false;
volatile bool msgsUpdatePending = false;
volatile bool cmdPending = false;
volatile bool wifiUpdatePending = false;
volatile bool apiKeyUpdatePending = false;
volatile bool locsUpdatePending = false;

char pendingTickerStr[BLE_TICKER_BUF_LEN];
char pendingModeStr[16];
char pendingMsgsStr[BLE_MSGS_BUF_LEN];
char pendingCmd[16];
char pendingWifiStr[BLE_WIFI_BUF_LEN];
char pendingApiKey[MAX_APIKEY_LEN];
char pendingLocsStr[BLE_LOCS_BUF_LEN];

// Minimum ms between writes that trigger network activity
#define BLE_FETCH_COOLDOWN_MS 10000
volatile unsigned long lastBLEFetchMs = 0;

class TickerCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pChar) override {
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

static const char* modeName(int m) {
  switch (m) {
  case MODE_STOCKS:
    return "stocks";
  case MODE_MESSAGES:
    return "messages";
  case MODE_WEATHER:
    return "weather";
  case MODE_ALL:
    return "all";
  }
  return "?";
}

class ModeCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pChar) override {
    std::string val = pChar->getValue();
    if (val.length() > 0 && val.length() < sizeof(pendingModeStr)) {
      memcpy(pendingModeStr, val.c_str(), val.length());
      pendingModeStr[val.length()] = '\0';
      modeUpdatePending = true;
    }
  }

  void onRead(NimBLECharacteristic* pChar) override {
    const char* mode = modeName(currentMode);
    pChar->setValue((uint8_t*)mode, strlen(mode));
  }
};

class MsgsCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pChar) override {
    std::string val = pChar->getValue();
    if (val.length() > 0 && val.length() < BLE_MSGS_BUF_LEN) {
      memcpy(pendingMsgsStr, val.c_str(), val.length());
      pendingMsgsStr[val.length()] = '\0';
      msgsUpdatePending = true;
    }
  }

  void onRead(NimBLECharacteristic* pChar) override {
    char buf[BLE_MSGS_BUF_LEN];
    int len = 0;
    int count = messageCount > 0 ? messageCount : fallbackCount;
    for (int i = 0; i < count && len < (int)sizeof(buf) - 1; i++) {
      const char* msg = messageCount > 0 ? messageStore[i] : fallbackMessages[i];
      if (i > 0 && len < (int)sizeof(buf) - 1)
        buf[len++] = '|';
      int remaining = sizeof(buf) - 1 - len;
      int msgLen = strnlen(msg, remaining);
      memcpy(buf + len, msg, msgLen);
      len += msgLen;
    }
    buf[len] = '\0';
    pChar->setValue((uint8_t*)buf, len);
  }
};

class WifiCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pChar) override {
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

void initBLE() {
  // Append the low 2 bytes of the chip MAC so multiple units on the
  // same bench (or in the same household) are distinguishable during
  // first-time setup. The iOS app scans by service UUID so it doesn't
  // care about the name, but humans do.
  uint64_t mac = ESP.getEfuseMac();
  char name[sizeof(BLE_DEVICE_NAME) + 6];
  snprintf(name, sizeof(name), "%s-%02X%02X",
    BLE_DEVICE_NAME,
    (uint8_t)((mac >> 8) & 0xFF),
    (uint8_t)(mac & 0xFF));
  NimBLEDevice::init(name);
  NimBLEDevice::setMTU(512);
  NimBLEServer* pServer = NimBLEDevice::createServer();
  NimBLEService* pService = pServer->createService(BLE_SERVICE_UUID);

  pService->createCharacteristic(BLE_TICKER_CHAR_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE)
    ->setCallbacks(new TickerCallbacks());
  pService->createCharacteristic(BLE_MODE_CHAR_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE)
    ->setCallbacks(new ModeCallbacks());
  pService->createCharacteristic(BLE_MSGS_CHAR_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE)
    ->setCallbacks(new MsgsCallbacks());
  pService->createCharacteristic(BLE_CMD_CHAR_UUID, NIMBLE_PROPERTY::WRITE)
    ->setCallbacks(new CmdCallbacks());
  pService->createCharacteristic(BLE_WIFI_CHAR_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE)
    ->setCallbacks(new WifiCallbacks());
  pService->createCharacteristic(BLE_APIKEY_CHAR_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE)
    ->setCallbacks(new ApiKeyCallbacks());
  pService->createCharacteristic(BLE_LOCS_CHAR_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE)
    ->setCallbacks(new LocsCallbacks());

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
  configTzTime("EST5EDT,M3.2.0,M11.1.0", "pool.ntp.org");

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

  const int MARKET_OPEN = 9 * 60 + 30;
  const int MARKET_CLOSE = 16 * 60;
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

int currentMode = MODE_ALL;
void enterAllMode();


void showNextMsg() {
  int total = getTotalMessages();
  scrollText(getMessage(currentMsg));
  currentMsg = (currentMsg + 1) % total;
}

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

// Sub-mode within MODE_ALL. Advances when the current category wraps so
// that each category shows every item once per cycle.
int allSubMode = MODE_STOCKS;

static bool stocksAvailable() {
  return wifiConfigured() && apiKeyConfigured() && stockCount > 0;
}

static bool weatherAvailable() {
  return wifiConfigured() && weatherCount > 0;
}

static bool subModeHasData(int m) {
  if (m == MODE_STOCKS)
    return stocksAvailable();
  if (m == MODE_WEATHER)
    return weatherAvailable();
  return true; // messages always has fallback text
}

static void advanceAllSubMode() {
  for (int i = 0; i < SUB_MODE_COUNT; i++) {
    allSubMode = (allSubMode + 1) % SUB_MODE_COUNT;
    if (subModeHasData(allSubMode))
      break;
  }
  // Reset the index so the new sub-mode shows its full cycle.
  if (allSubMode == MODE_STOCKS)
    currentStock = 0;
  else if (allSubMode == MODE_WEATHER)
    currentWeather = 0;
  else
    currentMsg = 0;
}

void enterAllMode() {
  allSubMode = MODE_STOCKS;
  currentStock = 0;
  currentMsg = 0;
  currentWeather = 0;
  if (!subModeHasData(allSubMode))
    advanceAllSubMode();
}

void showNext() {
  if (currentMode == MODE_STOCKS) {
    if (!wifiConfigured()) {
      scrollText("Configure WiFi over BLE");
      return;
    }
    if (!apiKeyConfigured()) {
      scrollText("Set Finnhub key over BLE");
      return;
    }
    if (stockCount == 0) {
      scrollText("Loading stocks...");
      return;
    }
    showNextStock();
    return;
  }
  if (currentMode == MODE_WEATHER) {
    if (!wifiConfigured()) {
      scrollText("Configure WiFi over BLE");
      return;
    }
    if (weatherCount == 0) {
      scrollText("Loading weather...");
      return;
    }
    showNextWeather();
    return;
  }
  if (currentMode == MODE_ALL) {
    // Availability can change between frames (e.g. fetch completes, WiFi
    // drops). Skip ahead if the current sub-mode has gone empty.
    if (!subModeHasData(allSubMode))
      advanceAllSubMode();

    if (allSubMode == MODE_STOCKS) {
      showNextStock();
      if (currentStock == 0)
        advanceAllSubMode();
    }
    else if (allSubMode == MODE_WEATHER) {
      showNextWeather();
      if (currentWeather == 0)
        advanceAllSubMode();
    }
    else {
      showNextMsg();
      if (currentMsg == 0)
        advanceAllSubMode();
    }
    return;
  }
  showNextMsg();
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
    Serial.println("\nWiFi failed, using fallback messages");
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
    prefs.begin("msgs", false);
    prefs.clear();
    prefs.end();
    prefs.begin("locs", false);
    prefs.clear();
    prefs.end();

    loadTickersFromNVS();   // re-seeds from config.h since NVS is now empty
    loadLocationsFromNVS(); // same — re-seeds default locations

    messageCount = 0;
    currentMsg = 0;
    stockCount = 0;
    weatherCount = 0;
    currentWeather = 0;
    triggerFetch(true);
  }
  else {
    Serial.printf("BLE cmd: unknown command \"%s\"\n", pendingCmd);
  }
}

void applyPendingMode() {
  modeUpdatePending = false;
  if (strcmp(pendingModeStr, "stocks") == 0)
    currentMode = MODE_STOCKS;
  else if (strcmp(pendingModeStr, "messages") == 0)
    currentMode = MODE_MESSAGES;
  else if (strcmp(pendingModeStr, "weather") == 0)
    currentMode = MODE_WEATHER;
  else if (strcmp(pendingModeStr, "all") == 0) {
    currentMode = MODE_ALL;
    enterAllMode();
  }
  else {
    Serial.printf("BLE: unknown mode \"%s\", ignoring\n", pendingModeStr);
    return;
  }
  Serial.printf("BLE: mode -> %s\n", modeName(currentMode));
}

void applyPendingMessages() {
  char buf[BLE_MSGS_BUF_LEN];
  strncpy(buf, pendingMsgsStr, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';
  msgsUpdatePending = false;

  char tmp[MAX_MESSAGES][MAX_STRING_LEN + 1];
  int count = 0;

  char* token = strtok(buf, "|");
  while (token && count < MAX_MESSAGES) {
    while (*token == ' ')
      token++;
    int len = strlen(token);
    while (len > 0 && token[len - 1] == ' ')
      len--;
    token[len] = '\0';

    if (len > 0) {
      strncpy(tmp[count], token, MAX_STRING_LEN);
      tmp[count][MAX_STRING_LEN] = '\0';
      count++;
    }
    token = strtok(nullptr, "|");
  }

  if (count == 0) {
    Serial.println("BLE: no valid messages, ignoring");
    return;
  }

  for (int i = 0; i < count; i++)
    strncpy(messageStore[i], tmp[i], MAX_STRING_LEN + 1);
  messageCount = count;
  currentMsg = 0;
  saveMessagesToNVS();
  Serial.printf("BLE: applied %d messages\n", count);
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
  loadMessagesFromNVS();
  loadTickersFromNVS();
  loadLocationsFromNVS();
  enterAllMode();
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
  if (msgsUpdatePending)
    applyPendingMessages();
  if (tickerUpdatePending)
    applyPendingTickers();
  if (locsUpdatePending)
    applyPendingLocations();

  updateStatusLed();

  if (display.displayAnimate()) {
    display.displayReset();
    showNext();
  }

  if (millis() - lastFetch > FETCH_INTERVAL_MS) {
    lastFetch = millis();
    triggerFetch();
  }
}
