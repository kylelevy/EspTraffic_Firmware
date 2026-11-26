// ============================================================================
//  patterns.h - centralised display request manager + pattern engine
//
//  Priorities (highest first):
//    1. system overlay   - Wi-Fi "loading" animation / connect flash
//    2. active request   - segments, number, all on/off, or a pattern
//    3. idle             - countdown (if enabled and valid) or all OFF
//
//  Rules:
//    * only ONE request is active at a time;
//    * a new request replaces the current one immediately;
//    * every request has a timeout (default from settings, optional override);
//    * all timing uses millis(); nothing here ever blocks;
//    * GET requests never touch the manager, so polling cannot reset timeouts;
//    * changing settings never restarts an active pattern: speeds are read
//      from settings on every step, timeouts are fixed when a request starts.
// ============================================================================
#pragma once

#include <Arduino.h>
#include <string.h>
#include <strings.h>

#include "config.h"
#include "countdown.h"
#include "display.h"
#include "power.h"
#include "storage.h"

namespace Req {

enum Mode : uint8_t {
  MODE_IDLE,
  MODE_COUNTDOWN,
  MODE_SEGMENTS,
  MODE_NUMBER,
  MODE_ALL,
  MODE_PATTERN
};

enum PatternId : uint8_t {
  PAT_NONE,
  PAT_FLASH,
  PAT_COUNT,
  PAT_FIGURE8,
  PAT_BLINK
};

enum Overlay : uint8_t {
  OV_NONE,
  OV_LOADING,
  OV_FLASH
};

// Diagnostic: records why the previous request ended.
static const char* lastEndReason = "startup";

inline const char* endReason() {
  return lastEndReason;
}

// Figure-eight path through both digits.
static const uint8_t FIG8_PATH[12] = {
  0, 7, 8, 13, 6, 4, 3, 10, 9, 13, 6, 5
};

static const uint8_t FIG8_LEN = 12;

struct Request {
  bool active;
  Mode mode;
  PatternId pattern;
  uint16_t mask;
  uint16_t blinkMask;
  uint16_t blinkCount;
  uint16_t speedMs;
  uint32_t startedMs;
  uint32_t timeoutMs;

