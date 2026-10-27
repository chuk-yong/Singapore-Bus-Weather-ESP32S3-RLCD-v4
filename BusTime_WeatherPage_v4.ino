// ============================================================
//  BusTime_WeatherPage_v4
//  Waveshare ESP32-S3-RLCD-4.2 - Singapore bus arrivals + weather
//
//  POWER MODEL - three internal modes, two states the user sees:
//
//    WAIT       deep sleep, panel holds "Press button to connect".
//               Wakes on GPIO18 (KEY) or at the next 07:00.
//    SCHEDULED  awake 07:00-10:00, refresh every 60 s.
//    SESSION    same screen as SCHEDULED, but started by a button
//               press and ends SESSION_MINUTES later.
//
//  SCHEDULED and SESSION share one render path and one refresh loop;
//  they differ only in how they decide to stop.  WAIT is the only
//  place that calls esp_deep_sleep_start().
//
//  While awake, automatic light sleep (esp_pm_configure) lets the CPU
//  nap between 60 s ticks with WiFi still associated, which is most of
//  the saving: the 3 h morning window is ~10x the cost of the 21 h
//  wait, so optimising the wait alone would be pointless.
//
//  Panel notes: 0x38 = High Power Mode (fast, clean update),
//  0x39 = Low Power Mode (~8 Hz) which holds the image at minimal
//  current.  Every frame is pushed high-power then parked low-power,
//  and the SPI pins are latched before deep sleep so noise cannot
//  corrupt the retained image.
// ============================================================

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <sys/time.h>
#include <U8g2lib.h>
#include <SPI.h>
#include <Wire.h>
#include <math.h>
#include <esp_sleep.h>
#include <esp_pm.h>
#include <esp_idf_version.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>

#include "config.h"
#include "secrets.h"
#include "shtc3.h"

// ------------------------------------------------------------
//  Display
// ------------------------------------------------------------
U8G2_ST7305_300X400_F_4W_HW_SPI u8g2(U8G2_R1, RLCD_CS, RLCD_DC, RLCD_RST);

// ------------------------------------------------------------
//  State that survives deep sleep
// ------------------------------------------------------------
RTC_DATA_ATTR uint32_t rtcSessionEnd  = 0;   // epoch; 0 = no session
RTC_DATA_ATTR uint32_t rtcLastUpdate  = 0;   // epoch of last good draw
RTC_DATA_ATTR uint32_t rtcBoots       = 0;

// ------------------------------------------------------------
//  Live state
// ------------------------------------------------------------
Mode g_mode = MODE_WAIT;
const char* g_waitNote = nullptr;
bool g_everFetched = false;
bool g_errorShown = false;
bool g_waitDrawn = false;
uint32_t g_lastFetchMs = 0;

BusService buses[NUM_STOPS][NUM_TARGET_BUSES];
WeatherData weather;
bool g_sensorPresent = false;

WiFiClientSecure g_ltaTls;
WiFiClientSecure g_govTls;

// ============================================================
//  Time helpers
// ============================================================

// Epoch of the moment this firmware was compiled. Used as the
// floor for "is the clock plausible?" - anything before it means
// the RTC never synced (fresh flash, or the 18650 was swapped).
static time_t buildEpoch() {
  struct tm t = {};
  strptime(__DATE__ " " __TIME__, "%b %d %Y %H:%M:%S", &t);
  t.tm_isdst = -1;
  return mktime(&t);
}

bool timeSane() {
  static time_t minValid = 0;
  if (minValid == 0) minValid = buildEpoch() - 2 * 86400;
  return time(nullptr) >= minValid;
}

bool inScheduledWindow() {
  if (!timeSane()) return false;
  time_t now = time(nullptr);
  struct tm ti;
  localtime_r(&now, &ti);
  return ti.tm_hour >= SCHEDULE_START_HOUR && ti.tm_hour < SCHEDULE_END_HOUR;
}

bool sessionActive() {
  return rtcSessionEnd != 0 && (uint32_t)time(nullptr) < rtcSessionEnd;
}

Mode decideMode() {
  if (inScheduledWindow()) return MODE_SCHEDULED;   // wins over a session
  if (sessionActive()) return MODE_SESSION;
  return MODE_WAIT;
}

// When does the next 07:00 happen? 0 if the clock is unusable.
time_t nextWindowStart() {
  if (!timeSane()) return 0;
  time_t now = time(nullptr);
  struct tm ti;
  localtime_r(&now, &ti);
  ti.tm_hour = SCHEDULE_START_HOUR;
  ti.tm_min = 0;
  ti.tm_sec = 0;
  ti.tm_isdst = -1;
  time_t cand = mktime(&ti);
  if (cand <= now) {
    ti.tm_mday += 1;
    ti.tm_isdst = -1;
    cand = mktime(&ti);
  }
  return cand;
}

