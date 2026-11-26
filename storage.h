// ============================================================================
//  storage.h - persistent settings (ESP32 Preferences / NVS)
//
//  All values are validated on load; anything invalid silently falls back to
//  the compile-time default. Settings live in NVS, which is a separate
//  partition, so they survive OTA updates.
//
//  NOTE: the Wi-Fi password is stored in clear text in NVS (the ESP32 needs
//  it in clear text to connect). It is never returned by any API response.
// ============================================================================
#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"

struct Settings {
  char ssid[33];
  char pass[64];
  char eventDate[11];  // "YYYY-MM-DD" or ""
  bool countdownEnabled;
  uint16_t timeoutS;
  uint16_t patternMs;
  uint16_t countMs;
  uint16_t fig8Ms;
  uint16_t flashMs;
  bool powerSave;
  char tz[48];
};

static Settings settings;

// ---------------------------------------------------------------------------
// Validators (shared by storage and the web API)
// ---------------------------------------------------------------------------
inline bool parseIsoDate(const char* s, int& y, int& m, int& d) {
  if (!s || strlen(s) != 10 || s[4] != '-' || s[7] != '-') return false;
  for (int i = 0; i < 10; i++) {
    if (i != 4 && i != 7 && !isdigit((unsigned char)s[i])) return false;
  }
  y = atoi(s);
  m = atoi(s + 5);
  d = atoi(s + 8);
  if (y < 2000 || y > 2099 || m < 1 || m > 12 || d < 1) return false;
  static const uint8_t dim[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  int maxDay = dim[m - 1];
  if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) maxDay = 29;
  return d <= maxDay;
}

inline bool validSsid(const char* s) {
  size_t n = s ? strlen(s) : 0;
  return n >= 1 && n <= 32;
}

// WPA2 passphrase: 8..63 characters; empty = open network.
inline bool validPassword(const char* p) {
  size_t n = p ? strlen(p) : 0;
  return n == 0 || (n >= 8 && n <= 63);
}

inline bool validTz(const char* tz) {
  size_t n = tz ? strlen(tz) : 0;
  if (n < 1 || n > 47) return false;
  for (size_t i = 0; i < n; i++) {
    char c = tz[i];
    if (!(isalnum((unsigned char)c) || c == ',' || c == '.' || c == ':' || c == '+' ||
          c == '-' || c == '/' || c == '<' || c == '>'))
      return false;
  }
  return true;
}

inline bool validAnimMs(long v) { return v >= MIN_ANIM_MS && v <= MAX_ANIM_MS; }
inline bool validTimeoutS(long v) { return v >= MIN_TIMEOUT_S && v <= MAX_TIMEOUT_S; }

namespace Storage {

static const char* NS = "dispctl";

inline void setDefaults(Settings& s) {
  memset(&s, 0, sizeof(s));
  strncpy(s.ssid, WIFI_SSID, sizeof(s.ssid) - 1);
  strncpy(s.pass, WIFI_PASSWORD, sizeof(s.pass) - 1);
  s.eventDate[0] = 0;
  s.countdownEnabled = false;
  s.timeoutS = DEFAULT_TIMEOUT_S;
  s.patternMs = DEFAULT_PATTERN_MS;
  s.countMs = DEFAULT_COUNT_MS;
  s.fig8Ms = DEFAULT_FIG8_MS;
  s.flashMs = DEFAULT_FLASH_MS;
  s.powerSave = DEFAULT_POWER_SAVE;
  strncpy(s.tz, DEFAULT_TZ, sizeof(s.tz) - 1);
}

// Loads and validates every setting. Never fails: invalid or missing values
// keep their defaults.
inline void load() {
  setDefaults(settings);
  Preferences p;
  if (!p.begin(NS, false)) return;

  // Wi-Fi: use stored credentials only if the stored pair is valid
  char ssid[33] = {0};
  char pass[64] = {0};
  p.getString("ssid", ssid, sizeof(ssid));
  p.getString("pass", pass, sizeof(pass));
  if (validSsid(ssid) && validPassword(pass)) {
    strncpy(settings.ssid, ssid, sizeof(settings.ssid) - 1);
    strncpy(settings.pass, pass, sizeof(settings.pass) - 1);
  }

  // Countdown
  char date[11] = {0};
  p.getString("evdate", date, sizeof(date));
  int y, m, d;
  if (date[0] && parseIsoDate(date, y, m, d)) strncpy(settings.eventDate, date, sizeof(settings.eventDate) - 1);
  settings.countdownEnabled = p.getBool("cden", false);

  // Timing
  long v = p.getUShort("timeout", DEFAULT_TIMEOUT_S);
  if (validTimeoutS(v)) settings.timeoutS = (uint16_t)v;
  v = p.getUShort("patMs", DEFAULT_PATTERN_MS);
  if (validAnimMs(v)) settings.patternMs = (uint16_t)v;
  v = p.getUShort("cntMs", DEFAULT_COUNT_MS);
  if (validAnimMs(v)) settings.countMs = (uint16_t)v;
  v = p.getUShort("f8Ms", DEFAULT_FIG8_MS);
  if (validAnimMs(v)) settings.fig8Ms = (uint16_t)v;
  v = p.getUShort("flMs", DEFAULT_FLASH_MS);
  if (validAnimMs(v)) settings.flashMs = (uint16_t)v;

  // Power saving
  settings.powerSave = p.getBool("psave", DEFAULT_POWER_SAVE);

  // Timezone
  char tz[48] = {0};
  p.getString("tz", tz, sizeof(tz));
  if (validTz(tz)) strncpy(settings.tz, tz, sizeof(settings.tz) - 1);

  p.end();
}

inline bool saveWifi() {
  Preferences p;
  if (!p.begin(NS, false)) return false;
  p.putString("ssid", settings.ssid);
  p.putString("pass", settings.pass);
  p.end();
  return true;
}

inline bool saveCountdown() {
  Preferences p;
  if (!p.begin(NS, false)) return false;
  p.putString("evdate", settings.eventDate);
  p.putBool("cden", settings.countdownEnabled);
  p.end();
  return true;
}

inline bool saveTiming() {
  Preferences p;
  if (!p.begin(NS, false)) return false;
  p.putUShort("timeout", settings.timeoutS);
  p.putUShort("patMs", settings.patternMs);
  p.putUShort("cntMs", settings.countMs);
  p.putUShort("f8Ms", settings.fig8Ms);
  p.putUShort("flMs", settings.flashMs);
  p.end();
  return true;
}

inline bool savePower() {
  Preferences p;
  if (!p.begin(NS, false)) return false;
  p.putBool("psave", settings.powerSave);
  p.end();
  return true;
}

inline bool saveTz() {
  Preferences p;
  if (!p.begin(NS, false)) return false;
  p.putString("tz", settings.tz);
  p.end();
  return true;
}

}  // namespace Storage
