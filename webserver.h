// ============================================================================
//  webserver.h - HTTP server, JSON REST API and browser OTA upload
//
//  * Uses the stock ESP32 Arduino WebServer (no external libraries).
//  * JSON is produced with a fixed-size stack buffer (no String building) and
//    parsed with a tiny strict top-level parser (no ArduinoJson).
//  * Every API response is JSON; errors look like {"ok":false,"error":"..."}.
//  * GET requests never modify display state, so polling cannot reset
//    timeouts or interrupt patterns.
// ============================================================================
#pragma once

#include <Arduino.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <ctype.h>
#include <esp_timer.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "countdown.h"
#include "display.h"
#include "networking.h"
#include "patterns.h"
#include "power.h"
#include "storage.h"
#include "web_ui.h"

static WebServer server(HTTP_PORT);

// ============================================================================
//  Minimal JSON writer (bounded buffer)
// ============================================================================
struct JsonOut {
  char* buf;
  size_t cap;
  size_t len;
  bool over;

  JsonOut(char* b, size_t c) : buf(b), cap(c), len(0), over(false) {
    if (cap) buf[0] = 0;
  }
  void raw(const char* s) {
    if (over) return;
    size_t n = strlen(s);
    if (len + n + 1 > cap) { over = true; return; }
    memcpy(buf + len, s, n + 1);
    len += n;
  }
  void fmt(const char* f, ...) __attribute__((format(printf, 2, 3)));
  void str(const char* s) {  // quoted + escaped
    raw("\"");
    for (; *s && !over; s++) {
      unsigned char c = (unsigned char)*s;
      if (c == '"') raw("\\\"");
      else if (c == '\\') raw("\\\\");
      else if (c < 0x20) fmt("\\u%04x", c);
      else {
        char t[2] = {(char)c, 0};
        raw(t);
      }
    }
    raw("\"");
  }
};

inline void JsonOut::fmt(const char* f, ...) {
  if (over) return;
  va_list a;
  va_start(a, f);
  int n = vsnprintf(buf + len, cap - len, f, a);
  va_end(a);
  if (n < 0 || (size_t)n >= cap - len) {
    over = true;
    buf[len] = 0;
  } else {
    len += (size_t)n;
  }
}