const char* modeName(Mode m) {
  switch (m) {
    case MODE_SCHEDULED: return "SCHEDULED";
    case MODE_SESSION:   return "SESSION";
    default:             return "WAIT";
  }
}

// ============================================================
//  WiFi / NTP
// ============================================================
void wifiOff() {
#if POWER_SAVE
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
#endif
}

bool wifiConnect(uint32_t timeoutMs) {
  if (WiFi.status() == WL_CONNECTED) return true;

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(WIFI_PS_MIN_MODEM);
  // 8.5 dBm is plenty indoors and lowers the TX current spike, which
  // is what browns out a near-empty cell behind a 3.3 V LDO.
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeoutMs) {
    delay(100);
  }
  bool ok = (WiFi.status() == WL_CONNECTED);
  LOG("[wifi] %s in %lu ms  ip=%s  rssi=%d\n", ok ? "connected" : "FAILED",
      (unsigned long)(millis() - start),
      WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
  return ok;
}

bool wifiEnsure() {
  if (WiFi.status() == WL_CONNECTED) return true;
  return wifiConnect(WIFI_CONNECT_MS);
}

bool ensureTime() {
  configTime(0, 0, NTP_SERVER);
  // configTime() may rewrite TZ depending on core version; re-assert it
  // so the 07:00 boundary is always local Singapore time.
  setenv("TZ", TZ_STRING, 1);
  tzset();

  uint32_t start = millis();
  while (!timeSane() && (millis() - start) < NTP_WAIT_MS) {
    delay(200);
  }
  LOG("[ntp] %s\n", timeSane() ? "synced" : "FAILED");
  return timeSane();
}

// ============================================================
//  HTTP
// ============================================================
bool httpGet(WiFiClientSecure& tls, const char* url, const char* hdrName,
             const char* hdrValue, String& out) {
  for (uint8_t attempt = 0; attempt < 2; attempt++) {
    HTTPClient http;
    if (!http.begin(tls, url)) {
      LOG("[http] begin failed: %s\n", url);
      return false;
    }
#if USE_HTTP_KEEPALIVE
    http.setReuse(true);
#endif
    if (hdrName != nullptr) http.addHeader(hdrName, hdrValue);
    http.addHeader("accept", "application/json");
    http.setTimeout(HTTP_TIMEOUT_MS);

    int code = http.GET();
    if (code == HTTP_CODE_OK) {
      out = http.getString();
      http.end();
      return true;
    }
    LOG("[http] %d from %s\n", code, url);
    http.end();
    tls.stop();          // stale keep-alive socket: start clean next try
    delay(100);
  }
  return false;
}

// ============================================================
//  Parsing
// ============================================================
// LTA sends "2026-10-26T07:35:00+08:00" (local). mktime interprets
// the broken-down time in TZ, which is exactly right here.
int calculateETA(const char* iso) {
  if (iso == nullptr || strlen(iso) < 19) return -1;
  if (!timeSane()) return -1;

  struct tm arr = {};
  if (strptime(iso, "%Y-%m-%dT%H:%M:%S", &arr) == nullptr) return -1;
  arr.tm_isdst = -1;

  time_t arrival = mktime(&arr);
  long diff = (long)(arrival - time(nullptr));
  if (diff < -60) return -1;              // already gone
  int mins = (int)((diff + 30) / 60);     // round to nearest minute
  return mins < 0 ? 0 : mins;
}

bool fetchBusData(int stopIdx) {
  BusService* arr = buses[stopIdx];
  for (int i = 0; i < NUM_TARGET_BUSES; i++) {
    arr[i].serviceNo = TARGET_BUSES[i];
    for (int k = 0; k < 3; k++) arr[i].eta[k] = -1;
  }

  char url[192];
  snprintf(url, sizeof(url), "%s?BusStopCode=%s", LTA_BASE_URL, STOP_CODES[stopIdx]);

  String payload;
  if (!httpGet(g_ltaTls, url, "AccountKey", LTA_API_KEY, payload)) return false;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    LOG("[lta] json error: %s\n", err.c_str());
    return false;
  }

  JsonArray services = doc["Services"].as<JsonArray>();
  if (services.isNull() && !doc["value"].isNull()) {
    services = doc["value"][0]["Services"].as<JsonArray>();   // datamall v2 shape
  }
  if (services.isNull()) {
    LOG("[lta] no Services array (stop %s)\n", STOP_CODES[stopIdx]);
    return false;
  }

  for (JsonObject svc : services) {
    const char* no = svc["ServiceNo"].as<const char*>();
    if (no == nullptr) continue;
    for (int t = 0; t < NUM_TARGET_BUSES; t++) {
      if (strcmp(no, TARGET_BUSES[t]) != 0) continue;
      arr[t].eta[0] = calculateETA(svc["NextBus"]["EstimatedArrival"].as<const char*>());
      arr[t].eta[1] = calculateETA(svc["NextBus2"]["EstimatedArrival"].as<const char*>());
      arr[t].eta[2] = calculateETA(svc["NextBus3"]["EstimatedArrival"].as<const char*>());
      break;
    }
  }
  return true;
}

