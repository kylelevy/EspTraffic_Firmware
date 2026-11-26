# ESP32 Seven-Segment Display Controller

Firmware for a two-digit, seven-segment display driven by an ESP32 DevKit (ESP32-WROOM-32).
Control it from any browser over Wi-Fi at **http://espTraffic.local**: set segments or numbers,
run display patterns, show an event countdown, and update the firmware over the air.

> **Status of testing.** The source has been syntax-checked against API stubs and its logic
> (GPIO mask maths, number glyphs, pattern timing, request interruption/timeout, JSON parser,
> countdown rounding) was unit-tested on a PC. It has **not** been compiled with the real
> PlatformIO toolchain or run on real hardware by the author. Work through
> [`docs/TESTING.md`](docs/TESTING.md) on your board before relying on it.

## 1. Required hardware

- ESP32 DevKit with an **ESP32-WROOM-32** module (4 MB flash). **WROVER boards are not supported
  with this pin map** (GPIO16/17 are used for PSRAM there).
- Two seven-segment digits, common-anode, one dedicated GPIO per segment (14 GPIOs, no multiplexing),
  switched through Darlington transistor arrays. **GPIO HIGH = segment ON.**
- 2.4 GHz Wi-Fi network with DHCP.

## 2. GPIO mapping

Defined once in `include/config.h` (`DIGIT_PINS[digit][segment]`); nothing else in the code names a pin.

| Segment | Left digit (tens) | Right digit (units) |
|---|---|---|
| A | GPIO26 | GPIO19 |
| B | GPIO4  | GPIO1 (UART0 TX) |
| C | GPIO17 | GPIO22 |
| D | GPIO33 | GPIO3 (UART0 RX) |
| E | GPIO16 | GPIO23 |
| F | GPIO25 | GPIO32 |
| G | GPIO18 | GPIO21 |

## 3. Hardware caveats

1. **GPIO1 and GPIO3 are the USB-to-UART programming pins.** The firmware never uses `Serial`
   (all debugging goes through the browser), but the physical USB-UART bridge stays connected to those pins.
   - While the **USB cable is plugged in and a serial monitor/terminal is open**, the bridge can drive GPIO3
     and fight the output driving *Right D*. Keep serial monitors closed during normal operation.
   - During flashing and at reset, the boot ROM prints on GPIO1/UART0, so *Right B* may flicker briefly at
     power-up/reset **before** the firmware starts. This is expected and accepted. As soon as `setup()` runs,
     the firmware latches all 14 outputs LOW, reconnects GPIO1/3 to the GPIO matrix, and re-asserts the
     outputs every 100 ms (`DISPLAY_REFRESH_MS`).
   - The re-assert cannot cure an *electrical* conflict (two drivers on one wire). If you see ghosting on Right D
     while USB is attached, check for a series resistor between the bridge and GPIO3 or run the board from
     a separate 5 V supply without the serial port open.
   - Flashing over USB can briefly light segments (the bridge toggles DTR/RTS and the UART lines). Harmless.
2. **GPIO16/17** are free on WROOM but reserved on WROVER. If your module is a WROVER, do not use this map as is.
3. GPIO1/3 being outputs does not stop you flashing over USB. OTA via the browser works regardless.
4. Each segment output only needs to source the Darlington input current; check your array's input resistor.
5. The ESP32 needs reliable 5 V. 14 lit LEDs through Darlingtons are fed from the LED supply, not the ESP32.

## 4. Required software

