#include "wled_compat.h"

#include <Arduino.h>
#include <math.h>

namespace effects {

uint8_t random8() { return static_cast<uint8_t>(rp2040.hwrand32()); }
uint8_t random8(uint8_t max) { return max ? static_cast<uint8_t>(rp2040.hwrand32() % max) : 0; }
uint8_t random8(uint8_t min, uint8_t max) { return max > min ? static_cast<uint8_t>(min + random8(static_cast<uint8_t>(max - min))) : min; }
uint16_t random16() { return static_cast<uint16_t>(rp2040.hwrand32()); }
uint16_t random16(uint16_t max) { return max ? static_cast<uint16_t>(rp2040.hwrand32() % max) : 0; }
uint16_t random16(uint16_t min, uint16_t max) { return max > min ? static_cast<uint16_t>(min + random16(static_cast<uint16_t>(max - min))) : min; }

uint8_t sin8(uint8_t theta) {
  float rad = theta * (2.0f * static_cast<float>(M_PI) / 256.0f);
  return static_cast<uint8_t>(128.0f + 127.0f * sinf(rad));
}
uint8_t cos8(uint8_t theta) { return sin8(static_cast<uint8_t>(theta + 64)); }

int16_t sin16(uint16_t theta) {
  float rad = theta * (2.0f * static_cast<float>(M_PI) / 65536.0f);
  return static_cast<int16_t>(32767.0f * sinf(rad));
}
int16_t cos16(uint16_t theta) { return sin16(static_cast<uint16_t>(theta + 16384)); }

namespace {
// One full beat cycle (0-65535) at `bpm` beats/minute, `now_ms`-driven.
// FastLED's beat16() does the same job via fixed-point bit shifts (exact
// constants not replicated here - see this file's header comment on the
// "shape, not bit-exact" tolerance every helper here shares).
uint16_t beat16(uint32_t now_ms, uint8_t bpm, uint16_t phase_offset) {
  uint32_t period_ms = 60000u / (bpm ? bpm : 1);
  uint64_t pos = (static_cast<uint64_t>(now_ms % period_ms) * 65536ull) / period_ms;
  return static_cast<uint16_t>(pos) + phase_offset;
}
}  // namespace

uint8_t beatsin8(uint32_t now_ms, uint8_t bpm, uint8_t low, uint8_t high, uint16_t phase_offset) {
  uint8_t s = sin8(static_cast<uint8_t>(beat16(now_ms, bpm, phase_offset) >> 8));
  return static_cast<uint8_t>(low + scale8(s, static_cast<uint8_t>(high - low)));
}

uint16_t beatsin16(uint32_t now_ms, uint8_t bpm, uint16_t low, uint16_t high, uint16_t phase_offset) {
  int16_t s = sin16(beat16(now_ms, bpm, phase_offset));
  uint16_t su = static_cast<uint16_t>(s + 32768);
  uint32_t range = static_cast<uint32_t>(high) - low;
  return static_cast<uint16_t>(low + (static_cast<uint32_t>(su) * range) / 65535);
}

}  // namespace effects