bool fetchForecast() {
  String payload;
  if (!httpGet(g_govTls, FORECAST_URL, "x-api-key", DATA_GOV_SG_API_KEY, payload)) {
    return false;
  }

  JsonDocument doc;
  if (deserializeJson(doc, payload)) return false;

  JsonArray items = doc["data"]["items"].as<JsonArray>();
  if (items.isNull() || items.size() == 0) return false;

  // Bind the array to a named variable first: iterating a temporary
  // proxy binds to an object that is destroyed at the end of the
  // statement (ArduinoJson -Wdangling-reference).
  JsonArray forecasts = items[0]["forecasts"].as<JsonArray>();
  for (JsonObject f : forecasts) {
    const char* area = f["area"] | "";
    if (strcmp(area, FORECAST_AREA) == 0) {
      weather.forecast = f["forecast"].as<String>();
      LOG("[forecast] %s\n", weather.forecast.c_str());
      return true;
    }
  }
  LOG("[forecast] area '%s' not in response\n", FORECAST_AREA);
  return false;
}

// ============================================================
//  Temperature / humidity
// ============================================================
#if USE_ONBOARD_SENSOR
void readTempHumidity() {
  int16_t tc = 0, hc = 0;
  if (shtc3Read(tc, hc)) {
    weather.tempC    = (int)lroundf(tc / 100.0f);
    weather.humidity = (int)lroundf(hc / 100.0f);
    weather.valid    = true;
    LOG("[shtc3] %.2f C  %.2f %%RH\n", tc / 100.0f, hc / 100.0f);
  } else {
    weather.valid = false;
    LOG("[shtc3] read failed\n");
  }
}
#else
// Recursive hunt for the first plausible number inside whatever shape
// data.gov.sg chose today. Only compiled when the onboard sensor is off.
static bool extractReading(JsonVariant node, int& out, int depth) {
  if (node.isNull() || depth > 6) return false;

  if (node.is<float>()) {
    float f = node.as<float>();
    if (f >= -50.0f && f <= 200.0f) {
      out = (int)lroundf(f);
      return true;
    }
    return false;
  }
  if (node.is<JsonArray>()) {
    for (JsonVariant item : node.as<JsonArray>()) {
      if (extractReading(item, out, depth + 1)) return true;
    }
    return false;
  }
  if (node.is<JsonObject>()) {
    for (JsonPair kv : node.as<JsonObject>()) {
      const char* key = kv.key().c_str();
      if (strcmp(key, "stations") == 0 || strcmp(key, "location") == 0 ||
          strcmp(key, "label_location") == 0 || strcmp(key, "valid_period") == 0) {
        continue;
      }
      if (extractReading(kv.value(), out, depth + 1)) return true;
    }
  }
  return false;
}

static bool fetchApiReading(const char* url, const char* label, int& out) {
  String payload;
  if (!httpGet(g_govTls, url, "x-api-key", DATA_GOV_SG_API_KEY, payload)) return false;
  JsonDocument doc;
  if (deserializeJson(doc, payload)) return false;

  JsonVariant readings = doc["data"]["readings"];
  if (readings.isNull()) readings = doc["data"]["items"][0]["readings"];

  bool ok = false;
  if (!readings.isNull()) ok = extractReading(readings, out, 0);
  if (!ok) ok = extractReading(doc["data"], out, 0);
  LOG("[%s] %s\n", label, ok ? "ok" : "no reading found");
  return ok;
}

void readTempHumidity() {
  bool t = fetchApiReading(AIR_TEMP_URL, "temperature", weather.tempC);
  bool h = fetchApiReading(HUMIDITY_URL, "humidity", weather.humidity);
  weather.valid = t && h;
}
#endif

void fetchAllData() {
  bool s1 = fetchBusData(0);
  bool s2 = fetchBusData(1);
  bool f  = fetchForecast();
  readTempHumidity();
  LOG("[fetch] stop1=%d stop2=%d forecast=%d weather=%d\n", s1, s2, f, weather.valid);
}

// ============================================================
//  Battery
// ============================================================
uint16_t readBatteryMilliVolts() {
  uint32_t acc = 0;
  for (uint8_t i = 0; i < 8; i++) acc += analogReadMilliVolts(BATT_ADC_PIN);
  return (uint16_t)lroundf((acc / 8.0f) * BATT_DIVIDER);
}