  // Runtime state
  uint32_t lastStepMs;
  uint16_t step;
  uint16_t cycles;
  bool phaseOn;
};

static Request req;
static Overlay overlay = OV_NONE;
static uint32_t overlayStartMs = 0;
static uint32_t loadLastMs = 0;
static uint8_t loadStep = 0;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
inline uint16_t fig8Mask(uint8_t head) {
  uint16_t m = 0;
  for (uint8_t k = 0; k < FIG8_TRAIL_LENGTH; k++) {
    m |= (uint16_t)(1u << FIG8_PATH[(head + FIG8_LEN - k) % FIG8_LEN]);
  }
  return m;
}

inline uint32_t intervalFor(PatternId p, uint16_t override_) {
  if (override_) return override_;
  switch (p) {
    case PAT_FLASH: return settings.flashMs;
    case PAT_COUNT: return settings.countMs;
    case PAT_FIGURE8: return settings.fig8Ms;
    case PAT_BLINK: return settings.patternMs;
    default: return settings.patternMs;
  }
}

// Advance the step timer without accumulating drift (but never "catch up"
// with a burst of steps after a long stall).
inline void advance(uint32_t& last, uint32_t interval, uint32_t now) {
  if ((uint32_t)(now - last) >= 2UL * interval) last = now;
  else last += interval;
}

inline uint16_t idleMask() {
  return Countdown::active() ? Display::numberMask(Countdown::displayValue()) : 0;
}

inline void applyIdle() { Display::showMask(idleMask()); }

inline void endRequest(const char* reason) {
  lastEndReason = reason;
  req.active = false;
  req.mode = MODE_IDLE;
  req.pattern = PAT_NONE;
}

// ---------------------------------------------------------------------------
// Pattern stepping
// ---------------------------------------------------------------------------
inline void stepPattern(uint32_t now) {
  uint32_t iv = intervalFor(req.pattern, req.speedMs);
  bool tick = (uint32_t)(now - req.lastStepMs) >= iv;
  if (tick) advance(req.lastStepMs, iv, now);

  switch (req.pattern) {
    case PAT_FLASH:
      if (tick) req.phaseOn = !req.phaseOn;
      Display::showMask(req.phaseOn ? Display::ALL_MASK : 0);
      break;

    case PAT_COUNT:  // 00 11 22 ... 99 00 ...
      if (tick) req.step = (req.step + 1) % 10;
      Display::showMask(Display::numberMask((uint8_t)(req.step * 11)));
      break;

    case PAT_FIGURE8:
      if (tick) req.step = (req.step + 1) % FIG8_LEN;
      Display::showMask(fig8Mask((uint8_t)req.step));
      break;

    case PAT_BLINK:
      // One cycle = ON interval + OFF interval. The request ends right
      // after the OFF interval of the last cycle.
      if (tick) {
        if (req.phaseOn) {
          req.phaseOn = false;
        } else {
          req.cycles++;
          if (req.cycles >= req.blinkCount) {
            endRequest("blink_complete");
            return;
          }
          req.phaseOn = true;
        }
      }
      Display::showMask(req.phaseOn ? req.blinkMask : 0);
      break;

    default:
      endRequest("invalid_pattern");
      break;
  }
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------
inline void render(uint32_t now) {
  if (overlay == OV_LOADING) {
    uint32_t iv = intervalFor(PAT_FIGURE8, 0);
    if ((uint32_t)(now - loadLastMs) >= iv) {
      advance(loadLastMs, iv, now);
      loadStep = (loadStep + 1) % FIG8_LEN;
    }
    Display::showMask(fig8Mask(loadStep));
    return;
  }
  if (overlay == OV_FLASH) {
    if ((uint32_t)(now - overlayStartMs) < CONNECT_FLASH_MS) {
      Display::showMask(Display::ALL_MASK);
      return;
    }
    overlay = OV_NONE;  // flash finished: fall through to normal rendering
  }

  if (!req.active) {
    applyIdle();
    return;
  }
  if ((uint32_t)(now - req.startedMs) >= req.timeoutMs) {
    endRequest("timeout");
    applyIdle();
    return;
  }
  switch (req.mode) {
    case MODE_SEGMENTS:
    case MODE_NUMBER:
    case MODE_ALL:
      Display::showMask(req.mask);
      break;
    case MODE_PATTERN:
      stepPattern(now);
      if (!req.active) applyIdle();
      break;
    default:
      endRequest("invalid_mode");
      applyIdle();
      break;
  }
}

inline void update(uint32_t now) { render(now); }

// ---------------------------------------------------------------------------
// System overlays
// ---------------------------------------------------------------------------
inline void setLoading(bool on, uint32_t now) {
  if (on && overlay != OV_LOADING) {
    overlay = OV_LOADING;
    loadStep = 0;
    loadLastMs = now;
  } else if (!on && overlay == OV_LOADING) {
    overlay = OV_NONE;
  }
}

inline void startConnectFlash(uint32_t now) {
  overlay = OV_FLASH;
  overlayStartMs = now;
}

// ---------------------------------------------------------------------------
// Requests
// ---------------------------------------------------------------------------
inline void startRequest(
  Mode mode,
  PatternId pat,
  uint16_t mask,
  uint16_t timeoutS,
  uint32_t now
) {
  if (overlay == OV_FLASH) {
    overlay = OV_NONE;
  }

  Power::wake();

  lastEndReason = "active";

  req.active = true;
  req.mode = mode;
  req.pattern = pat;
  req.mask = mask;
  req.blinkMask = 0;
  req.blinkCount = 0;
  req.speedMs = 0;
  req.startedMs = now;

  req.timeoutMs =
    (uint32_t)(timeoutS ? timeoutS : settings.timeoutS) * 1000UL;

  req.lastStepMs = now;
  req.step = 0;
  req.cycles = 0;
  req.phaseOn = true;
}

// timeoutS == 0 -> use the configured default timeout.
inline void requestMask(uint16_t mask, uint16_t timeoutS, uint32_t now) {
  startRequest(MODE_SEGMENTS, PAT_NONE, mask & Display::ALL_MASK, timeoutS, now);
  render(now);
}

inline void requestNumber(uint8_t n, uint16_t timeoutS, uint32_t now) {
  startRequest(MODE_NUMBER, PAT_NONE, Display::numberMask(n), timeoutS, now);
  render(now);
}

inline void requestAll(bool on, uint16_t timeoutS, uint32_t now) {
  startRequest(MODE_ALL, PAT_NONE, on ? Display::ALL_MASK : 0, timeoutS, now);
  render(now);
}

// speedMs == 0 -> configured speed. blinkMask == 0 -> all segments.
inline void requestPattern(PatternId pat, uint16_t speedMs, uint16_t timeoutS, uint16_t blinkMask,
                           uint16_t blinkCount, uint32_t now) {
  startRequest(MODE_PATTERN, pat, 0, timeoutS, now);
  req.speedMs = speedMs;
  if (pat == PAT_BLINK) {
    req.blinkMask = blinkMask ? (blinkMask & Display::ALL_MASK) : Display::ALL_MASK;
    req.blinkCount = blinkCount ? blinkCount : BLINK_COUNT_DEFAULT;
    // Unless the caller gave an explicit timeout, make sure the timeout does
    // not cut the requested number of blinks short.
    if (timeoutS == 0) {
      uint32_t need = (uint32_t)req.blinkCount * 2UL * intervalFor(PAT_BLINK, speedMs) + 1000UL;
      if (req.timeoutMs < need) req.timeoutMs = need;
    }
  }
  render(now);
}

// Ends the active request (if any); the display returns to countdown / OFF.
inline void stop(uint32_t now) {
  endRequest("api_stop");
  render(now);
}

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------
inline void begin() {
  memset(&req, 0, sizeof(req));
  req.mode = MODE_IDLE;
  req.pattern = PAT_NONE;
  overlay = OV_NONE;
  lastEndReason = "begin";
}

inline bool active() { return req.active; }
inline bool busy() { return req.active || overlay != OV_NONE; }

inline const char* modeName() {
  if (overlay == OV_LOADING) return "loading";
  if (overlay == OV_FLASH) return "connect_flash";
  if (req.active) {
    switch (req.mode) {
      case MODE_SEGMENTS: return "segments";
      case MODE_NUMBER: return "number";
      case MODE_ALL: return "all";
      case MODE_PATTERN: return "pattern";
      default: break;
    }
  }
  return Countdown::active() ? "countdown" : "idle";
}

inline const char* patternName() {
  if (overlay == OV_LOADING) return "figure8";
  if (req.active && req.mode == MODE_PATTERN) {
    switch (req.pattern) {
      case PAT_FLASH: return "flash";
      case PAT_COUNT: return "count";
      case PAT_FIGURE8: return "figure8";
      case PAT_BLINK: return "blink";
      default: break;
    }
  }
  return "none";
}

inline uint32_t remainingMs(uint32_t now) {
  if (!req.active) return 0;
  uint32_t elapsed = now - req.startedMs;
  return elapsed >= req.timeoutMs ? 0 : req.timeoutMs - elapsed;
}

// Accepts: flash, count, figure8 (figure-eight / figure_eight / fig8), blink
inline PatternId parsePattern(const char* s) {
  if (!s) return PAT_NONE;
  if (!strcasecmp(s, "flash")) return PAT_FLASH;
  if (!strcasecmp(s, "count") || !strcasecmp(s, "counting")) return PAT_COUNT;
  if (!strcasecmp(s, "figure8") || !strcasecmp(s, "figure-eight") || !strcasecmp(s, "figure_eight") ||
      !strcasecmp(s, "fig8"))
    return PAT_FIGURE8;
  if (!strcasecmp(s, "blink")) return PAT_BLINK;
  return PAT_NONE;
}

}  // namespace Req
