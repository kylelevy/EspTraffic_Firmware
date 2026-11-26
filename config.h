// ============================================================================
//  ESP32 Seven-Segment Display Controller - configuration
//  Everything a user normally needs to edit lives in this file.
// ============================================================================

// ---- 1. Wi-Fi defaults ------------------------------------------------------
// Used on first boot only. Credentials saved through the web interface are
// stored in NVS and take priority over these values afterwards.
#define WIFI_SSID "YOUR_SSID"
#define WIFI_PASSWORD "YOUR_PASSWORD"

#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// ---- 2. Identity ------------------------------------------------------------
#define DEVICE_HOSTNAME "espTraffic"      // reachable as http://espTraffic.local
#define FIRMWARE_VERSION "1.0.0"
#define HTTP_PORT 80

// ---- 3. GPIO mapping --------------------------------------------------------
// DIGIT_PINS[digit][segment]  digit: 0 = LEFT (tens), 1 = RIGHT (units)
//                             segment: 0..6 = A, B, C, D, E, F, G
// Output logic is ACTIVE HIGH: GPIO HIGH = segment ON, LOW = OFF.
// (Common-anode LEDs are switched by Darlington arrays, which invert the
//  logic in hardware, so the firmware always treats HIGH as "on".)
//
//   Segment | Left   | Right
//   --------+--------+-------
//      A    | GPIO26 | GPIO19
//      B    | GPIO4  | GPIO1   <- UART0 TX pin
//      C    | GPIO17 | GPIO22
//      D    | GPIO33 | GPIO3   <- UART0 RX pin
//      E    | GPIO16 | GPIO23
//      F    | GPIO25 | GPIO32
//      G    | GPIO18 | GPIO21
static const uint8_t DIGIT_PINS[2][7] = {
    /* Left  */ {26, 4, 17, 33, 16, 25, 18},
    /* Right */ {19, 1, 22, 3, 23, 32, 21}};

// ---- 4. Request / animation defaults (all editable from the web UI) ----------
#define DEFAULT_TIMEOUT_S 10      // temporary display requests expire after this
#define MIN_TIMEOUT_S 1
#define MAX_TIMEOUT_S 3600

#define DEFAULT_PATTERN_MS 150    // generic animation step; used by "blink" (half period)
#define DEFAULT_COUNT_MS 400      // repeated-digit counting step
#define DEFAULT_FIG8_MS 150       // figure-eight step
#define DEFAULT_FLASH_MS 300      // all-flash half period
#define MIN_ANIM_MS 30
#define MAX_ANIM_MS 5000

#define BLINK_COUNT_DEFAULT 3
#define BLINK_COUNT_MAX 100
#define FIG8_TRAIL_LENGTH 3       // lit segments in the moving trail

// ---- 5. Time / countdown -----------------------------------------------------
// POSIX TZ string. Default: Toronto (EST/EDT with North American DST rules).
#define DEFAULT_TZ "EST5EDT,M3.2.0,M11.1.0"
#define NTP_SERVER_1 "pool.ntp.org"
#define NTP_SERVER_2 "time.google.com"
#define NTP_SERVER_3 "time.cloudflare.com"
#define MIN_VALID_EPOCH 1735689600UL  // 2025-01-01: anything earlier = clock not synced
#define NTP_RETRY_MS 30000UL

// ---- 6. Power saving (light, Wi-Fi stays up; NOT deep sleep) -----------------
#define DEFAULT_POWER_SAVE false
#define CPU_ACTIVE_MHZ 240
#define CPU_IDLE_MHZ 80
#define POWER_IDLE_MS 10000UL     // idle time before the CPU is slowed down

// ---- 7. Authentication (HTTP Basic) -------------------------------------------
// DISABLED by default. Intended for a trusted private network only.
// When enabled it protects EVERYTHING: the page, display commands, settings and OTA.
#define AUTH_ENABLED 0
#define AUTH_USERNAME "admin"
#define AUTH_PASSWORD "change-me"

// ---- 8. Timing / limits (rarely changed) --------------------------------------
#define STARTUP_FLASH_MS 500
#define CONNECT_FLASH_MS 500
#define WIFI_CONNECT_TIMEOUT_MS 15000UL
#define WIFI_RETRY_DELAY_MS 3000UL
#define WIFI_TRIAL_TIMEOUT_MS 20000UL   // when testing newly entered credentials
#define MDNS_RETRY_MS 10000UL
#define DISPLAY_REFRESH_MS 100UL        // outputs are re-asserted at this interval
#define MAX_BODY_BYTES 1024
#define MIN_HEAP_FOR_OTA 30000UL
#define UPDATE_RESTART_DELAY_MS 1500UL

#endif  // CONFIG_H
