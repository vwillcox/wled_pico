#pragma once

#include <cstdint>

// A small subset of FastLED/WLED's 8/16-bit math and PRNG helpers - almost
// every WLED mode_ function calls at least one of these (random8/sin8/
// beatsin8 especially). Not bit-exact against FastLED's actual lookup
// tables/PRNG algorithm - same kind of intentional simplification this
// codebase already documents for sin16_t/breathe() in effects.cpp - these
// match the *shape* (same output range, same rough curve/distribution),
// not bit-for-bit reproducibility, which no ported effect here depends on.
namespace effects {

// RP2040's real hardware RNG (rp2040.hwrand32(), arduino-pico) backs these
// - no manual seeding needed, unlike FastLED's software PRNG.
uint8_t random8();
uint8_t random8(uint8_t max);               // [0, max)
uint8_t random8(uint8_t min, uint8_t max);  // [min, max)
uint16_t random16();
uint16_t random16(uint16_t max);
uint16_t random16(uint16_t min, uint16_t max);

// One sine/cosine period over a full 8-bit or 16-bit input range.
uint8_t sin8(uint8_t theta);   // 0-255
uint8_t cos8(uint8_t theta);   // 0-255
int16_t sin16(uint16_t theta);  // roughly -32767..32767
int16_t cos16(uint16_t theta);  // roughly -32767..32767

// WLED's beatsin8()/beatsin16(): a `bpm`-beats-per-minute sine wave
// oscillating between low/high, phase-shiftable. WLED keys this off
// strip.now; here it's an explicit `now_ms` parameter (this firmware's
// equivalent, passed through from render()'s now_ms same as everywhere
// else).
uint8_t beatsin8(uint32_t now_ms, uint8_t bpm, uint8_t low = 0, uint8_t high = 255,
                  uint16_t phase_offset = 0);
uint16_t beatsin16(uint32_t now_ms, uint8_t bpm, uint16_t low = 0, uint16_t high = 65535,
                    uint16_t phase_offset = 0);

inline uint8_t qadd8(uint8_t a, uint8_t b) {
  unsigned s = static_cast<unsigned>(a) + b;
  return s > 255 ? 255 : static_cast<uint8_t>(s);
}
inline uint8_t qsub8(uint8_t a, uint8_t b) { return a > b ? a - b : 0; }
// FastLED's scale8(): i scaled by scale/256.
inline uint8_t scale8(uint8_t i, uint8_t scale) {
  return static_cast<uint8_t>((static_cast<unsigned>(i) * (1u + scale)) >> 8);
}

}  // namespace effects