// Li-ion discharge is flat around 3.7 V, so a straight line from
// 2.5 V to 4.2 V would read 90 % and then fall off a cliff.
uint8_t getBatteryPercent() {
  static const uint16_t mv[] = {4200, 4100, 4050, 4000, 3950, 3900, 3850,
                                3800, 3750, 3700, 3650, 3600, 3500, 3400, 3300};
  static const uint8_t  pc[] = { 100,   95,   90,   85,   75,   65,   55,
                                  45,   35,   25,   18,   12,    6,    2,    0};
  const size_t n = sizeof(mv) / sizeof(mv[0]);

  uint16_t v = readBatteryMilliVolts();
  if (v >= mv[0]) return 100;
  for (size_t i = 1; i < n; i++) {
    if (v >= mv[i]) {
      uint16_t span = mv[i - 1] - mv[i];
      uint32_t rise = (uint32_t)(v - mv[i]) * (pc[i - 1] - pc[i]);
      return (uint8_t)(pc[i] + (span ? rise / span : 0));
    }
  }
  return 0;
}

// ============================================================
//  Weather icons (32x32 vector, no bitmaps)
// ============================================================
WeatherIcon classifyWeather(const char* f) {
  if (f == nullptr) return WI_CLOUD;
  if (strstr(f, "Thundery")) return WI_THUNDER;
  if (strstr(f, "Rain") || strstr(f, "Showers")) return WI_RAIN;
  if (strstr(f, "Mist") || strstr(f, "Fog")) return WI_FOG;
  if (strstr(f, "Partly Cloudy")) return WI_PARTLY;
  if (strstr(f, "Cloudy") || strstr(f, "Hazy")) return WI_CLOUD;
  if (strstr(f, "Night")) return WI_MOON;
  if (strstr(f, "Fair") || strstr(f, "Windy")) return WI_SUN;
  return WI_CLOUD;
}

static void iconCloudShape(int x, int y) {
  u8g2.drawDisc(x + 11, y + 17, 6);
  u8g2.drawDisc(x + 21, y + 14, 7);
  u8g2.drawDisc(x + 25, y + 18, 4);
  u8g2.drawBox(x + 9, y + 17, 19, 6);
}

static void iconSun(int x, int y) {
  int cx = x + 16, cy = y + 16;
  u8g2.drawDisc(cx, cy, 6);
  const int dx[8] = {9, 6, 0, -6, -9, -6, 0, 6};
  const int dy[8] = {0, 6, 9, 6, 0, -6, -9, -6};
  const int ex[8] = {14, 10, 0, -10, -14, -10, 0, 10};
  const int ey[8] = {0, 10, 14, 10, 0, -10, -14, -10};
  for (int i = 0; i < 8; i++) {
    u8g2.drawLine(cx + dx[i], cy + dy[i], cx + ex[i], cy + ey[i]);
  }
}

static void iconMoon(int x, int y) {
  u8g2.drawDisc(x + 16, y + 16, 11);
  u8g2.setDrawColor(0);
  u8g2.drawDisc(x + 22, y + 12, 10);
  u8g2.setDrawColor(1);
}

static void iconPartly(int x, int y) {
  u8g2.drawDisc(x + 24, y + 8, 4);
  u8g2.drawLine(x + 24, y + 1, x + 24, y + 2);
  u8g2.drawLine(x + 30, y + 8, x + 31, y + 8);
  u8g2.drawLine(x + 29, y + 3, x + 30, y + 4);
  iconCloudShape(x - 2, y + 8);
}

static void iconRain(int x, int y, boolean storm) {
  iconCloudShape(x, y - 5);
  if (storm) {
    u8g2.drawLine(x + 18, y + 21, x + 13, y + 27);
    u8g2.drawLine(x + 13, y + 27, x + 19, y + 27);
    u8g2.drawLine(x + 19, y + 27, x + 14, y + 31);
  } else {
    for (int i = 0; i < 3; i++) {
      int rx = x + 10 + i * 7;
      u8g2.drawLine(rx, y + 24, rx - 2, y + 29);
    }
  }
}

static void iconFog(int x, int y) {
  iconCloudShape(x, y - 7);
  u8g2.drawLine(x + 7, y + 24, x + 26, y + 24);
  u8g2.drawLine(x + 10, y + 27, x + 29, y + 27);
  u8g2.drawLine(x + 6, y + 30, x + 24, y + 30);
}

void drawWeatherIcon(int x, int y, const char* forecast) {
  u8g2.setDrawColor(1);
  switch (classifyWeather(forecast)) {
    case WI_SUN:     iconSun(x, y);            break;
    case WI_MOON:    iconMoon(x, y);           break;
    case WI_PARTLY:  iconPartly(x, y);         break;
    case WI_RAIN:    iconRain(x, y, false);    break;
    case WI_THUNDER: iconRain(x, y, true);     break;
    case WI_FOG:     iconFog(x, y);            break;
    default:         iconCloudShape(x, y);     break;
  }
}

// ============================================================
//  Rendering
// ============================================================
void pushFrame() {
  u8g2.sendF("c", 0x38);   // High Power Mode: full-speed, clean update
  u8g2.sendBuffer();
  u8g2.sendF("c", 0x39);   // park in Low Power Mode so it holds cheaply
}

