# Singapore-Bus-Weather-ESP32S3-RLCD-v4
Battery saving features - press to turn on, wake in morning

# BusTime_WeatherPage v4

**Active page** (SCHEDULED / SESSION)

- Header: date, large clock, battery percentage + icon
- Two columns, one per bus stop: service number and the next three arrivals
  (`Arr` when due)
- Weather row: temperature, humidity, forecast icon (32x32 vector, no bitmaps)
- Footer: last update time, refresh period, current mode

**Standby page** (WAIT)

- `Press button to connect` / `Data refreshes for 20 minutes`
- Last update stamp, automatic window, battery icon

## Files

| File                         | Purpose                                                      |
| ---------------------------- | ------------------------------------------------------------ |
| `BusTime_WeatherPage_v4.ino` | Mode logic, WiFi/NTP, fetching, rendering, power management  |
| `config.h`                   | Pins, schedule, timings, feature switches                    |
| `secrets.h`                  | WiFi credentials + API keys — keep out of version control    |
| `shtc3.h`                    | ~50-line SHTC3 driver on raw Wire (avoids pulling in SensorLib) |

## The three modes

```
                07:00 timer
      ┌──────────────────────────────┐
      │                              ▼
   ┌──────┐                      ┌───────────┐
   │ WAIT │                      │ SCHEDULED │──10:00──┐
   └──────┘                      └───────────┘         │
      │   ▲                            ▲               │
 btn  │   │ sessionEnd                 │ (press is     │
      ▼   │ reached                    │  ignored)     │
   ┌─────────┐                         │               │
   │ SESSION │─────────────────────────┘               │
   └─────────┘                     (if 07:00-10:00)    │
      └────────────────────────────────────────────────┘
```

| Mode          | Screen                          | Radio      | CPU                   |
| ------------- | ------------------------------- | ---------- | --------------------- |
| **WAIT**      | standby page                    | off        | deep sleep            |
| **SCHEDULED** | active page, refresh every 60 s | associated | automatic light sleep |
| **SESSION**   | same as SCHEDULED               | associated | automatic light sleep |

Only `WAIT` calls `esp_deep_sleep_start()`. `SCHEDULED` and `SESSION` share one
render path and one refresh loop; they differ only in the exit test — a clock
boundary versus a stored timestamp. That is the whole reason there are three
modes and not two.

**Wake sources in WAIT:** GPIO18 (`KEY`, active LOW level, RTC pull-up enabled)
and a timer for the next 07:00. If the clock is unusable the timer falls back to
30 minutes so the device keeps retrying NTP.

## Timings

| Thing                       | Value                        | Where                                       |
| --------------------------- | ---------------------------- | ------------------------------------------- |
| Automatic window            | 07:00–10:00 local, daily     | `SCHEDULE_START_HOUR` / `SCHEDULE_END_HOUR` |
| Button session              | 20 minutes                   | `SESSION_MINUTES`                           |
| Refresh period while active | 60 s                         | `REFRESH_INTERVAL_MS`                       |
| Wake timer in WAIT          | next 07:00 (30 min fallback) | `armWakeSources()`                          |
| WiFi join timeout           | 15 s                         | `WIFI_CONNECT_MS`                           |
| HTTP timeout                | 8 s                          | `HTTP_TIMEOUT_MS`                           |
| NTP wait                    | 8 s                          | `NTP_WAIT_MS`                               |

60 s is deliberate: LTA republishes arrival estimates once a minute, so a faster
loop spends radio time for identical data.


## Power budget

Rough figures for a 3000 mAh 18650 — measure yours rather than trusting these:

| Phase                                  | Duration | Draw        | Daily cost |
| -------------------------------------- | -------- | ----------- | ---------- |
| WAIT (deep sleep)                      | ~21 h    | ~0.15 mA    | ~3 mAh     |
| SCHEDULED (light sleep + 60 s fetches) | 3 h      | 8–15 mA avg | ~70–90 mAh |
| One button session                     | 20 min   | ~25 mA avg  | ~10 mAh    |

So roughly **100–150 mAh/day**, or about **3–4 weeks per charge**, with the
20-minute sessions being nearly free compared with the morning window. If you
ever want to double that, the next step is deep-sleeping *between* refreshes in
the active window (each refresh becomes a fresh boot, so it needs static IP and
a skipped panel re-init to avoid a white flash every minute).

