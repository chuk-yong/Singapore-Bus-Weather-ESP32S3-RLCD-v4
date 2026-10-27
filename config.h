// ============================================================
//  config.h - BusTime_WeatherPage_v4
//  Waveshare ESP32-S3-RLCD-4.2 (ST7305 400x300 reflective LCD)
// ============================================================
#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// ------------------------------------------------------------
//  Feature switches
// ------------------------------------------------------------
// POWER_SAVE 0 = never sleep, never power down the radio. Use it when
//               debugging over USB: deep sleep makes the board
//               re-enumerate and the monitor loses the port.
//               In this mode the KEY button is polled instead of
//               waking the chip, so sessions still work on the bench.
// POWER_SAVE 1 = the real thing: deep sleep in WAIT, GPIO18 wake.
#define POWER_SAVE            1

// Verbose serial logging.
#define DEBUG_LOG             1

// 1 = read temperature/humidity from the onboard SHTC3 (I2C 0x70).
//     Costs one wire transaction and zero HTTPS calls.
// 0 = fall back to data.gov.sg air-temperature + relative-humidity.
//     Costs ~80 extra lines of JSON hunting and two extra TLS handshakes.
#define USE_ONBOARD_SENSOR    1

// Reuse TLS connections between refreshes (5 handshakes -> 2).
// Set to 0 if you ever see empty/failed responses from either API.
#define USE_HTTP_KEEPALIVE    1

// ------------------------------------------------------------
//  Schedule
// ------------------------------------------------------------
// Automatic window, local time, [start, end).  07:00 -> 10:00
#define SCHEDULE_START_HOUR   7
#define SCHEDULE_END_HOUR     10

// Button session length in minutes.
#define SESSION_MINUTES       20

// Bus/weather refresh period while active. LTA republishes once a
// minute, so a lower value only burns radio time for identical data.
#define REFRESH_INTERVAL_MS   60000UL

// ------------------------------------------------------------
//  Pins (Waveshare ESP32-S3-RLCD-4.2)
// ------------------------------------------------------------
#define RLCD_SCK    11
#define RLCD_MOSI   12
#define RLCD_DC      5
#define RLCD_CS     40
#define RLCD_RST    41

#define BATT_ADC_PIN 4
#define BATT_DIVIDER 3.0f      // onboard 3x resistor divider

#define I2C_SDA     13
#define I2C_SCL     14

// Second user key.  Active LOW, pull-up fitted on the board.
// GPIO18 is an RTC GPIO, so it can wake the S3 from deep sleep.
#define KEY_PIN     18

// ------------------------------------------------------------
//  Location / transport
// ------------------------------------------------------------
#define TZ_STRING       "SGT-8"          // POSIX TZ: UTC+8, no DST
#define NTP_SERVER      "pool.ntp.org"
#define FORECAST_AREA   "Hougang"

#define LTA_BASE_URL    "https://datamall2.mytransport.sg/ltaodataservice/v3/BusArrival"
#define FORECAST_URL    "https://api-open.data.gov.sg/v2/real-time/api/two-hr-forecast"
#define AIR_TEMP_URL    "https://api-open.data.gov.sg/v2/real-time/api/air-temperature"
#define HUMIDITY_URL    "https://api-open.data.gov.sg/v2/real-time/api/relative-humidity"

// Two bus stops, side by side on the display.
#define NUM_STOPS        2
static const char* const STOP_CODES[NUM_STOPS] = { "63261", "63269" };
static const char* const STOP_NAMES[NUM_STOPS] = { "Opp Blk 232", "Blk 232" };

// Services to track, in display order.
#define NUM_TARGET_BUSES 5
static const char* const TARGET_BUSES[NUM_TARGET_BUSES] = { "53", "53M", "62", "113", "146" };

// ------------------------------------------------------------
//  Timeouts
// ------------------------------------------------------------
#define WIFI_CONNECT_MS   15000
#define HTTP_TIMEOUT_MS    8000
#define NTP_WAIT_MS        8000

// ------------------------------------------------------------
//  Shared types (kept here so the Arduino prototype generator,
//  which inserts prototypes below the includes, can see them)
// ------------------------------------------------------------
enum Mode : uint8_t {
  MODE_WAIT = 0,      // deep sleep, screen holds "Press button"
  MODE_SCHEDULED = 1, // awake inside 07:00-10:00
  MODE_SESSION = 2    // awake after a button press, for SESSION_MINUTES
};

enum WeatherIcon : uint8_t {
  WI_SUN, WI_MOON, WI_PARTLY, WI_CLOUD, WI_RAIN, WI_THUNDER, WI_FOG
};

struct BusService {
  String serviceNo;
  int eta[3];         // minutes, -1 = unknown
};

struct WeatherData {
  bool valid;         // temp+humidity available
  int tempC;
  int humidity;
  String forecast;
};

// ------------------------------------------------------------
//  Logging
// ------------------------------------------------------------
#if DEBUG_LOG
  #define LOG(...) Serial.printf(__VA_ARGS__)
#else
  #define LOG(...) do {} while (0)
#endif

#endif // CONFIG_H