void drawBatteryIcon() {
  uint8_t pct = getBatteryPercent();
  char buf[8];
  snprintf(buf, sizeof(buf), "%u%%", (unsigned)pct);
  u8g2.setFont(u8g2_font_7x13_tf);
  int tw = (int)u8g2.getStrWidth(buf);

  int iconX = 370;
  u8g2.drawStr(iconX - 6 - tw, 20, buf);
  u8g2.drawFrame(iconX, 10, 20, 11);
  u8g2.drawBox(iconX + 20, 13, 2, 5);

  int fill = (int)((pct * 16) / 100);
  if (fill > 0) u8g2.drawBox(iconX + 2, 12, fill, 7);
}

void drawHeader() {
  struct tm ti;
  // Timeout 0: never block waiting for a clock that will not arrive.
  if (!getLocalTime(&ti, 0)) {
    drawBatteryIcon();
    return;
  }

  char dateStr[28], timeStr[8];
  strftime(dateStr, sizeof(dateStr), "%a, %d %b %Y", &ti);
  strftime(timeStr, sizeof(timeStr), "%H:%M", &ti);
  for (char* p = dateStr; *p; p++) *p = toupper(*p);

  u8g2.setFont(u8g2_font_7x13B_tf);
  u8g2.drawStr(10, 20, dateStr);

  u8g2.setFont(u8g2_font_logisoso20_tf);
  int tw = (int)u8g2.getStrWidth(timeStr);
  u8g2.drawStr((400 - tw) / 2, 24, timeStr);

  drawBatteryIcon();
}

void drawColumn(int x, int y, const char* label, int stopIdx) {
  u8g2.setDrawColor(1);
  u8g2.drawBox(x, y, 180, 22);
  u8g2.setDrawColor(0);
  u8g2.setFont(u8g2_font_9x15B_tf);
  u8g2.drawStr(x + 5, y + 16, label);
  u8g2.setDrawColor(1);

  int cy = y + 45;
  for (int i = 0; i < NUM_TARGET_BUSES; i++) {
    u8g2.setFont(u8g2_font_fub14_tf);
    u8g2.drawStr(x, cy, buses[stopIdx][i].serviceNo.c_str());

    u8g2.setFont(u8g2_font_fub17_tf);
    for (int k = 0; k < 3; k++) {
      int eta = buses[stopIdx][i].eta[k];
      if (eta < 0) continue;
      char s[12];
      if (eta == 0) {
        strcpy(s, "Arr");
      } else {
        snprintf(s, sizeof(s), "%d", eta);
      }
      u8g2.drawStr(x + 55 + k * 40, cy, s);
    }
    cy += 30;
  }
}

void drawWeather() {
  if (!weather.valid) {
    u8g2.setFont(u8g2_font_7x13_tf);
    u8g2.drawStr(14, 258, "sensor unavailable");
  } else {
    u8g2.setFont(u8g2_font_fub20_tf);
    char buf[12];

    snprintf(buf, sizeof(buf), "%d%%", weather.humidity);
    u8g2.drawStr(14, 262, buf);

    snprintf(buf, sizeof(buf), "%d", weather.tempC);
    int tw = (int)u8g2.getStrWidth(buf);
    int tx = (400 - tw) / 2;
    u8g2.drawStr(tx, 262, buf);

    // Degree sign as a dot: the _tf fonts carry no 0xB0 glyph.
    int dx = tx + tw + 4;
    u8g2.drawDisc(dx + 2, 246, 2);
    u8g2.drawStr(dx + 8, 262, "C");
  }
  drawWeatherIcon(356, 234, weather.forecast.c_str());
}

void drawFooter() {
  char hm[8] = "--:--";
  struct tm ti;
  if (getLocalTime(&ti, 0)) strftime(hm, sizeof(hm), "%H:%M", &ti);

  char buf[64];
  snprintf(buf, sizeof(buf), "Updated %s  |  every %lus  |  %s",
           hm, REFRESH_INTERVAL_MS / 1000UL, modeName(g_mode));
  u8g2.setFont(u8g2_font_5x8_tf);
  u8g2.drawStr(10, 292, buf);
}

void drawBusPage() {
  u8g2.clearBuffer();
  drawHeader();
  drawColumn(10, 40, STOP_NAMES[0], 0);
  drawColumn(210, 40, STOP_NAMES[1], 1);
  u8g2.drawHLine(0, 232, 400);
  drawWeather();
  u8g2.drawHLine(0, 268, 400);
  drawFooter();
}

void drawMessagePage(const char* title, const char* sub) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_fub20_tf);
  u8g2.drawStr((400 - (int)u8g2.getStrWidth(title)) / 2, 132, title);
  if (sub != nullptr) {
    u8g2.setFont(u8g2_font_7x13_tf);
    u8g2.drawStr((400 - (int)u8g2.getStrWidth(sub)) / 2, 162, sub);
  }
  drawBatteryIcon();
}

