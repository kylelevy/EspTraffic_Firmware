// ============================================================================
//  ESP32 Seven-Segment Display Controller
//
//  Two seven-segment digits, 14 dedicated GPIOs, Wi-Fi + browser control,
//  OTA updates, display patterns and an event countdown.
//
//  Architecture (all non-blocking, driven from loop()):
//    Net      Wi-Fi connection state machine, mDNS          (networking.h)
//    Clock    NTP / timezone, Countdown days calculation     (countdown.h)
//    Req      display request manager + pattern engine       (patterns.h)
//    Display  batched GPIO register output                   (display.h)
//    Web      HTTP server, REST API, OTA upload              (webserver.h)
//    Power    modem sleep + CPU frequency scaling            (power.h)
//    Storage  validated NVS settings                         (storage.h)
//
//  The serial port is deliberately NOT used: GPIO1/GPIO3 (UART0) drive
//  display segments. Debug through the browser (/api/info, /api/state).
// ============================================================================
#include <Arduino.h>
#include <WiFi.h>
#include <esp_log.h>

#include "config.h"
#include "countdown.h"
#include "display.h"
#include "networking.h"
#include "patterns.h"
#include "power.h"
#include "storage.h"
#include "web_ui.h"
#include "webserver.h"

static uint32_t lastRefreshMs = 0;

void setup() {
  // Stage 1 - hardware: outputs LOW immediately, then configured as outputs.
  Display::begin();
  esp_log_level_set("*", ESP_LOG_NONE);  // nothing may be printed on UART0

  Storage::load();
  Clock::applyTimezone(settings.tz);
  Countdown::invalidate();

  // Startup flash: every segment ON for 500 ms, then OFF.
  // This is the only blocking delay in the firmware (setup only, before any
  // networking, and far shorter than any request timeout).
  Display::setDisplayState(Display::ALL_MASK);
  delay(STARTUP_FLASH_MS);
  Display::setDisplayState(0);

  Req::begin();
  Web::begin();   // register HTTP routes (listening starts after Wi-Fi is up)

  // Stage 2 - Wi-Fi: non-blocking; the loading animation runs from loop().
  delay(3000);
  Net::begin();
  Power::apply();
}

void loop() {
  uint32_t now = millis();

  // 1. Wi-Fi state machine
  Net::update(now);

  // 2. Wi-Fi events -> overlays and service (re)initialisation
  if (Net::consumeConnectedEvent()) {
    // Stage 3 - connected: one flash (non-blocking), then OFF/normal.
    Req::setLoading(false, now);
    Req::startConnectFlash(now);
    Net::restartMdns();       // mDNS is restarted on every (re)connection
    Clock::startNtp(now);     // NTP sync (also re-applies the timezone)
    Web::start();             // (re)start the HTTP listener
    Power::apply();
  } else {
    Req::setLoading(!Net::isConnected(), now);  // figure-eight while not connected
  }

  // 3. HTTP requests (never blocked by animations)
  Web::handle();

  // 4. Clock + countdown
  Clock::update(now, Net::isConnected());
  Countdown::update(now);

  // 5. Display requests, timeouts and pattern stepping
  // Use a fresh timestamp because HTTP handlers may have started
  // a request after the timestamp captured at the top of loop().
  Req::update(millis());

  // 6. Periodically re-assert the outputs (cheap register writes)
  if ((uint32_t)(now - lastRefreshMs) >= DISPLAY_REFRESH_MS) {
    lastRefreshMs = now;
    Display::refresh();
  }

  // 7. Power management, deferred restart after OTA
  bool busy = Req::busy();
  Power::update(millis(), busy);  // wake() may be newer than "now"
  Web::serviceRestart(now);

  delay(Power::loopDelayMs(busy));  // 1 ms normally, 5 ms when power saving and idle
}
