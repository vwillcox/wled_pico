#pragma once

#include <cstdint>

#include "effects.h"

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
//
// `phase_offset` is uint8_t here, matching real WLED's beatsin8_t()
// exactly (wled00/util.cpp: `sin8_t(beat8(bpm, timebase) + phase_offset)`)
// - it's added in the same 8-bit domain sin8() operates in, so 128 means
// exactly half a cycle (used constantly to put two waves 180 degrees
// apart, e.g. the two strands of the DNA effect). A wider type here would
// silently shrink that to a fraction of a percent of a cycle instead - a
// real bug this had until a report of DNA showing one line instead of two
// (effect id 152) traced it here. Callers passing an expression like
// `x*10` that overflows 255 get real WLED's own wraparound behavior for
// free via the implicit truncation, which several effects rely on as a
// deliberate repeating pattern - not something to "fix" further.
uint8_t beatsin8(uint32_t now_ms, uint8_t bpm, uint8_t low = 0, uint8_t high = 255,
                  uint8_t phase_offset = 0);
uint16_t beatsin16(uint32_t now_ms, uint8_t bpm, uint16_t low = 0, uint16_t high = 65535,
                    uint16_t phase_offset = 0);

// WLED's color_blend(c1, c2, amount): blend from a to b by amount/255.
// wled00/FX_fcn.cpp
Rgb blend(const Rgb &a, const Rgb &b, uint8_t amount);

// WLED's fadeToBlackBy()/fade_out(): darkens every pixel in `frame` toward
// black by `fade_amount`/255 - the "afterglow" most trail/sparkle-decay
// effects use instead of clearing the frame outright each call.
void fade_to_black_by(Frame frame, uint8_t fade_amount);

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