void drawWaitPage() {
  u8g2.clearBuffer();

  u8g2.setFont(u8g2_font_fub20_tf);
  const char* l1 = "Press button";
  u8g2.drawStr((400 - (int)u8g2.getStrWidth(l1)) / 2, 112, l1);
  const char* l2 = "to connect";
  u8g2.drawStr((400 - (int)u8g2.getStrWidth(l2)) / 2, 142, l2);

  u8g2.setFont(u8g2_font_7x13_tf);
  const char* l3 = "Data refreshes for 20 minutes";
  u8g2.drawStr((400 - (int)u8g2.getStrWidth(l3)) / 2, 176, l3);

  u8g2.drawHLine(0, 200, 400);

  char buf[48];
  if (rtcLastUpdate != 0) {
    time_t t = (time_t)rtcLastUpdate;
    struct tm ti;
    localtime_r(&t, &ti);
    char stamp[20];
    strftime(stamp, sizeof(stamp), "%d %b %H:%M", &ti);
    snprintf(buf, sizeof(buf), "Last update: %s", stamp);
  } else {
    snprintf(buf, sizeof(buf), "Last update: never");
  }
  u8g2.drawStr(14, 226, buf);

  snprintf(buf, sizeof(buf), "Auto window: %02d:00-%02d:00",
           SCHEDULE_START_HOUR, SCHEDULE_END_HOUR);
  u8g2.drawStr(14, 248, buf);

  if (g_waitNote != nullptr) {
    u8g2.setFont(u8g2_font_5x8_tf);
    u8g2.drawStr((400 - (int)u8g2.getStrWidth(g_waitNote)) / 2, 278, g_waitNote);
  }

  drawBatteryIcon();
}

// ============================================================
//  Power management
// ============================================================
void enableLightSleep() {
#if POWER_SAVE
  esp_pm_config_t cfg = {};
  cfg.max_freq_mhz = 80;
  cfg.min_freq_mhz = 40;
  cfg.light_sleep_enable = true;
  esp_err_t err = esp_pm_configure(&cfg);
  LOG("[pm] esp_pm_configure -> %s\n", esp_err_to_name(err));
  if (err != ESP_OK) setCpuFrequencyMhz(80);   // still fine, just no naps
#else
  setCpuFrequencyMhz(240);
#endif
}

void releasePanelPins() {
  gpio_deep_sleep_hold_dis();
  gpio_hold_dis((gpio_num_t)RLCD_CS);
  gpio_hold_dis((gpio_num_t)RLCD_RST);
  gpio_hold_dis((gpio_num_t)RLCD_DC);
  gpio_hold_dis((gpio_num_t)RLCD_SCK);
  gpio_hold_dis((gpio_num_t)RLCD_MOSI);

  pinMode(RLCD_CS, OUTPUT);   digitalWrite(RLCD_CS, HIGH);
  pinMode(RLCD_RST, OUTPUT);  digitalWrite(RLCD_RST, HIGH);
  pinMode(RLCD_DC, OUTPUT);   digitalWrite(RLCD_DC, HIGH);
}

// Latch the SPI lines so they cannot float while the digital domain is
// powered down. A glitching DC/CS line would scribble over the image
// that is supposed to survive the night.
void holdPanelPins() {
  pinMode(RLCD_CS, OUTPUT);    digitalWrite(RLCD_CS, HIGH);
  pinMode(RLCD_RST, OUTPUT);   digitalWrite(RLCD_RST, HIGH);
  pinMode(RLCD_DC, OUTPUT);    digitalWrite(RLCD_DC, LOW);
  pinMode(RLCD_SCK, OUTPUT);   digitalWrite(RLCD_SCK, LOW);
  pinMode(RLCD_MOSI, OUTPUT);  digitalWrite(RLCD_MOSI, LOW);

  gpio_hold_en((gpio_num_t)RLCD_CS);
  gpio_hold_en((gpio_num_t)RLCD_RST);
  gpio_hold_en((gpio_num_t)RLCD_DC);
  gpio_hold_en((gpio_num_t)RLCD_SCK);
  gpio_hold_en((gpio_num_t)RLCD_MOSI);
  gpio_deep_sleep_hold_en();
}

void armWakeSources() {
  // GPIO18 is an RTC GPIO, so it keeps its pull-up while the digital
  // domain is off. Without the RTC pull-up a floating pin produces
  // phantom wakes that quietly flatten the cell.
  const uint64_t mask = 1ULL << KEY_PIN;
  rtc_gpio_pullup_en((gpio_num_t)KEY_PIN);
  rtc_gpio_pulldown_dis((gpio_num_t)KEY_PIN);
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 0)
  esp_sleep_enable_ext1_wakeup_io(mask, ESP_EXT1_WAKEUP_ANY_LOW);
