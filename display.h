// ============================================================================
//  display.h - low-level seven-segment output (14 dedicated GPIOs)
//
//  Segment bit layout used everywhere in the firmware ("segment mask"):
//
//      bit 0..6  = LEFT  digit  A B C D E F G
//      bit 7..13 = RIGHT digit  A B C D E F G
//
//  How the batched GPIO update works
//  ---------------------------------
//  The ESP32 has two GPIO register banks: GPIO0-31 (bank 0) and GPIO32-39
//  (bank 1). Each bank has a write-one-to-set (W1TS) and write-one-to-clear
//  (W1TC) register; writing a 1 to a bit changes ONLY that pin and leaves
//  every other pin untouched, so no read-modify-write is needed.
//
//  setDisplayState(mask):
//    1. walk the 14 segment bits and build the "ON" pin masks for each bank,
//    2. derive the "OFF" masks (all display pins of the bank minus the ON ones),
//    3. write W1TC then W1TS once per bank  -> at most 4 register writes
//       in total, instead of 14 digitalWrite() calls.
//
//  A single instruction cannot update both banks, so the two banks change a
//  few nanoseconds apart - invisible on LEDs.
//
//  If the selected core does not expose the register macros, a portable
//  digitalWrite() fallback is compiled instead (see DISPLAY_USE_REGISTERS).
// ============================================================================
#pragma once

#include <Arduino.h>
#include <ctype.h>
#include <string.h>

#include "config.h"

#if __has_include("soc/gpio_reg.h")
#include "soc/gpio_reg.h"
#include "soc/gpio_sig_map.h"
#include "esp_rom_gpio.h"
#include "soc/soc.h"
#define DISPLAY_USE_REGISTERS 1
#else
#define DISPLAY_USE_REGISTERS 0
#endif