// ============================================================================
//  Minimal JSON reader (top-level object only, strict about structure)
// ============================================================================
namespace Jr {

inline const char* skipWs(const char* p) {
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
  return p;
}

inline const char* skipString(const char* p) {  // p at opening quote
  if (*p != '"') return nullptr;
  p++;
  while (*p) {
    if (*p == '\\') {
      if (!p[1]) return nullptr;
      p += 2;
    } else if (*p == '"') {
      return p + 1;
    } else {
      p++;
    }
  }
  return nullptr;
}

inline const char* skipValue(const char* p) {
  p = skipWs(p);
  if (!*p) return nullptr;
  if (*p == '"') return skipString(p);
  if (*p == '{' || *p == '[') {
    int depth = 0;
    while (*p) {
      if (*p == '"') {
        p = skipString(p);
        if (!p) return nullptr;
        continue;
      }
      if (*p == '{' || *p == '[') depth++;
      else if (*p == '}' || *p == ']') {
        depth--;
        if (depth == 0) return p + 1;
      }
      p++;
    }
    return nullptr;
  }
  while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
  return p;
}

inline bool objBegin(const char* s, const char*& it) {
  it = skipWs(s);
  if (*it != '{') return false;
  it++;
  return true;
}

// 1 = member read (key copied, val -> value start), 0 = end of object, -1 = malformed
inline int objNext(const char*& it, char* key, size_t cap, const char*& val) {
  it = skipWs(it);
  if (*it == '}') { it++; return 0; }
  if (*it == ',') it = skipWs(it + 1);
  if (*it != '"') return -1;
  const char* p = it + 1;
  size_t n = 0;
  while (*p && *p != '"') {
    if (*p == '\\' && p[1]) p++;
    if (n + 1 < cap) key[n++] = *p;
    p++;
  }
  if (*p != '"') return -1;
  key[n] = 0;
  p = skipWs(p + 1);
  if (*p != ':') return -1;
  p = skipWs(p + 1);
  val = p;
  const char* e = skipValue(p);
  if (!e) return -1;
  it = e;
  return 1;
}

// Finds a top-level key. Returns pointer to its value or nullptr.
inline const char* find(const char* s, const char* wanted) {
  const char* it;
  if (!objBegin(s, it)) return nullptr;
  char key[24];
  const char* val;
  int r;
  while ((r = objNext(it, key, sizeof(key), val)) == 1) {
    if (!strcmp(key, wanted)) return val;
  }
  return nullptr;
}

inline bool isObject(const char* s) { return *skipWs(s) == '{'; }

inline bool valBool(const char* v, bool& out) {
  if (!strncmp(v, "true", 4) && !isalnum((unsigned char)v[4])) { out = true; return true; }
  if (!strncmp(v, "false", 5) && !isalnum((unsigned char)v[5])) { out = false; return true; }
  return false;
}

inline bool valInt(const char* v, long& out) {
  if (!(*v == '-' || isdigit((unsigned char)*v))) return false;
  char* e;
  long x = strtol(v, &e, 10);
  if (e == v || *e == '.' || *e == 'e' || *e == 'E') return false;
  out = x;
  return true;
}

// Decodes a JSON string into out (UTF-8). False if not a string, too long or invalid.
inline bool valString(const char* v, char* out, size_t cap) {
  if (*v != '"') return false;
  v++;
  size_t n = 0;
  while (*v && *v != '"') {
    uint32_t cp;
    if (*v == '\\') {
      v++;
      switch (*v) {
        case '"': cp = '"'; break;
        case '\\': cp = '\\'; break;
        case '/': cp = '/'; break;
        case 'b': cp = '\b'; break;
        case 'f': cp = '\f'; break;
        case 'n': cp = '\n'; break;
        case 'r': cp = '\r'; break;
        case 't': cp = '\t'; break;
        case 'u': {
          cp = 0;
          for (int i = 1; i <= 4; i++) {
            char h = v[i];
            if (!isxdigit((unsigned char)h)) return false;
            cp = cp * 16 + (uint32_t)(isdigit((unsigned char)h) ? h - '0' : (tolower(h) - 'a' + 10));
          }
          v += 4;
          break;
        }
        default: return false;
      }
      v++;
    } else {
      cp = (unsigned char)*v;
      v++;
      if (cp >= 0x80) {  // pass raw UTF-8 bytes through unchanged
        if (n + 1 >= cap) return false;
        out[n++] = (char)cp;
        continue;
      }
    }
    if (cp < 0x80) {
      if (n + 1 >= cap) return false;
      out[n++] = (char)cp;
    } else if (cp < 0x800) {
      if (n + 2 >= cap) return false;
      out[n++] = (char)(0xC0 | (cp >> 6));
      out[n++] = (char)(0x80 | (cp & 0x3F));
    } else {
      if (n + 3 >= cap) return false;
      out[n++] = (char)(0xE0 | (cp >> 12));
      out[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
      out[n++] = (char)(0x80 | (cp & 0x3F));
    }
  }
  if (*v != '"') return false;
  out[n] = 0;
  return true;
}

// ["left.A","right.G",...] -> segment mask (union). Empty array -> 0.
inline bool valSegmentList(const char* v, uint16_t& mask) {
  const char* p = skipWs(v);
  if (*p != '[') return false;
  p++;
  mask = 0;
  while (true) {
    p = skipWs(p);
    if (*p == ']') return true;
    if (*p == ',') { p++; continue; }
    if (*p != '"') return false;
    char tok[16];
    if (!valString(p, tok, sizeof(tok))) return false;
    uint16_t m;
    if (!Display::parseToken(tok, m)) return false;
    mask |= m;
    p = skipString(p);
    if (!p) return false;
  }
}

}  // namespace Jr

// ============================================================================
//  Web layer
// ============================================================================
namespace Web {

static bool started = false;
static uint32_t restartAtMs = 0;

// OTA upload state
static bool otaStarted = false;
static bool otaDone = false;
static bool otaDenied = false;
static char otaError[96];

// ---------------------------------------------------------------------------
// Responses
// ---------------------------------------------------------------------------
inline void sendJson(int code, JsonOut& j) {
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  if (j.over) {
    server.send(500, "application/json", "{\"ok\":false,\"error\":\"response buffer overflow\"}");
    return;
  }
  server.send(code, "application/json", j.buf);
}

inline void sendError(int code, const char* msg) {
  char b[200];
  JsonOut j(b, sizeof(b));
  j.raw("{\"ok\":false,\"error\":");
  j.str(msg);
  j.raw("}");
  sendJson(code, j);
}

inline void sendErrorf(int code, const char* f, ...) __attribute__((format(printf, 2, 3)));
inline void sendErrorf(int code, const char* f, ...) {
  char m[140];
  va_list a;
  va_start(a, f);
  vsnprintf(m, sizeof(m), f, a);
  va_end(a);
  sendError(code, m);
}

// ---------------------------------------------------------------------------
// Auth + guards
// ---------------------------------------------------------------------------
inline bool authorizedQuiet() {
#if AUTH_ENABLED
  return server.authenticate(AUTH_USERNAME, AUTH_PASSWORD);
#else
  return true;
#endif
}

inline bool authorize() {
  if (authorizedQuiet()) return true;
  server.sendHeader("WWW-Authenticate", "Basic realm=\"ESP32 Display\"");
  sendError(401, "authentication required");
  return false;
}

// Common entry check: method, authentication, wake from power saving.
inline bool enter(HTTPMethod expected) {
  if (server.method() != expected) {
    server.sendHeader("Allow", expected == HTTP_GET ? "GET" : "POST");
    sendError(405, "method not allowed");
    return false;
  }
  if (!authorize()) return false;
  Power::wake();
  return true;
}

inline bool commandAllowed() {
  if (Power::otaActive) {
    sendError(503, "firmware update in progress");
    return false;
  }
  return true;
}

// Reads the JSON body into 'body'. Returns false (error already sent) if unusable.
inline bool readBody(String& body) {
  body = server.arg("plain");
  if (body.length() == 0) {
    sendError(400, "JSON body required (Content-Type: application/json)");
    return false;
  }
  if (body.length() > MAX_BODY_BYTES) {
    sendError(413, "request body too large");
    return false;
  }
  if (!Jr::isObject(body.c_str())) {
    sendError(400, "request body must be a JSON object");
    return false;
  }
  return true;
}

// Optional "timeout" (seconds). out = 0 when absent. False = error sent.
inline bool readTimeout(const char* body, uint16_t& out) {
  out = 0;
  const char* v = Jr::find(body, "timeout");
  if (!v) return true;
  long x;
  if (!Jr::valInt(v, x) || !validTimeoutS(x)) {
    sendErrorf(400, "timeout must be an integer from %d to %d seconds", MIN_TIMEOUT_S, MAX_TIMEOUT_S);
    return false;
  }
  out = (uint16_t)x;
  return true;
}

// ---------------------------------------------------------------------------
// JSON fragments
// ---------------------------------------------------------------------------
inline void writeDisplay(JsonOut& j, uint16_t m) {
  const char* side[2] = {"left", "right"};
  for (int d = 0; d < 2; d++) {
    j.fmt("\"%s\":{", side[d]);
    for (int s = 0; s < 7; s++) {
      bool on = m & (1u << (d * 7 + s));
      j.fmt("\"%c\":%s%s", 'A' + s, on ? "true" : "false", s < 6 ? "," : "");
    }
    j.raw("},");
  }
  j.fmt("\"mask\":%u", (unsigned)m);
}

inline void writeCountdown(JsonOut& j) {
  char lt[24];
  bool haveTime = Clock::localTimeString(lt, sizeof(lt));
  long days = Countdown::days();
  j.raw("\"countdown\":{");
  j.fmt("\"enabled\":%s,", settings.countdownEnabled ? "true" : "false");
  j.raw("\"date\":");
  j.str(settings.eventDate);
  j.fmt(",\"dateValid\":%s,", Countdown::dateValid() ? "true" : "false");
  j.fmt("\"synced\":%s,", Clock::synced() ? "true" : "false");
  if (days >= 0) j.fmt("\"daysRemaining\":%ld,", days);
  else j.raw("\"daysRemaining\":null,");
  if (days >= 0) j.fmt("\"displayed\":%u,", (unsigned)Countdown::displayValue());
  else j.raw("\"displayed\":null,");
  j.fmt("\"active\":%s,", Countdown::active() ? "true" : "false");
  j.raw("\"localTime\":");
  if (haveTime) j.str(lt);
  else j.raw("null");
  j.raw(",\"timezone\":");
  j.str(settings.tz);
  j.fmt(",\"ntpAttempts\":%u}", (unsigned)Clock::ntpAttempts);
}

inline void writeWifi(JsonOut& j) {
  char ip[16] = "";
  bool up = Net::isConnected();
  if (up) Net::ipString(ip, sizeof(ip));
  j.fmt("\"wifi\":{\"connected\":%s,\"state\":\"%s\",\"ssid\":", up ? "true" : "false", Net::stateName());
  j.str(up ? settings.ssid : "");
  j.raw(",\"ip\":");
  j.str(ip);
  if (up) j.fmt(",\"rssi\":%d}", (int)WiFi.RSSI());
  else j.raw(",\"rssi\":null}");
}

inline void writeWifiChange(JsonOut& j) {
  j.fmt("\"wifiChange\":{\"seq\":%u,\"status\":\"%s\",\"message\":", (unsigned)Net::changeSeq, Net::changeStatusName());
  j.str(Net::changeStatus == Net::CHG_NONE ? "" : Net::changeMsg);
  j.raw("}");
}

// Full state of the board. Segment states are read back from the real pads.
inline void writeState(JsonOut& j, uint32_t now) {
  uint16_t actual = Display::readActualMask();
  int number = Display::decodeNumber(actual);
  j.raw("{\"ok\":true,");
  writeDisplay(j, actual);
  if (number >= 0) j.fmt(",\"number\":%d", number);
  else j.raw(",\"number\":null");
  j.fmt(",\"mode\":\"%s\",\"pattern\":\"%s\",", Req::modeName(), Req::patternName());
  j.fmt("\"requestActive\":%s,\"timeoutRemainingMs\":%lu,", Req::active() ? "true" : "false",
        (unsigned long)Req::remainingMs(now));
  j.raw("\"lastEndReason\":");
  j.str(Req::endReason());
  j.raw(",");
  writeWifi(j);
  j.raw(",");
  writeWifiChange(j);
  j.raw(",");
  writeCountdown(j);
  j.fmt(",\"powerSave\":%s,\"uptimeS\":%lu}", settings.powerSave ? "true" : "false",
        (unsigned long)(esp_timer_get_time() / 1000000ULL));
}

inline void sendState(int code) {
  char b[1280];
  JsonOut j(b, sizeof(b));
  writeState(j, millis());
  sendJson(code, j);
}

// ---------------------------------------------------------------------------
// GET /
// ---------------------------------------------------------------------------
inline void handleRoot() {
  if (!enter(HTTP_GET)) return;
  server.sendHeader("Cache-Control", "no-cache");
  server.send_P(200, "text/html", INDEX_HTML);
}

// ---------------------------------------------------------------------------
// GET /api/state, GET /api/info
// ---------------------------------------------------------------------------
inline void handleState() {
  if (!enter(HTTP_GET)) return;
  sendState(200);
}

inline void handleInfo() {
  if (!enter(HTTP_GET)) return;
  char b[1792];
  JsonOut j(b, sizeof(b));
  char ip[16] = "";
  bool up = Net::isConnected();
  if (up) Net::ipString(ip, sizeof(ip));
  uint16_t actual = Display::readActualMask();

  j.raw("{\"ok\":true,\"firmware\":");
  j.str(FIRMWARE_VERSION);
  j.raw(",\"chipModel\":");
  j.str(ESP.getChipModel());
  j.fmt(",\"chipRevision\":%d,\"cpuMHz\":%u,\"flashSize\":%lu,", (int)ESP.getChipRevision(),
        (unsigned)ESP.getCpuFreqMHz(), (unsigned long)ESP.getFlashChipSize());
  j.fmt("\"psramSize\":%lu,\"freePsram\":%lu,", (unsigned long)ESP.getPsramSize(), (unsigned long)ESP.getFreePsram());
  j.fmt("\"freeHeap\":%lu,\"minFreeHeap\":%lu,", (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMinFreeHeap());
  j.raw("\"wifiSsid\":");
  j.str(up ? settings.ssid : "");
  j.raw(",\"ip\":");
  j.str(ip);
  if (up) j.fmt(",\"rssi\":%d", (int)WiFi.RSSI());
  else j.raw(",\"rssi\":null");
  j.raw(",\"hostname\":");
  j.str(DEVICE_HOSTNAME);
  j.raw(",\"mdns\":");
  j.str(DEVICE_HOSTNAME ".local");
  j.fmt(",\"mdnsActive\":%s,", Net::mdnsActive ? "true" : "false");
  j.fmt("\"uptimeS\":%lu,", (unsigned long)(esp_timer_get_time() / 1000000ULL));
  j.fmt("\"activePattern\":\"%s\",\"mode\":\"%s\",", Req::patternName(), Req::modeName());
  j.raw("\"display\":{");
  writeDisplay(j, actual);
  j.raw("},");
  writeCountdown(j);
  j.fmt(",\"powerSave\":%s,\"lowCpu\":%s,", settings.powerSave ? "true" : "false", Power::lowCpu ? "true" : "false");
  j.fmt("\"authEnabled\":%s}", AUTH_ENABLED ? "true" : "false");
  sendJson(200, j);
}

// ---------------------------------------------------------------------------
// POST /api/segments   {"segments":{"left.A":true,"right.G":false}} | {"mask":123}
// ---------------------------------------------------------------------------
inline void handleSegments() {
  if (!enter(HTTP_POST) || !commandAllowed()) return;
  String body;
  if (!readBody(body)) return;
  const char* s = body.c_str();
  uint16_t timeoutS;
  if (!readTimeout(s, timeoutS)) return;

  uint16_t mask = Display::desiredMask;
  const char* vMask = Jr::find(s, "mask");
  const char* vSegs = Jr::find(s, "segments");
  if (vMask) {
    long x;
    if (!Jr::valInt(vMask, x) || x < 0 || x > Display::ALL_MASK) {
      sendErrorf(400, "mask must be an integer from 0 to %u", (unsigned)Display::ALL_MASK);
      return;
    }
    mask = (uint16_t)x;
  } else if (vSegs) {
    const char* it;
    if (!Jr::objBegin(vSegs, it)) {
      sendError(400, "segments must be an object such as {\"left.A\":true}");
      return;
    }
    char key[24];
    const char* val;
    int r;
    int count = 0;
    while ((r = Jr::objNext(it, key, sizeof(key), val)) == 1) {
      uint16_t m;
      bool on;
      if (!Display::parseToken(key, m) || key[0] == 0) {
        sendErrorf(400, "unknown segment '%s' (use left.A .. right.G)", key);
        return;
      }
      if (!Jr::valBool(val, on)) {
        sendErrorf(400, "value for '%s' must be true or false", key);
        return;
      }
      if (on) mask |= m;
      else mask &= (uint16_t)~m;
      count++;
    }
    if (r < 0) {
      sendError(400, "malformed segments object");
      return;
    }
    if (count == 0) {
      sendError(400, "segments object is empty");
      return;
    }
  } else {
    sendError(400, "provide \"segments\" (object) or \"mask\" (integer)");
    return;
  }
  Req::requestMask(mask, timeoutS, millis());
  sendState(200);
}

// ---------------------------------------------------------------------------
// POST /api/number   {"number":42}
// ---------------------------------------------------------------------------
inline void handleNumber() {
  if (!enter(HTTP_POST) || !commandAllowed()) return;
  String body;
  if (!readBody(body)) return;
  const char* s = body.c_str();
  uint16_t timeoutS;
  if (!readTimeout(s, timeoutS)) return;
  const char* v = Jr::find(s, "number");
  long n;
  if (!v) {
    sendError(400, "\"number\" is required");
    return;
  }
  if (!Jr::valInt(v, n) || n < 0 || n > 99) {
    sendError(400, "number must be an integer from 0 to 99");
    return;
  }
  Req::requestNumber((uint8_t)n, timeoutS, millis());
  sendState(200);
}

// ---------------------------------------------------------------------------
// POST /api/all   {"on":true}
// ---------------------------------------------------------------------------
inline void handleAll() {
  if (!enter(HTTP_POST) || !commandAllowed()) return;
  String body;
  if (!readBody(body)) return;
  const char* s = body.c_str();
  uint16_t timeoutS;
  if (!readTimeout(s, timeoutS)) return;
  const char* v = Jr::find(s, "on");
  bool on;
  if (!v || !Jr::valBool(v, on)) {
    sendError(400, "\"on\" (true or false) is required");
    return;
  }
  Req::requestAll(on, timeoutS, millis());
  sendState(200);
}

// ---------------------------------------------------------------------------
// POST /api/pattern
//   {"pattern":"flash|count|figure8|blink", "speed":150, "timeout":10,
//    "segments":["left.A","right.G"], "count":5}
//   "pattern":"none" stops the active request.
// ---------------------------------------------------------------------------
inline void handlePattern() {
  if (!enter(HTTP_POST) || !commandAllowed()) return;
  String body;
  if (!readBody(body)) return;
  const char* s = body.c_str();

  const char* vp = Jr::find(s, "pattern");
  char name[20];
  if (!vp || !Jr::valString(vp, name, sizeof(name))) {
    sendError(400, "\"pattern\" is required: flash, count, figure8, blink");
    return;
  }
  if (!strcasecmp(name, "none") || !strcasecmp(name, "stop")) {
    Req::stop(millis());
    sendState(200);
    return;
  }
  Req::PatternId pat = Req::parsePattern(name);
  if (pat == Req::PAT_NONE) {
    sendError(400, "unknown pattern (use flash, count, figure8, blink)");
    return;
  }
  uint16_t timeoutS;
  if (!readTimeout(s, timeoutS)) return;

  uint16_t speed = 0;
  const char* vs = Jr::find(s, "speed");
  if (vs) {
    long x;
    if (!Jr::valInt(vs, x) || !validAnimMs(x)) {
      sendErrorf(400, "speed must be an integer from %d to %d ms", MIN_ANIM_MS, MAX_ANIM_MS);
      return;
    }
    speed = (uint16_t)x;
  }

  uint16_t blinkMask = 0, blinkCount = 0;
  if (pat == Req::PAT_BLINK) {
    const char* vseg = Jr::find(s, "segments");
    if (vseg && !Jr::valSegmentList(vseg, blinkMask)) {
      sendError(400, "segments must be an array of names like \"left.A\", \"right.G\", \"left\", \"right\", \"all\"");
      return;
    }
    const char* vc = Jr::find(s, "count");
    blinkCount = BLINK_COUNT_DEFAULT;
    if (vc) {
      long x;
      if (!Jr::valInt(vc, x) || x < 1 || x > BLINK_COUNT_MAX) {
        sendErrorf(400, "count must be an integer from 1 to %d", BLINK_COUNT_MAX);
        return;
      }
      blinkCount = (uint16_t)x;
    }
  }
  Req::requestPattern(pat, speed, timeoutS, blinkMask, blinkCount, millis());
  sendState(200);
}

// ---------------------------------------------------------------------------
// POST /api/stop
// ---------------------------------------------------------------------------
inline void handleStop() {
  if (!enter(HTTP_POST) || !commandAllowed()) return;
  Req::stop(millis());
  sendState(200);
}

// ---------------------------------------------------------------------------
// GET/POST /api/countdown   {"date":"2027-01-15","enabled":true}
// ---------------------------------------------------------------------------
inline void handleCountdown() {
  if (server.method() == HTTP_GET) {
    if (!enter(HTTP_GET)) return;
    char b[640];
    JsonOut j(b, sizeof(b));
    j.raw("{\"ok\":true,");
    writeCountdown(j);
    j.raw("}");
    sendJson(200, j);
    return;
  }
  if (!enter(HTTP_POST) || !commandAllowed()) return;
  String body;
  if (!readBody(body)) return;
  const char* s = body.c_str();
  const char* vd = Jr::find(s, "date");
  const char* ve = Jr::find(s, "enabled");
  if (!vd && !ve) {
    sendError(400, "provide \"date\" (YYYY-MM-DD or \"\") and/or \"enabled\" (true/false)");
    return;
  }
  char date[16] = "";
  bool enabled = settings.countdownEnabled;
  if (vd) {
    if (!Jr::valString(vd, date, sizeof(date))) {
      sendError(400, "date must be a string in the form YYYY-MM-DD");
      return;
    }
    int y, m, d;
    if (date[0] && !parseIsoDate(date, y, m, d)) {
      sendError(400, "invalid date; use a real calendar date from 2000-01-01 to 2099-12-31 (YYYY-MM-DD)");
      return;
    }
  }
  if (ve && !Jr::valBool(ve, enabled)) {
    sendError(400, "enabled must be true or false");
    return;
  }
  if (vd) {
    strncpy(settings.eventDate, date, sizeof(settings.eventDate) - 1);
    settings.eventDate[sizeof(settings.eventDate) - 1] = 0;
  }
  settings.countdownEnabled = enabled;
  Storage::saveCountdown();
  Countdown::invalidate();
  Countdown::update(millis());
  // An active temporary request is NOT interrupted; the countdown shows
  // as soon as that request ends.
  sendState(200);
}

// ---------------------------------------------------------------------------
// GET/POST /api/settings
// ---------------------------------------------------------------------------
inline bool readAnim(const char* v, uint16_t& out) {
  long x;
  if (!Jr::valInt(v, x) || !validAnimMs(x)) return false;
  out = (uint16_t)x;
  return true;
}

inline void handleSettings() {
  if (server.method() == HTTP_GET) {
    if (!enter(HTTP_GET)) return;
    char b[512];
    JsonOut j(b, sizeof(b));
    j.fmt("{\"ok\":true,\"timeout\":%u,\"patternMs\":%u,\"countMs\":%u,\"fig8Ms\":%u,\"flashMs\":%u,", settings.timeoutS,
          settings.patternMs, settings.countMs, settings.fig8Ms, settings.flashMs);
    j.fmt("\"powerSave\":%s,\"tz\":", settings.powerSave ? "true" : "false");
    j.str(settings.tz);
    j.raw(",\"ssid\":");
    j.str(settings.ssid);
    // The stored password is never returned; only whether one is set.
    j.fmt(",\"passwordSet\":%s,\"authEnabled\":%s}", settings.pass[0] ? "true" : "false", AUTH_ENABLED ? "true" : "false");
    sendJson(200, j);
    return;
  }
  if (!enter(HTTP_POST) || !commandAllowed()) return;
  String body;
  if (!readBody(body)) return;
  const char* s = body.c_str();

  Settings next = settings;
  char newSsid[33] = "";
  char newPass[64] = "";
  bool gotSsid = false, gotPass = false;

  const char* it;
  Jr::objBegin(s, it);
  char key[24];
  const char* v;
  int r;
  while ((r = Jr::objNext(it, key, sizeof(key), v)) == 1) {
    long x;
    bool bv;
    if (!strcmp(key, "timeout")) {
      if (!Jr::valInt(v, x) || !validTimeoutS(x)) {
        sendErrorf(400, "timeout must be an integer from %d to %d seconds", MIN_TIMEOUT_S, MAX_TIMEOUT_S);
        return;
      }
      next.timeoutS = (uint16_t)x;
    } else if (!strcmp(key, "patternMs") || !strcmp(key, "countMs") || !strcmp(key, "fig8Ms") ||
               !strcmp(key, "flashMs")) {
      uint16_t* dst = !strcmp(key, "patternMs") ? &next.patternMs
                      : !strcmp(key, "countMs") ? &next.countMs
                      : !strcmp(key, "fig8Ms")  ? &next.fig8Ms
                                                : &next.flashMs;
      if (!readAnim(v, *dst)) {
        sendErrorf(400, "%s must be an integer from %d to %d ms", key, MIN_ANIM_MS, MAX_ANIM_MS);
        return;
      }
    } else if (!strcmp(key, "powerSave")) {
      if (!Jr::valBool(v, bv)) {
        sendError(400, "powerSave must be true or false");
        return;
      }
      next.powerSave = bv;
    } else if (!strcmp(key, "tz")) {
      char tz[48];
      if (!Jr::valString(v, tz, sizeof(tz)) || !validTz(tz)) {
        sendError(400, "tz must be a POSIX timezone string such as EST5EDT,M3.2.0,M11.1.0 (max 47 chars)");
        return;
      }
      strcpy(next.tz, tz);
    } else if (!strcmp(key, "ssid")) {
      if (!Jr::valString(v, newSsid, sizeof(newSsid)) || !validSsid(newSsid)) {
        sendError(400, "ssid must be 1 to 32 characters");
        return;
      }
      gotSsid = true;
    } else if (!strcmp(key, "password")) {
      if (!Jr::valString(v, newPass, sizeof(newPass)) || !validPassword(newPass)) {
        sendError(400, "password must be empty (open network) or 8 to 63 characters");
        return;
      }
      gotPass = true;
    } else {
      sendErrorf(400, "unknown setting '%s'", key);
      return;
    }
  }
  if (r < 0) {
    sendError(400, "malformed JSON");
    return;
  }

  // Wi-Fi credential change?
  bool credChange = false;
  if (gotSsid || gotPass) {
    if (!gotSsid) strcpy(newSsid, settings.ssid);
    bool sameSsid = strcmp(newSsid, settings.ssid) == 0;
    if (!gotPass && !sameSsid) newPass[0] = 0;  // new SSID without password = open network
    credChange = gotPass || !sameSsid;
  }

  // Commit (everything validated above, so this cannot half-apply)
  bool timingChanged = next.timeoutS != settings.timeoutS || next.patternMs != settings.patternMs ||
                       next.countMs != settings.countMs || next.fig8Ms != settings.fig8Ms ||
                       next.flashMs != settings.flashMs;
  bool powerChanged = next.powerSave != settings.powerSave;
  bool tzChanged = strcmp(next.tz, settings.tz) != 0;
  settings = next;

  if (timingChanged) Storage::saveTiming();
  if (powerChanged) {
    Storage::savePower();
    Power::apply();
  }
  if (tzChanged) {
    Storage::saveTz();
    Clock::applyTimezone(settings.tz);
    Countdown::invalidate();
    Countdown::update(millis());
  }
  if (credChange) Net::scheduleCredentialChange(newSsid, newPass);
  // Display requests and running patterns are deliberately left untouched.
  sendState(credChange ? 202 : 200);
}

// ---------------------------------------------------------------------------
// POST /update   (multipart/form-data, field "firmware")
// ---------------------------------------------------------------------------
inline void otaFail(const char* msg) {
  if (!otaError[0]) {
    strncpy(otaError, msg, sizeof(otaError) - 1);
    otaError[sizeof(otaError) - 1] = 0;
  }
}

inline void handleUpdateUpload() {
  HTTPUpload& up = server.upload();
  switch (up.status) {
    case UPLOAD_FILE_START:
      otaStarted = false;
      otaDone = false;
      otaDenied = false;
      otaError[0] = 0;
      if (!authorizedQuiet()) {
        otaDenied = true;
        return;
      }
      if (ESP.getFreeHeap() < MIN_HEAP_FOR_OTA) {
        otaFail("not enough free memory for the update");
        return;
      }
      Power::beginOta();
      Display::setDisplayState(Display::ALL_MASK);  // all segments ON while uploading
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
        otaFail(Update.errorString());
        return;
      }
      otaStarted = true;
      break;

    case UPLOAD_FILE_WRITE:
      if (otaStarted && !otaError[0]) {
        if (Update.write(up.buf, up.currentSize) != up.currentSize) {
          otaFail(Update.errorString());
          Update.abort();
        }
      }
      break;

    case UPLOAD_FILE_END:
      if (otaStarted && !otaError[0]) {
        if (Update.end(true)) otaDone = true;
        else otaFail(Update.errorString());
      }
      break;

    case UPLOAD_FILE_ABORTED:
      // WebServer never calls handleUpdateDone() after an abort: clean up here.
      if (otaStarted) Update.abort();
      otaStarted = false;
      Power::endOta();
      break;
  }
}

inline void handleUpdateDone() {
  if (otaDenied) {
    otaDenied = false;
    server.sendHeader("WWW-Authenticate", "Basic realm=\"ESP32 Display\"");
    sendError(401, "authentication required");
    return;
  }
  if (otaError[0]) {
    Power::endOta();
    Display::refresh();
    sendErrorf(500, "update failed: %s", otaError);
    otaStarted = false;
    return;
  }
  if (!otaDone) {
    Power::endOta();
    sendError(400, "no firmware file received (send multipart/form-data with a .bin file)");
    return;
  }
  char b[128];
  JsonOut j(b, sizeof(b));
  j.raw("{\"ok\":true,\"message\":\"Update successful. Restarting...\"}");
  sendJson(200, j);
  restartAtMs = millis() + UPDATE_RESTART_DELAY_MS;
  if (restartAtMs == 0) restartAtMs = 1;
}

inline void handleUpdateGet() {
  server.sendHeader("Allow", "POST");
  sendError(405, "use POST with multipart/form-data");
}

inline void handleNotFound() {
  if (!authorize()) return;
  sendError(404, "not found");
}

inline void handleFavicon() { server.send(204); }

// ---------------------------------------------------------------------------
// Setup / loop
// ---------------------------------------------------------------------------
// Register routes (once, at boot). Does not start listening yet.
inline void begin() {
  server.on("/", HTTP_ANY, handleRoot);
  server.on("/favicon.ico", HTTP_ANY, handleFavicon);
  server.on("/api/state", HTTP_ANY, handleState);
  server.on("/api/info", HTTP_ANY, handleInfo);
  server.on("/api/segments", HTTP_ANY, handleSegments);
  server.on("/api/number", HTTP_ANY, handleNumber);
  server.on("/api/all", HTTP_ANY, handleAll);
  server.on("/api/pattern", HTTP_ANY, handlePattern);
  server.on("/api/stop", HTTP_ANY, handleStop);
  server.on("/api/countdown", HTTP_ANY, handleCountdown);
  server.on("/api/settings", HTTP_ANY, handleSettings);
  server.on("/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);
  server.on("/update", HTTP_GET, handleUpdateGet);
  server.onNotFound(handleNotFound);
  otaError[0] = 0;
}

// (Re)start listening; called after every successful Wi-Fi (re)connection.
inline void start() {
  server.begin();
  started = true;
}

inline void handle() {
  if (started) server.handleClient();
}

inline void serviceRestart(uint32_t now) {
  if (restartAtMs && (int32_t)(now - restartAtMs) >= 0) {
    ESP.restart();
  }
}

}  // namespace Web