- [PlatformIO](https://platformio.org/) (VS Code extension or CLI). No libraries to install:
  WiFi, WebServer, ESPmDNS, Preferences and Update ship with the ESP32 Arduino core.
- Pinned platform: `espressif32@6.5.0` (Arduino core 2.0.14). PlatformIO downloads it automatically.

**Arduino IDE:** the code is plain Arduino C++11. Create a sketch folder, rename `src/main.cpp` to
`<folder>.ino`, copy all `include/*.h` files and `partitions.csv` into the same folder, choose
*ESP32 Dev Module* and Tools → Partition Scheme → *Custom* (uses `partitions.csv`). PlatformIO is recommended.

## 5. Open, configure, build and flash

1. Open the `ESP32-Traffic-Controller` folder in VS Code with PlatformIO (*File → Open Folder*).
2. Edit the top of `include/config.h`:
   ```cpp
   #define WIFI_SSID "YOUR_WIFI_NAME"
   #define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
   ```
   These are used on first boot only. Credentials saved later through the web page take priority.
3. **Compile:** PlatformIO toolbar → *Build*, or `pio run`.
4. **First upload over USB:** connect the board, then *Upload* or `pio run -t upload`.
   (If the upload does not start, hold the **BOOT** button while it says "Connecting...".)
5. The compiled image for later OTA updates is `.pio/build/esp32dev/firmware.bin`.

### Startup sequence

1. All 14 outputs LOW immediately, then **every segment ON for 500 ms**, then OFF.
2. Wi-Fi connects without blocking; the **figure-eight loading animation** runs until connected
   (retries every 3 s after a 15 s attempt).
3. On connection: **one flash**, then mDNS, web server and NTP start.
4. If Wi-Fi drops the animation returns, the device reconnects by itself, flashes once and carries on.

## 6. Using the web interface

Open **http://espTraffic.local** (or the IP address shown in your router/`/api/info`).

- Click any segment in the picture to toggle it. The picture shows the **real GPIO state** read back from the pins.
- *All OFF / All ON*, a number box (0-99), the four patterns and *Stop*.
- Every temporary command expires after the configured **request timeout** (default 10 s), then the display
  returns to the countdown (if enabled) or turns off. A newer command always replaces the running one immediately.
- The page polls `/api/state` every 3 s; polling never changes the display or resets a timeout.
- Open the browser console (F12) for a table of controller diagnostics.

### espTraffic.local troubleshooting

mDNS works out of the box on macOS, iOS, Linux (avahi) and current Windows 10/11. On older Windows install
Apple Bonjour, or use the IP address. Some routers block multicast between Wi-Fi and wired clients.
Android browsers often cannot resolve `.local`; use the IP address.

## 7. Patterns

| Pattern | Behaviour |
|---|---|
| `flash` | all 14 segments ON/OFF together |
| `count` | 00, 11, 22 ... 99, 00 ... (both digits equal) |
| `figure8` | a 3-segment trail snakes around both digits, crossing over the two middle (G) segments. It uses the outer segments and both G bars; the four inner verticals (Left B/C, Right E/F) are not part of the path |
| `blink` | blinks chosen segments (`left.A`, `right.G`, `left`, `right`, `all`); `count` = complete ON/OFF cycles; none chosen = all. Ends by itself after the last cycle |

Speeds (30-5000 ms per step) are set in *Settings* or per request (`speed`). Blink with no explicit timeout
extends its own timeout so the requested number of blinks completes.

## 8. Changing Wi-Fi credentials

*Settings → Wi-Fi SSID / password → Save settings*. The controller **tests the new network first**:

- success → the credentials are saved to NVS and used from now on;
- failure (20 s) → it **reverts to the previous network** and reports the failure.

The result appears under the Settings card and in `/api/state` (`wifiChange`). If the new network is a different
one, your browser will lose the connection; reconnect your computer to the new network and open
`http://espTraffic.local`. Leave the password empty to keep the stored one (same SSID), or to join an *open* network
when you change the SSID. The saved password is never returned by any API.

## 9. Countdown

*Countdown card:* pick the event date, tick **Enable countdown**, **Save**. Stored in NVS; survives reboots and OTA.

- Needs NTP time (shown as *clock synced*). Until the clock has synced nothing is shown and the API says so.
- Remaining days are rounded **up**: 2 d 3 h → 3, 1 d 4 h → 2, under 24 h → 1, event day reached → 0.
  The event date means local midnight **at the start** of that day, so the display reads `00` on the event day.
- More than 99 days shows `99`; the API/website still report the true number.
- Timezone: POSIX string in *Settings* (default Toronto `EST5EDT,M3.2.0,M11.1.0`).
- Temporary requests override the countdown; it returns when they expire. Saving countdown settings does
  not interrupt a running request.

## 10. OTA firmware update

1. Build the new firmware (`pio run`) and locate `.pio/build/esp32dev/firmware.bin`.
2. *Firmware update* card → choose the `.bin` → **Upload firmware**. A progress bar shows the transfer; all
   segments stay ON while uploading. On success the device restarts after ~1.5 s and the page reloads itself.
3. Failures show a message (bad image, not enough memory, aborted upload) and the old firmware keeps running.
4. Settings, saved Wi-Fi credentials and the countdown are in NVS and **survive** the update.

Two 1.875 MB application slots (`partitions.csv`) give safe A/B updates. Use the `firmware.bin` app image, **not**
`bootloader.bin` or `partitions.bin`.
Command line: `curl -F "firmware=@.pio/build/esp32dev/firmware.bin" http://espTraffic.local/update`

## 11. Power-saving mode

Not deep sleep. Wi-Fi, mDNS and the web server stay up. When enabled: Wi-Fi modem sleep (`WIFI_PS_MIN_MODEM`,
wakes every DTIM so mDNS keeps working), CPU drops from 240 to 80 MHz after 10 s of idle, loop delay rises to 5 ms.
Any request wakes it immediately. It is suspended during OTA. Expect a few extra ms of latency on the first request.

## 12. Security

- HTTP only, designed for a **trusted private network**. With the default `AUTH_ENABLED 0` anyone on the network
  can control the display, change settings and flash firmware.
- Set `AUTH_ENABLED 1` plus `AUTH_USERNAME` / `AUTH_PASSWORD` in `config.h` for HTTP Basic authentication on
  **every** route (page, display commands, settings, OTA). Basic auth is not encrypted: it only deters casual use.
- **Do not port-forward or otherwise expose the controller to the internet.**
- The Wi-Fi password is stored unencrypted in NVS (a requirement of connecting) and is never sent over the API.

## 13. Troubleshooting

| Symptom | Check |
|---|---|
| Figure-eight never stops | wrong SSID/password in `config.h`, 5 GHz-only network, weak signal. Reflash with correct values |
| `espTraffic.local` not found | use the IP (router's client list); see mDNS notes above |
| Page loads but shows *offline* | device lost Wi-Fi; wait for reconnect animation to finish |
| Countdown blank | enabled? valid date saved? "clock synced"? (NTP needs internet access) |
| Wrong number of days | check timezone string in Settings |
| A segment never lights | wiring/Darlington channel; test with `POST /api/segments` for that segment alone |
| Right D / Right B flicker on USB | see *Hardware caveats* |
| OTA fails | upload `firmware.bin` (app image), keep >30 KB free heap, stay on the same network |
| Lost the device after a typo in Wi-Fi settings | cannot happen: new credentials are tested and reverted on failure |
| Needs a factory reset of settings | flash once with *Erase Flash* (`pio run -t erase`) |

## 14. Project layout

```
platformio.ini      board, pinned platform, partition table
partitions.csv      nvs + otadata + 2 x 1.875 MB app slots
include/config.h    GPIO map, Wi-Fi defaults, hostname, timeouts, TZ, power, auth
include/display.h   batched GPIO register output, glyphs, segment parsing
include/storage.h   validated NVS settings
include/countdown.h NTP/timezone + days-remaining
include/power.h     modem sleep, CPU scaling
include/patterns.h  request manager, timeouts, 4 patterns, loading overlay
include/networking.h non-blocking Wi-Fi state machine, mDNS, credential trial
include/webserver.h HTTP server, JSON API, OTA
include/web_ui.h    embedded HTML/CSS/JS (PROGMEM)
src/main.cpp        setup() and the non-blocking loop()
docs/API.md         REST API reference
docs/TESTING.md     hardware test checklist
```

## 15. How the GPIO update works

`setDisplayState(mask)` builds per-bank pin masks and writes the ESP32 W1TC/W1TS registers (bank 0 = GPIO0-31,
bank 1 = GPIO32-39): at most four register writes per update, touching only the 14 display pins. GPIO32/33 live
in bank 1, so two banks change a few nanoseconds apart. If the core lacks the register macros, a `digitalWrite()`
fallback is compiled automatically (`DISPLAY_USE_REGISTERS` in `display.h`).