#else
  // ALL_LOW is the pre-5.4 spelling; only one pin is armed so the two
  // modes behave identically here.
  esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ALL_LOW);
#endif

  time_t next = nextWindowStart();
  int64_t us;
  if (next > 0) {
    us = (int64_t)(next - time(nullptr)) * 1000000LL;
    if (us < 1000000LL) us = 1000000LL;
  } else {
    // Clock unusable: retry NTP in 30 minutes.
    us = 30LL * 60LL * 1000000LL;
  }
  esp_sleep_enable_timer_wakeup((uint64_t)us);
  LOG("[sleep] timer %lld min  (session end=%lu)\n", us / 60000000LL,
      (unsigned long)rtcSessionEnd);
}

void enterWait() {
  if (!g_waitDrawn) {
    LOG("[wait] drawing standby page\n");
    drawWaitPage();
    pushFrame();
    g_waitDrawn = true;
  }

#if POWER_SAVE
  wifiOff();
  holdPanelPins();
  armWakeSources();
  Serial.flush();
  delay(50);
  LOG("[wait] deep sleep now\n");
  esp_deep_sleep_start();
  // not reached
#else
  // Bench mode: stay awake so you can watch the log and the clock.
  // The real build wakes from deep sleep on GPIO18; here we poll the same
  // button, so the identical 20-minute session can be exercised without it.
  static bool prevKey = HIGH;
  bool keyNow = digitalRead(KEY_PIN);
  if (prevKey == HIGH && keyNow == LOW) {
    LOG("[bench] KEY (GPIO%d) pressed -> %d min session\n", KEY_PIN, SESSION_MINUTES);
    rtcSessionEnd = (uint32_t)time(nullptr) + (uint32_t)SESSION_MINUTES * 60u;
    g_waitDrawn = false;          // standby page redraws when we come back
  }
  prevKey = keyNow;

  // Heartbeat every 2 s: proves the board is alive, and prints the raw level
  // of both user buttons so you can see which physical key is GPIO18.
  // Press one at a time - the correct one makes key18 go 1 -> 0.
  static uint32_t lastBeat = 0;
  if (millis() - lastBeat >= 2000) {
    lastBeat = millis();
    Serial.printf("[beat] up=%lus mode=%s sane=%d batt=%umV | key18=%d boot0=%d\n",
                  (unsigned long)(millis() / 1000), modeName(g_mode),
                  (int)timeSane(), (unsigned)readBatteryMilliVolts(),
                  (int)keyNow, (int)digitalRead(0));
  }
  delay(100);
#endif
}

// ============================================================
//  Active path
// ============================================================
void doRefresh() {
  if (!g_everFetched) {
    drawMessagePage("Connecting", "Please wait");
    pushFrame();
  }

  if (!wifiEnsure()) {
    LOG("[refresh] no wifi\n");
    // Draw the failure once, not every 60 s: repainting a static message
    // is the only thing an RLCD does inefficiently.
    if (!g_errorShown) {
      drawMessagePage("No WiFi", "Retrying every minute");
      pushFrame();
      g_errorShown = true;
    }
    g_everFetched = true;
    g_lastFetchMs = millis();
    return;
  }
  g_errorShown = false;

  if (!timeSane()) ensureTime();

  fetchAllData();
  drawBusPage();
  pushFrame();

  g_everFetched = true;
  g_lastFetchMs = millis();
  rtcLastUpdate = (uint32_t)time(nullptr);
  LOG("[refresh] drawn, last update=%lu\n", (unsigned long)rtcLastUpdate);
}

// Leave the active path. Re-evaluates instead of blindly going to WAIT:
// a session pressed at 06:50 expires at 07:10, which is *inside* the
// automatic window. Sleeping there would park the device until the
// next morning, so hand over to SCHEDULED instead.
void leaveActive(const char* why) {
  rtcSessionEnd = 0;
  Mode next = decideMode();

  if (next != MODE_WAIT) {
    LOG("[mode] %s -> %s (%s)\n", modeName(g_mode), modeName(next), why);
    g_mode = next;
    return;
  }

  LOG("[mode] %s -> WAIT (%s)\n", modeName(g_mode), why);
  g_mode = MODE_WAIT;
  g_waitNote = why;
  g_waitDrawn = false;
  wifiOff();
}

