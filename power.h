// ============================================================================
//  power.h - light power saving (NOT deep sleep)
//
//  Enabled:  Wi-Fi modem sleep (WIFI_PS_MIN_MODEM: the radio sleeps between
//            beacons but wakes for every DTIM, so multicast mDNS keeps
//            working) and the CPU drops to CPU_IDLE_MHZ after POWER_IDLE_MS of
//            inactivity. Any display request or HTTP request wakes it again.
//  Disabled: Wi-Fi power save off (lowest latency), CPU stays at CPU_ACTIVE_MHZ.
//  OTA:      power saving is suspended for the duration of an upload.
// ============================================================================
#pragma once

#include <Arduino.h>
#include <WiFi.h>

#include "config.h"
#include "storage.h"

namespace Power {

static bool otaActive = false;
static bool lowCpu = false;
static uint32_t lastActivityMs = 0;

inline void wake() {
  lastActivityMs = millis();
  if (lowCpu) {
    setCpuFrequencyMhz(CPU_ACTIVE_MHZ);
    lowCpu = false;
  }
}

inline void applyWifiSleep() {
  if (otaActive || !settings.powerSave) {
    WiFi.setSleep(WIFI_PS_NONE);
  } else {
    WiFi.setSleep(WIFI_PS_MIN_MODEM);
  }
}

// Apply the current preference (call after boot, reconnect and settings changes).
inline void apply() {
  applyWifiSleep();
  if (!settings.powerSave) wake();
}

inline void beginOta() {
  otaActive = true;
  wake();
  applyWifiSleep();
}

inline void endOta() {
  otaActive = false;
  applyWifiSleep();
  wake();
}

// busy = a display request or overlay is active
inline void update(uint32_t now, bool busy) {
  if (!settings.powerSave || otaActive) {
    if (lowCpu) wake();
    return;
  }
  if (busy) {
    lastActivityMs = now;
    if (lowCpu) wake();
    return;
  }
  if (!lowCpu && (uint32_t)(now - lastActivityMs) >= POWER_IDLE_MS) {
    setCpuFrequencyMhz(CPU_IDLE_MHZ);
    lowCpu = true;
  }
}

inline uint32_t loopDelayMs(bool busy) {
  return (settings.powerSave && !busy && !otaActive) ? 5 : 1;
}

}  // namespace Power