namespace Display {

static const uint8_t SEGS_PER_DIGIT = 7;
static const uint8_t SEG_TOTAL = 14;
static const uint16_t ALL_MASK = 0x3FFF;
static const uint16_t LEFT_MASK = 0x007F;
static const uint16_t RIGHT_MASK = 0x3F80;

// Bit 0 = A ... bit 6 = G
static const uint8_t DIGIT_GLYPH[10] = {
    0x3F,  // 0: A B C D E F
    0x06,  // 1: B C
    0x5B,  // 2: A B D E G
    0x4F,  // 3: A B C D G
    0x66,  // 4: B C F G
    0x6D,  // 5: A C D F G
    0x7D,  // 6: A C D E F G
    0x07,  // 7: A B C
    0x7F,  // 8: all
    0x6F   // 9: A B C D F G
};

// Per-segment register masks, built once in begin()
static uint32_t bank0Bit[SEG_TOTAL];
static uint32_t bank1Bit[SEG_TOTAL];
static uint32_t bank0All = 0;
static uint32_t bank1All = 0;

static uint16_t desiredMask = 0;  // what the firmware last asked the pins to be
static uint16_t writtenMask = 0;

inline uint8_t pinForBit(uint8_t bit) {
  return DIGIT_PINS[bit / SEGS_PER_DIGIT][bit % SEGS_PER_DIGIT];
}

// ---------------------------------------------------------------------------
// Raw output
// ---------------------------------------------------------------------------
inline void setDisplayState(uint16_t mask) {
  mask &= ALL_MASK;
#if DISPLAY_USE_REGISTERS
  uint32_t on0 = 0, on1 = 0;
  for (uint8_t i = 0; i < SEG_TOTAL; i++) {
    if (mask & (1u << i)) {
      on0 |= bank0Bit[i];
      on1 |= bank1Bit[i];
    }
  }
  REG_WRITE(GPIO_OUT_W1TC_REG, bank0All & ~on0);
  REG_WRITE(GPIO_OUT1_W1TC_REG, bank1All & ~on1);
  REG_WRITE(GPIO_OUT_W1TS_REG, on0);
  REG_WRITE(GPIO_OUT1_W1TS_REG, on1);
#else
  for (uint8_t i = 0; i < SEG_TOTAL; i++) {
    digitalWrite(pinForBit(i), (mask & (1u << i)) ? HIGH : LOW);
  }
#endif
  desiredMask = mask;
  writtenMask = mask;
}

// Only touch the hardware when the requested mask differs from the last write.
inline void showMask(uint16_t mask) {
  mask &= ALL_MASK;
  if (mask != writtenMask) setDisplayState(mask);
}

// Re-assert the desired outputs (cheap). Keeps the display consistent even if
// something momentarily disturbed a pin (e.g. activity on the UART0 bridge).
inline void refresh() { setDisplayState(desiredMask); }

// Read the REAL pad levels of the 14 outputs (outputs are configured as
// INPUT_OUTPUT by pinMode(OUTPUT), so the input path is enabled).
inline uint16_t readActualMask() {
  uint16_t m = 0;
#if DISPLAY_USE_REGISTERS
  uint32_t in0 = REG_READ(GPIO_IN_REG);
  uint32_t in1 = REG_READ(GPIO_IN1_REG);
  for (uint8_t i = 0; i < SEG_TOTAL; i++) {
    if ((in0 & bank0Bit[i]) || (in1 & bank1Bit[i])) m |= (1u << i);
  }
#else
  for (uint8_t i = 0; i < SEG_TOTAL; i++) {
    if (digitalRead(pinForBit(i))) m |= (1u << i);
  }
#endif
  return m;
}

// ---------------------------------------------------------------------------
// Initialisation: call this FIRST in setup().
//   1. build register masks
//   2. force the output latches LOW *before* enabling the outputs
//   3. configure the 14 pins as outputs
//   4. make sure GPIO1/GPIO3 are driven by the GPIO matrix (not UART0)
// ---------------------------------------------------------------------------
inline void begin() {
  bank0All = 0;
  bank1All = 0;
  for (uint8_t i = 0; i < SEG_TOTAL; i++) {
    uint8_t pin = pinForBit(i);
    bank0Bit[i] = (pin < 32) ? (1UL << pin) : 0;
    bank1Bit[i] = (pin >= 32) ? (1UL << (pin - 32)) : 0;
    bank0All |= bank0Bit[i];
    bank1All |= bank1Bit[i];
  }
#if DISPLAY_USE_REGISTERS
  REG_WRITE(GPIO_OUT_W1TC_REG, bank0All);
  REG_WRITE(GPIO_OUT1_W1TC_REG, bank1All);
#endif
  for (uint8_t i = 0; i < SEG_TOTAL; i++) {
    uint8_t pin = pinForBit(i);
#if !DISPLAY_USE_REGISTERS
    digitalWrite(pin, LOW);
#endif
    pinMode(pin, OUTPUT);
#if DISPLAY_USE_REGISTERS
    esp_rom_gpio_connect_out_signal(pin, SIG_GPIO_OUT_IDX, false, false);
#endif
  }
  setDisplayState(0);
}

// ---------------------------------------------------------------------------
// Number / glyph helpers
// ---------------------------------------------------------------------------
inline uint16_t numberMask(uint8_t n) {
  if (n > 99) n = 99;
  return (uint16_t)DIGIT_GLYPH[n / 10] | ((uint16_t)DIGIT_GLYPH[n % 10] << SEGS_PER_DIGIT);
}

// Returns 0..99 if both digits show a valid numeral, otherwise -1.
inline int decodeNumber(uint16_t mask) {
  uint8_t left = mask & 0x7F;
  uint8_t right = (mask >> SEGS_PER_DIGIT) & 0x7F;
  int tens = -1, units = -1;
  for (int d = 0; d < 10; d++) {
    if (DIGIT_GLYPH[d] == left) tens = d;
    if (DIGIT_GLYPH[d] == right) units = d;
  }
  if (tens < 0 || units < 0) return -1;
  return tens * 10 + units;
}

// ---------------------------------------------------------------------------
// Segment names: "left.A" .. "right.G", plus "left", "right", "all"
// ---------------------------------------------------------------------------
inline bool parseToken(const char* t, uint16_t& m) {
  char b[16];
  size_t n = strlen(t);
  if (n == 0 || n >= sizeof(b)) return false;
  for (size_t i = 0; i < n; i++) b[i] = (char)tolower((unsigned char)t[i]);
  b[n] = 0;
  if (!strcmp(b, "all")) { m = ALL_MASK; return true; }
  if (!strcmp(b, "left")) { m = LEFT_MASK; return true; }
  if (!strcmp(b, "right")) { m = RIGHT_MASK; return true; }
  uint8_t digit;
  const char* seg;
  if (!strncmp(b, "left.", 5)) { digit = 0; seg = b + 5; }
  else if (!strncmp(b, "right.", 6)) { digit = 1; seg = b + 6; }
  else return false;
  if (strlen(seg) != 1 || seg[0] < 'a' || seg[0] > 'g') return false;
  m = (uint16_t)(1u << (digit * SEGS_PER_DIGIT + (seg[0] - 'a')));
  return true;
}

inline void segmentName(uint8_t bit, char* out, size_t cap) {
  snprintf(out, cap, "%s.%c", bit < SEGS_PER_DIGIT ? "left" : "right", 'A' + (bit % SEGS_PER_DIGIT));
}

}  // namespace Display
