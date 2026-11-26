// ============================================================================
//  countdown.h - NTP clock handling and the days-remaining countdown
//
//  Days remaining = ceil((event_00:00_local - now) / 24h), minimum 0.
//    2 days 3 h left -> 3     1 day 4 h left -> 2
//    < 24 h left     -> 1     event date reached or passed -> 0
//  The event "date" is local midnight at the START of that day, so on the
//  event day itself the display shows 00.
//  The display is limited to 99, but the API reports the true number.
// ============================================================================
#pragma once

#include <Arduino.h>
#include <time.h>

#include "config.h"
#include "storage.h"

namespace Clock {

static uint32_t lastNtpMs = 0;
static uint16_t ntpAttempts = 0;

inline bool synced() { return (uint32_t)time(nullptr) >= MIN_VALID_EPOCH; }

inline void applyTimezone(const char* tz) {
  setenv("TZ", tz, 1);
  tzset();
}

// (Re)starts SNTP and applies the timezone. Safe to call repeatedly.
inline void startNtp(uint32_t now) {
  configTzTime(settings.tz, NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3);
  lastNtpMs = now;
  if (ntpAttempts < 65535) ntpAttempts++;
}

// Retries SNTP periodically while the clock is still unsynchronised.
inline void update(uint32_t now, bool wifiUp) {
  if (!wifiUp || synced()) return;
  if ((uint32_t)(now - lastNtpMs) >= NTP_RETRY_MS) startNtp(now);
}

inline bool localTimeString(char* out, size_t cap) {
  if (!synced()) {
    if (cap) out[0] = 0;
    return false;
  }
  time_t t = time(nullptr);
  struct tm tmv;
  localtime_r(&t, &tmv);
  strftime(out, cap, "%Y-%m-%d %H:%M:%S", &tmv);
  return true;
}

}  // namespace Clock

namespace Countdown {

static long cachedDays = -1;      // true remaining days, -1 = unavailable
static uint32_t lastCalcMs = 0;
static bool dirty = true;

inline bool dateValid() {
  int y, m, d;
  return settings.eventDate[0] && parseIsoDate(settings.eventDate, y, m, d);
}

// Call after any change to the event date or timezone.
inline void invalidate() { dirty = true; }

inline long computeDays() {
  int y, m, d;
  if (!settings.eventDate[0] || !parseIsoDate(settings.eventDate, y, m, d)) return -1;
  if (!Clock::synced()) return -1;
  struct tm t;
  memset(&t, 0, sizeof(t));
  t.tm_year = y - 1900;
  t.tm_mon = m - 1;
  t.tm_mday = d;
  t.tm_hour = 0;
  t.tm_isdst = -1;
  time_t target = mktime(&t);
  if (target == (time_t)-1) return -1;
  int64_t diff = (int64_t)target - (int64_t)time(nullptr);
  if (diff <= 0) return 0;
  return (long)((diff + 86399) / 86400);
}

// Recalculate once per second (or immediately after invalidate()).
inline void update(uint32_t now) {
  if (!dirty && (uint32_t)(now - lastCalcMs) < 1000UL) return;
  dirty = false;
  lastCalcMs = now;
  cachedDays = computeDays();
}

inline long days() { return cachedDays; }

// True when the countdown should be shown while idle.
inline bool active() { return settings.countdownEnabled && dateValid() && cachedDays >= 0; }

inline uint8_t displayValue() {
  if (cachedDays < 0) return 0;
  return cachedDays > 99 ? 99 : (uint8_t)cachedDays;
}

}  // namespace Countdown