// ============================================================
//  Setup / loop
// ============================================================
void setup() {
  Serial.begin(115200);
  // USB-CDC drops writes when no host has the port open, so the boot trace
  // is lost if the monitor attaches after boot. Wait briefly for it, then
  // rely on the heartbeat in bench mode. Kept short because on a button
  // wake this delay sits in front of the first paint.
  uint32_t tAttach = millis();
  while (!Serial && (millis() - tAttach) < 800) delay(20);

  LOG("\n##### BusTime v4 starting #####\n");
  LOG("[step] 1: release pin holds\n");
  releasePanelPins();

  setenv("TZ", TZ_STRING, 1);
  tzset();

  LOG("[step] 2: SPI + u8g2.begin()\n");
  SPI.begin(RLCD_SCK, -1, RLCD_MOSI, RLCD_CS);
  u8g2.begin();
  u8g2.setBusClock(24000000);
  LOG("[step] 3: panel init returned\n");

  // The key pin was armed as an RTC input for deep-sleep wake; hand it
  // back to the digital domain before using it as a normal input.
  rtc_gpio_deinit((gpio_num_t)KEY_PIN);
  pinMode(KEY_PIN, INPUT_PULLUP);

  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  // Paint unconditionally, before anything that can block (I2C probe,
  // WiFi, NTP). If this splash does not appear on the panel, the fault
  // is below us in the display path, not in the network code.
  LOG("[step] 4: painting splash\n");
  drawMessagePage("Starting", "BusTime v4");
  pushFrame();
  LOG("[step] 5: splash pushed - panel should read 'Starting'\n");

#if USE_ONBOARD_SENSOR
  Wire.begin(I2C_SDA, I2C_SCL, 100000);
  g_sensorPresent = shtc3Probe();
  LOG("[i2c] SHTC3 %s\n", g_sensorPresent ? "found" : "NOT found");
#endif

  g_ltaTls.setInsecure();
  g_govTls.setInsecure();

  weather.valid    = false;
  weather.tempC    = 0;
  weather.humidity = 0;
  weather.forecast = "No Data";

  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  bool buttonWake = (cause == ESP_SLEEP_WAKEUP_EXT1);
  rtcBoots++;

  // reset=1 poweron, 4 panic, 9 brownout, 8 deep-sleep wake.
  // A rising boot # with reset=9 means the WiFi surge is browning it out.
  LOG("[step] 6: boot #%lu cause=%d (%s) reset=%d time=%ld sane=%d\n",
      (unsigned long)rtcBoots, (int)cause,
      buttonWake ? "button" :
      (cause == ESP_SLEEP_WAKEUP_TIMER ? "timer" : "power-on"),
      (int)esp_reset_reason(), (long)time(nullptr), (int)timeSane());

  if (buttonWake) {
    // A press means "give me 20 minutes of live data".
    rtcSessionEnd = 0;
    LOG("[step] 7: button wake, connecting wifi\n");
    bool ok = wifiConnect(WIFI_CONNECT_MS);
    LOG("[step] 8: wifi=%d\n", (int)ok);
    if (ok) ensureTime();
    if (timeSane()) {
      rtcSessionEnd = (uint32_t)time(nullptr) + (uint32_t)SESSION_MINUTES * 60u;
      LOG("[session] runs until epoch %lu\n", (unsigned long)rtcSessionEnd);
    }
    if (!timeSane()) {
      // Pressed, but we cannot even tell what time it is.
      g_waitNote = "No WiFi or clock - retrying in 30 min";
      g_mode = MODE_WAIT;
      return;
    }
  } else if (!timeSane()) {
    // Timer wake or cold boot with no clock (first boot, cell swapped).
    LOG("[step] 7: no clock, connecting wifi\n");
    bool ok = wifiConnect(WIFI_CONNECT_MS);
    LOG("[step] 8: wifi=%d\n", (int)ok);
    if (ok) ensureTime();
    if (!timeSane()) {
      g_waitNote = "Clock not set - retrying in 30 min";
      g_mode = MODE_WAIT;
      return;
    }
  } else {
    LOG("[step] 7: clock already valid, skipping wifi for now\n");
  }

  g_mode = decideMode();
  LOG("[step] 9: mode=%s\n", modeName(g_mode));

  if (g_mode != MODE_WAIT) {
    enableLightSleep();
    doRefresh();
  }
}

void loop() {
  if (g_mode == MODE_WAIT) {
    // Cheap re-check before sleeping: covers the bench case (POWER_SAVE 0,
    // where enterWait returns) and a clock that crossed 07:00 during boot.
    Mode next = decideMode();
    if (next != MODE_WAIT) {
      LOG("[mode] WAIT -> %s (window opened)\n", modeName(next));
      g_mode = next;
      g_waitDrawn = false;
      g_everFetched = false;    // force a fetch + splash on the new mode
      enableLightSleep();
      return;
    }
    enterWait();          // never returns when POWER_SAVE is on
    return;
  }

  // Exit conditions for the two active modes.
  if (g_mode == MODE_SCHEDULED && !inScheduledWindow()) {
    leaveActive("auto window closed");
    return;
  }
  if (g_mode == MODE_SESSION && !sessionActive()) {
    leaveActive("20 min session over");
    return;
  }

  if (!g_everFetched || (millis() - g_lastFetchMs) >= REFRESH_INTERVAL_MS) {
    doRefresh();          // also stamps g_lastFetchMs
  }

  delay(50);
}
