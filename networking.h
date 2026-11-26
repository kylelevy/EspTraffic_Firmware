// ============================================================================
//  networking.h - non-blocking Wi-Fi state machine, mDNS
//
//  States:  CONNECTING -> CONNECTED -> (link lost) -> CONNECTING ...
//           CONNECTING -> (timeout) -> RETRY_WAIT -> CONNECTING ...
//  Nothing here blocks; call Net::update() from loop().
//
//  Changing credentials from the web UI is a *trial*: the new network is
//  tried first (WIFI_TRIAL_TIMEOUT_MS). Only when it succeeds are the new
//  credentials written to NVS. If it fails, the device reverts to the
//  previously saved network, so a typo can never lock the device out.
// ============================================================================
#pragma once

#include <Arduino.h>
#include <ESPmDNS.h>
#include <WiFi.h>

#include "config.h"
#include "power.h"
#include "storage.h"

namespace Net {

enum State : uint8_t { NS_CONNECTING, NS_CONNECTED, NS_RETRY_WAIT };
enum ChangeStatus : uint8_t { CHG_NONE, CHG_PENDING, CHG_SUCCESS, CHG_FAILED };

static State state = NS_RETRY_WAIT;
static uint32_t stateSinceMs = 0;
static uint32_t retryAtMs = 0;
static bool eventConnected = false;

// mDNS
static bool mdnsActive = false;
static uint32_t lastMdnsTryMs = 0;

// Credential change (trial) bookkeeping
static char trialSsid[33];
static char trialPass[64];
static bool trialPending = false;  // waiting for the HTTP reply to flush
static bool trialActive = false;   // connection attempt with new credentials running
static uint32_t trialApplyAtMs = 0;
static ChangeStatus changeStatus = CHG_NONE;
static uint16_t changeSeq = 0;
static char changeMsg[112];

// WiFi.begin() is deferred until the old link is reported down.
static const char* nextSsid = nullptr;
static const char* nextPass = nullptr;
static bool beginPending = false;

inline bool isConnected() { return state == NS_CONNECTED; }

inline const char* stateName() {
  switch (state) {
    case NS_CONNECTING: return "connecting";
    case NS_CONNECTED: return "connected";
    default: return "retry_wait";
  }
}

inline const char* changeStatusName() {
  switch (changeStatus) {
    case CHG_PENDING: return "pending";
    case CHG_SUCCESS: return "success";
    case CHG_FAILED: return "failed";
    default: return "none";
  }
}

inline void setChange(ChangeStatus st, const char* fmt, const char* arg) {
  changeStatus = st;
  changeSeq++;
  snprintf(changeMsg, sizeof(changeMsg), fmt, arg);
}

inline void startAttempt(const char* ssid, const char* pass, uint32_t now) {
  WiFi.disconnect(false, false);
  nextSsid = ssid;
  nextPass = pass;
  beginPending = true;
  state = NS_CONNECTING;
  stateSinceMs = now;
}

inline void ipString(char* out, size_t cap) {
  IPAddress ip = WiFi.localIP();
  snprintf(out, cap, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
}

inline void restartMdns() {
  MDNS.end();
  mdnsActive = MDNS.begin(DEVICE_HOSTNAME);
  if (mdnsActive) MDNS.addService("http", "tcp", HTTP_PORT);
}

inline void begin() {
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);  // reconnection is handled by update()
  WiFi.setHostname(DEVICE_HOSTNAME);
  WiFi.mode(WIFI_STA);
  startAttempt(settings.ssid, settings.pass, millis());
}

// Queue a credential change. The attempt starts ~600 ms later so the HTTP
// reply can be delivered first.
inline void scheduleCredentialChange(const char* ssid, const char* pass) {
  strncpy(trialSsid, ssid, sizeof(trialSsid) - 1);
  trialSsid[sizeof(trialSsid) - 1] = 0;
  strncpy(trialPass, pass, sizeof(trialPass) - 1);
  trialPass[sizeof(trialPass) - 1] = 0;
  trialActive = false;  // an older trial must not save these untested values
  trialPending = true;
  trialApplyAtMs = millis() + 600UL;
  setChange(CHG_PENDING, "Connecting to \"%s\"...", trialSsid);
}

inline void update(uint32_t now) {
  if (trialPending && (int32_t)(now - trialApplyAtMs) >= 0) {
    trialPending = false;
    trialActive = true;
    startAttempt(trialSsid, trialPass, now);
  }

  switch (state) {
    case NS_CONNECTING: {
      if (beginPending && WiFi.status() != WL_CONNECTED) {
        beginPending = false;
        stateSinceMs = now;
        WiFi.begin(nextSsid, (nextPass && nextPass[0]) ? nextPass : nullptr);
        break;
      }
      if (!beginPending && WiFi.status() == WL_CONNECTED) {
        state = NS_CONNECTED;
        stateSinceMs = now;
        eventConnected = true;
        if (trialActive) {
          trialActive = false;
          strncpy(settings.ssid, trialSsid, sizeof(settings.ssid) - 1);
          settings.ssid[sizeof(settings.ssid) - 1] = 0;
          strncpy(settings.pass, trialPass, sizeof(settings.pass) - 1);
          settings.pass[sizeof(settings.pass) - 1] = 0;
          Storage::saveWifi();
          setChange(CHG_SUCCESS, "Connected to \"%s\". Credentials saved.", settings.ssid);
        }
        break;
      }
      uint32_t limit = trialActive ? WIFI_TRIAL_TIMEOUT_MS : WIFI_CONNECT_TIMEOUT_MS;
      if ((uint32_t)(now - stateSinceMs) >= limit) {
        WiFi.disconnect(false, false);
        if (trialActive) {
          trialActive = false;
          setChange(CHG_FAILED, "Could not connect to \"%s\". Reverted to the previous network.", trialSsid);
          startAttempt(settings.ssid, settings.pass, now);
        } else {
          state = NS_RETRY_WAIT;
          stateSinceMs = now;
          retryAtMs = now + WIFI_RETRY_DELAY_MS;
        }
      }
      break;
    }

    case NS_RETRY_WAIT:
      if ((int32_t)(now - retryAtMs) >= 0) startAttempt(settings.ssid, settings.pass, now);
      break;

    case NS_CONNECTED:
      if (WiFi.status() != WL_CONNECTED) {
        mdnsActive = false;
        startAttempt(settings.ssid, settings.pass, now);  // automatic reconnect
      }
      break;
  }

  // Keep mDNS alive while connected
  if (state == NS_CONNECTED && !mdnsActive && (uint32_t)(now - lastMdnsTryMs) >= MDNS_RETRY_MS) {
    lastMdnsTryMs = now;
    restartMdns();
  }
}

// True once per (re)connection.
inline bool consumeConnectedEvent() {
  bool e = eventConnected;
  eventConnected = false;
  return e;
}

}  // namespace Net
