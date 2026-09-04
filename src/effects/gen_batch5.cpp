#include "effects.h"

#include <cmath>
#include <cstring>

#include "display/gu_display.h"
#include "palettes.h"
#include "wled_compat.h"

namespace effects {
namespace {

// ---------------------------------------------------------------------------
// Shared math this batch needs that isn't in wled_compat.h - FastLED
// lib8tion.h / WLED util.cpp functions several effects below call, ported
// once here and reused (matches wled_compat.h's own "shape, not bit-exact"
// tolerance - none of this batch depends on bit-for-bit reproducibility).
// ---------------------------------------------------------------------------

uint8_t triwave8(uint8_t in) {
  if (in & 0x80) in = 255 - in;
  return static_cast<uint8_t>(in << 1);
}

// FastLED's ease8InOutCubic(), lib8tion.h - used by cubicwave8() below.
uint8_t ease8InOutCubic(uint8_t i) {
  uint8_t ii = scale8(i, i);
  uint8_t iii = scale8(ii, i);
  uint16_t r1 = (3u * ii) - (2u * iii);
  return (r1 & 0x100) ? 255 : static_cast<uint8_t>(r1);
}

// FastLED's cubicwave8() - a triangle wave eased into an S-curve. Used by
// Plasma/Phased/Sinewave/Dancing Shadows' gradient spot type.
uint8_t cubicwave8(uint8_t in) { return ease8InOutCubic(triwave8(in)); }

uint16_t triwave16(uint16_t in) {
  if (in & 0x8000) in = 65535 - in;
  return static_cast<uint16_t>(in << 1);
}

// wled00/FX.cpp:102 tristate_square8() - a square wave with attack/decay
// ramps, used by Washing Machine.
int8_t tristate_square8(uint8_t x, uint8_t pulsewidth, uint8_t attdec) {
  int8_t a = 127;
  if (x > 127) {
    a = -127;
    x -= 127;
  }
  if (x < attdec) return static_cast<int8_t>(static_cast<int16_t>(x) * a / attdec);
  if (x < pulsewidth - attdec) return a;
  if (x < pulsewidth) return static_cast<int8_t>(static_cast<int16_t>(pulsewidth - x) * a / attdec);
  return 0;
}

uint16_t scale16(uint16_t i, uint16_t scale) {
  return static_cast<uint16_t>((static_cast<uint32_t>(i) * scale) >> 16);
}

// Matches wled_compat.cpp's own private beat16() helper (same period_ms /
// linear-ramp formula) - duplicated here since that one isn't exposed via
// wled_compat.h, and Pacifica/Dancing Shadows below need the raw sawtooth
// (not run through sin8/16 like beatsin8/16() already do).
uint16_t beat16_local(uint32_t now_ms, uint16_t bpm) {
  uint32_t period_ms = 60000u / (bpm ? bpm : 1);
  uint64_t pos = (static_cast<uint64_t>(now_ms % period_ms) * 65536ull) / period_ms;
  return static_cast<uint16_t>(pos);
}
uint8_t beat8_local(uint32_t now_ms, uint16_t bpm) {
  return static_cast<uint8_t>(beat16_local(now_ms, bpm) >> 8);
}
// FastLED's beatsin88() takes an accum88 (fixed-point, bpm*256) beats-per-
// minute for finer control than beatsin16()'s plain uint8_t bpm - Pacifica
// uses it for its slow, oddly-specific drift speeds. We only have integer
// bpm here, so the fractional part of bpm88 is dropped (shape survives,
// the exact drift period is approximate).
uint16_t beatsin88_local(uint32_t now_ms, uint16_t bpm88, uint16_t low, uint16_t high) {
  return beatsin16(now_ms, static_cast<uint8_t>(bpm88 >> 8), low, high);
}

Rgb add_clamped(const Rgb &a, const Rgb &b) {
  return Rgb{qadd8(a.r, b.r), qadd8(a.g, b.g), qadd8(a.b, b.b)};
}
Rgb scale_rgb(const Rgb &c, uint8_t scale) {
  return Rgb{scale8(c.r, scale), scale8(c.g, scale), scale8(c.b, scale)};
}

// wled00/FX_fcn.cpp:1108 Segment::blur() - the 1D (non-smear) branch only;
// this batch never runs on a genuinely 2D segment.
void blur1d(Rgb *row, int len, uint8_t blur_amount) {
  if (blur_amount == 0 || len <= 0) return;
  uint8_t keep = 255 - blur_amount;
  uint8_t seep = blur_amount >> 1;
  Rgb cur = row[0];
  Rgb carry = scale_rgb(cur, seep);
  row[0] = scale_rgb(cur, keep);
  for (int i = 1; i < len; i++) {
    cur = row[i];
    Rgb part = scale_rgb(cur, seep);
    Rgb kept = scale_rgb(cur, keep);
    row[i - 1] = add_clamped(row[i - 1], part);
    row[i] = add_clamped(kept, carry);
    carry = part;
  }
}

// FastLED ColorFromPalette() over a raw 16-stop 0xRRGGBB table (LINEARBLEND,
// wrapping) - Pacifica's three hand-picked ocean palettes are fixed 16-entry
// tables, not one of this codebase's 72 registered palettes, so
// palettes::color_from_palette() can't be used for them directly.
Rgb palette16_lookup(const uint32_t *entries, uint8_t index) {
  uint8_t hi4 = index >> 4;
  uint8_t lo4 = static_cast<uint8_t>(index & 0x0F);
  uint32_t c1 = entries[hi4];
  uint32_t c2 = entries[(hi4 + 1) & 0x0F];
  Rgb a{static_cast<uint8_t>(c1 >> 16), static_cast<uint8_t>(c1 >> 8), static_cast<uint8_t>(c1)};
  Rgb b{static_cast<uint8_t>(c2 >> 16), static_cast<uint8_t>(c2 >> 8), static_cast<uint8_t>(c2)};
  return blend(a, b, static_cast<uint8_t>(lo4 << 4));
}

// A tiny seedable PRNG, used only by Twinkleup below. WLED reseeds its own
// PRNG to a fixed value once per frame so every pixel's "random" brightness
// is actually a deterministic function of its index (that's what makes the
// twinkle pattern look stable rather than pure static) - our random8() is
// backed by the RP2040's hardware RNG and can't be seeded, so this is a
// small separate LCG standing in for FastLED's PRNG. Shape (deterministic
// per-index, changes over time via strip.now), not bit-exact output.
struct SeededPrng {
  uint32_t s;
  explicit SeededPrng(uint32_t seed) : s(seed) {}
  uint8_t next() {
    s = s * 1664525u + 1013904223u;
    return static_cast<uint8_t>(s >> 24);
  }
};

// wled00/FX.cpp:3435 mode_popcorn(). Real WLED can run this over several
// "virtual strips" (columns of a 2D bar-graph segment) - dropped here since
// we always treat this effect as one 1D strip broadcast down every column.
// Also drops the "!SEGMENT.palette -> pick a raw SEGCOLOR" branch: this
// firmware's palette 0 is already a colorful default (see palettes.h), so
// every kernel always looks its color up through the palette.
// No default member initializers here or in the other per-effect state
// structs below - state.data arrives zero-filled (effects.cpp's render()
// zeroes it on every effect switch, matching WLED's own zeroed
// allocateData()), and this struct is reinterpret_cast over that raw
// memory rather than constructed, so any initializer here would never
// actually run. pos==0.0f (not negative) at start is intentional, same as
// upstream: it settles into the "inactive, waiting to pop" state within
// the first couple of frames (pos+vel with vel initially 0 then gravity).
struct PopcornKernel {
  float pos;
  float vel;
  uint8_t color_index;
};
constexpr int kMaxPopcorn = 21;
struct PopcornState {
  PopcornKernel kernels[kMaxPopcorn];
};
static_assert(sizeof(PopcornState) <= State::kDataSize, "PopcornState too big");

void mode_popcorn(uint32_t, const Params &p, State &state, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  auto &s = *reinterpret_cast<PopcornState *>(state.data);

  bool has_col2 = p.tertiary.r || p.tertiary.g || p.tertiary.b;
  if (!p.option2) {
    Rgb bg = has_col2 ? Rgb{0, 0, 0} : p.secondary;
    for (int x = 0; x < width; x++) fill_column(frame, x, bg);
  }

  float gravity = -0.0001f - (p.speed / 200000.0f);
  gravity *= width;

  int num_popcorn = p.intensity * kMaxPopcorn / 255;
  if (num_popcorn == 0) num_popcorn = 1;

  for (int i = 0; i < num_popcorn; i++) {
    PopcornKernel &k = s.kernels[i];
    if (k.pos >= 0.0f) {
      k.pos += k.vel;
      k.vel += gravity;
    } else {
      if (random8() < 2) {
        k.pos = 0.01f;
        unsigned peak_height = 128 + random8(128);
        peak_height = (peak_height * static_cast<unsigned>(width - 1)) >> 8;
        k.vel = sqrtf(-2.0f * gravity * peak_height);
        k.color_index = random8();
      }
    }
    if (k.pos >= 0.0f) {
      Rgb col = color_from_palette(p.palette_id, k.color_index, p.primary, p.secondary, p.tertiary);
      int led_index = static_cast<int>(k.pos);
      if (led_index >= 0 && led_index < width) fill_column(frame, led_index, col);
    }
  }
}
EFFECTS_REGISTER(Id::kPopcorn, mode_popcorn)

// wled00/FX.cpp:3867 mode_drip(). Virtual-strip support dropped for the
// same reason as Popcorn above - one 1D strip.
struct DripDrop {
  float pos = 0.0f;
  float vel = 0.0f;
  uint16_t brightness = 0;
  uint8_t drop_state = 0;  // 0 init, 1 forming, 2 falling, 5 bouncing
};
constexpr int kMaxDrops = 4;
struct DripState {
  DripDrop drops[kMaxDrops];
};
static_assert(sizeof(DripState) <= State::kDataSize, "DripState too big");

void mode_drip(uint32_t, const Params &p, State &state, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  auto &s = *reinterpret_cast<DripState *>(state.data);

  if (!p.option2) {
    for (int x = 0; x < width; x++) fill_column(frame, x, p.secondary);
  }

  int num_drops = 1 + (p.intensity >> 6);
  float gravity = -0.0005f - (p.speed / 50000.0f);
  gravity *= (width > 1 ? width - 1 : 1);
  constexpr int source_drop = 12;

  for (int j = 0; j < num_drops; j++) {
    DripDrop &d = s.drops[j];
    if (d.drop_state == 0) {
      d.pos = width - 1;
      d.vel = 0;
      d.brightness = source_drop;
      d.drop_state = 1;
    }

    fill_column(frame, width - 1, blend(Rgb{0, 0, 0}, p.primary, source_drop));

    if (d.drop_state == 1) {
      if (d.brightness > 255) d.brightness = 255;
      int pos_i = static_cast<int>(d.pos);
      if (pos_i >= 0 && pos_i < width)
        fill_column(frame, pos_i, blend(Rgb{0, 0, 0}, p.primary, static_cast<uint8_t>(d.brightness)));

      d.brightness = static_cast<uint16_t>(d.brightness + (1 + (p.speed * 5) / 255));  // map(speed,0,255,1,6)

      if (random8() < d.brightness / 10) {
        d.drop_state = 2;
        d.brightness = 255;
      }
    }
    if (d.drop_state > 1) {
      if (d.pos > 0) {
        d.pos += d.vel;
        if (d.pos < 0) d.pos = 0;
        d.vel += gravity;

        for (int i = 1; i < 7 - d.drop_state; i++) {
          int pos = static_cast<int>(d.pos) + i;
          if (pos < 0) pos = 0;
          if (pos > width - 1) pos = width - 1;
          fill_column(frame, pos, blend(Rgb{0, 0, 0}, p.primary, static_cast<uint8_t>(d.brightness / i)));
        }
        if (d.drop_state > 2) {
          fill_column(frame, 0, blend(p.primary, Rgb{0, 0, 0}, static_cast<uint8_t>(d.brightness)));
        }
      } else {
        if (d.drop_state > 2) {
          d.drop_state = 0;
          d.brightness = source_drop;
        } else {
          if (d.drop_state == 2) {
            d.vel = -d.vel / 4.0f;
            d.pos += d.vel;
          }
          d.brightness = source_drop * 2;
          d.drop_state = 5;
        }
      }
    }
  }
}
EFFECTS_REGISTER(Id::kDrip, mode_drip)

// wled00/FX.cpp:4040 mode_plasma()
void mode_plasma(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  if (state.call == 0) state.aux0 = random8(0, 2);

  unsigned this_phase = beatsin8(now_ms, static_cast<uint8_t>(6 + state.aux0), static_cast<uint8_t>(-64), 64);
  unsigned that_phase = beatsin8(now_ms, static_cast<uint8_t>(7 + state.aux0), static_cast<uint8_t>(-64), 64);

  for (int i = 0; i < width; i++) {
    unsigned color_index =
        cubicwave8(static_cast<uint8_t>((i * (2 + 3 * (p.speed >> 5)) + this_phase) & 0xFF)) / 2 +
        cos8(static_cast<uint8_t>((i * (1 + 2 * (p.speed >> 5)) + that_phase) & 0xFF)) / 2;
    uint8_t this_bright = qsub8(static_cast<uint8_t>(color_index),
                                 beatsin8(now_ms, 7, 0, static_cast<uint8_t>(128 - (p.intensity >> 1))));
    Rgb col = color_from_palette(p.palette_id, static_cast<uint8_t>(color_index), p.primary, p.secondary, p.tertiary);
    fill_column(frame, i, scale_rgb(col, this_bright));
  }
}
EFFECTS_REGISTER(Id::kPlasma, mode_plasma)

// wled00/FX.cpp:4062 mode_percent()
void mode_percent(uint32_t, const Params &p, State &state, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  unsigned percent = p.intensity;
  if (percent > 200) percent = 200;
  unsigned active_leds = (percent < 100) ? static_cast<unsigned>(roundf(width * percent / 100.0f))
                                          : static_cast<unsigned>(roundf(width * (200 - percent) / 100.0f));

  unsigned size = 1 + ((p.speed * width) >> 11);
  if (p.speed == 255) size = 255;

  for (int i = 0; i < width; i++) {
    Rgb col;
    if (percent <= 100) {
      if (static_cast<unsigned>(i) < state.aux1) {
        if (p.option1)
          col = color_from_palette(p.palette_id, static_cast<uint8_t>(percent * 255 / 100), p.primary, p.secondary,
                                    p.tertiary);
        else
          col = color_from_palette(p.palette_id, static_cast<uint8_t>(i * 255 / (width - 1)), p.primary, p.secondary,
                                    p.tertiary);
      } else {
        col = p.secondary;
      }
    } else {
      if (static_cast<unsigned>(i) < (width - state.aux1)) {
        col = p.secondary;
      } else {
        if (p.option1)
          col = color_from_palette(p.palette_id, static_cast<uint8_t>(255 - (percent - 100) * 255 / 100), p.primary,
                                    p.secondary, p.tertiary);
        else
          col = color_from_palette(p.palette_id, static_cast<uint8_t>(i * 255 / (width - 1)), p.primary, p.secondary,
                                    p.tertiary);
      }
    }
    fill_column(frame, i, col);
  }

  if (active_leds > state.aux1) {
    state.aux1 = static_cast<uint16_t>(state.aux1 + size);
    if (state.aux1 > active_leds) state.aux1 = static_cast<uint16_t>(active_leds);
  } else if (active_leds < state.aux1) {
    if (state.aux1 > size) state.aux1 = static_cast<uint16_t>(state.aux1 - size);
    else state.aux1 = 0;
    if (state.aux1 < active_leds) state.aux1 = static_cast<uint16_t>(active_leds);
  }
}
EFFECTS_REGISTER(Id::kPercent, mode_percent)

// wled00/FX.cpp:2557 mode_ripple_rainbow(), with its shared wled00/FX.cpp:
// 2492 ripple_base() helper ported alongside it (1D branch only - this
// effect never runs 2D here). "if (SEGMENT.palette) ... else raw HSV wheel"
// is simplified to always use the palette engine, same as every other
// effect in this batch - see palettes.h, this firmware's palette 0 is
// already a colorful default rather than a flat color.
struct RippleParticle {
  uint8_t state = 0;
  uint8_t color = 0;
  uint16_t pos = 0;
};
constexpr int kMaxRipples = 9;  // min(1 + (32>>2), 100)
struct RippleRainbowState {
  RippleParticle ripples[kMaxRipples];
};
static_assert(sizeof(RippleRainbowState) <= State::kDataSize, "RippleRainbowState too big");

void mode_ripple_rainbow(uint32_t, const Params &p, State &state, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  auto &s = *reinterpret_cast<RippleRainbowState *>(state.data);

  if (state.call == 0) {
    state.aux0 = random8();
    state.aux1 = random8();
  }
  if (state.aux0 == state.aux1) {
    state.aux1 = random8();
  } else if (state.aux1 > state.aux0) {
    state.aux0++;
  } else {
    state.aux0--;
  }

  Rgb bg = blend(color_from_palette(p.palette_id, static_cast<uint8_t>(state.aux0), p.primary, p.secondary, p.tertiary),
                 Rgb{0, 0, 0}, 235);
  Rgb row[width];
  for (int x = 0; x < width; x++) row[x] = bg;

  unsigned rippledecay = (p.speed >> 4) + 1;
  for (int i = 0; i < kMaxRipples; i++) {
    RippleParticle &r = s.ripples[i];
    if (r.state) {
      unsigned origin = r.pos;
      Rgb col = color_from_palette(p.palette_id, r.color, p.primary, p.secondary, p.tertiary);
      unsigned propagation = (r.state / rippledecay - 1) * (p.speed + 1);
      int prop_i = propagation >> 8;
      unsigned prop_f = propagation & 0xFF;
      unsigned amp;
      if (r.state < 17) {
        amp = triwave8(static_cast<uint8_t>((r.state - 1) * 8));
      } else {
        // Arduino map(state, 17, 255, 255, 2)
        amp = 255 - static_cast<unsigned>((r.state - 17) * 253L / 238);
      }

      int left = static_cast<int>(origin) - prop_i - 1;
      int right = static_cast<int>(origin) + prop_i + 2;
      for (int v = 0; v < 4; v++) {
        uint8_t mag = scale8(cubicwave8(static_cast<uint8_t>((prop_f >> 2) + v * 64)), static_cast<uint8_t>(amp));
        int lp = left + v, rp = right - v;
        if (lp >= 0 && lp < width) row[lp] = blend(row[lp], col, mag);
        if (rp >= 0 && rp < width) row[rp] = blend(row[rp], col, mag);
      }

      unsigned new_state = r.state + rippledecay;
      r.state = (new_state > 254) ? 0 : static_cast<uint8_t>(new_state);
    } else {
      if (random16(15100) <= p.intensity) {
        r.state = 1;
        r.pos = random16(width);
        r.color = random8();
      }
    }
  }

  blur1d(row, width, 0);  // ripple_base(0) - custom1>>1 not wired for this mode
  for (int x = 0; x < width; x++) fill_column(frame, x, row[x]);
}
EFFECTS_REGISTER(Id::kRippleRainbow, mode_ripple_rainbow)

// wled00/FX.cpp:4113 mode_heartbeat()
void mode_heartbeat(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  unsigned bpm = 40 + (p.speed >> 3);
  uint32_t ms_per_beat = 60000u / bpm;
  uint32_t second_beat = ms_per_beat / 3;
  uint32_t bri_lower = state.aux1;
  uint32_t beat_timer = now_ms - state.step;

  bri_lower = bri_lower * 2042u / (2048u + p.intensity);
  state.aux1 = static_cast<uint16_t>(bri_lower);

  if (beat_timer > second_beat && !state.aux0) {
    state.aux1 = 0xFFFF;
    state.aux0 = 1;
  }
  if (beat_timer > ms_per_beat) {
    state.aux1 = 0xFFFF;
    state.aux0 = 0;
    state.step = now_ms;
  }

  for (int i = 0; i < width; i++) {
    Rgb col = color_from_palette(p.palette_id, static_cast<uint8_t>(i * 255 / (width - 1)), p.primary, p.secondary,
                                  p.tertiary);
    fill_column(frame, i, blend(col, p.secondary, static_cast<uint8_t>(255 - (state.aux1 >> 8))));
  }
}
EFFECTS_REGISTER(Id::kHeartbeat, mode_heartbeat)

// wled00/FX.cpp:4180 mode_pacifica(), with its wled00/FX.cpp:4165
// pacifica_one_layer() helper. When a non-default palette is selected this
// firmware routes all three wave layers through it (matching WLED's own
// "if (SEGMENT.palette) use SEGPALETTE for every layer" branch); otherwise
// it uses Pacifica's three hand-picked 16-stop ocean palettes verbatim.
// CRGB::getAverageLight() is approximated as a plain (r+g+b)/3.
Rgb pacifica_lookup(const Params &p, const uint32_t *local_table, uint8_t index) {
  if (p.palette_id != 0) return color_from_palette(p.palette_id, index, p.primary, p.secondary, p.tertiary);
  return palette16_lookup(local_table, index);
}

Rgb pacifica_one_layer(const Params &p, const uint32_t *local_table, uint16_t i, uint16_t cistart,
                        uint16_t wavescale, uint8_t bri, uint16_t ioff) {
  unsigned ci = cistart;
  unsigned waveangle = ioff;
  unsigned wavescale_half = (wavescale >> 1) + 20;

  waveangle += (120 + p.intensity) * i;
  unsigned s16 = static_cast<uint16_t>(sin16(static_cast<uint16_t>(waveangle)) + 32768);
  unsigned cs = scale16(static_cast<uint16_t>(s16), static_cast<uint16_t>(wavescale_half)) + wavescale_half;
  ci += (cs * i);
  unsigned sindex16 = static_cast<uint16_t>(sin16(static_cast<uint16_t>(ci)) + 32768);
  unsigned sindex8 = scale16(static_cast<uint16_t>(sindex16), 240);

  Rgb col = pacifica_lookup(p, local_table, static_cast<uint8_t>(sindex8));
  return scale_rgb(col, bri);
}

void mode_pacifica(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  static const uint32_t kPalette1[16] = {0x002229, 0x001E2F, 0x001934, 0x001938, 0x00143F, 0x001443, 0x00B047,
                                          0x00B04C, 0x00004F, 0x000054, 0x000062, 0x00006F, 0x00007A, 0x000085,
                                          0x47938A, 0x64D08E};
  static const uint32_t kPalette2[16] = {0x002229, 0x001E2F, 0x001934, 0x001938, 0x00143F, 0x001443, 0x00B047,
                                          0x00B04C, 0x00004F, 0x000054, 0x000062, 0x00006F, 0x00007A, 0x000085,
                                          0x369B90, 0x4FDC9B};
  static const uint32_t kPalette3[16] = {0x00142C, 0x00193B, 0x002247, 0x002551, 0x002C5A, 0x002F63, 0x00346B,
                                          0x003671, 0x003B78, 0x003F7F, 0x00478E, 0x004D9C, 0x0054A9, 0x005AB4,
                                          0x3F7FDC, 0x5A9CFF};

  uint32_t sci1 = state.aux0, sci2 = state.aux1;
  uint32_t sci3 = state.step & 0xFFFFu, sci4 = state.step >> 16;

  // strip.now is temporarily sped up/slowed down by SEGMENT.speed in the
  // original; `now2` stands in for that adjusted clock, `deltams` for a
  // ~23ms (WLED_FPS=42) FRAMETIME_FIXED guess since this firmware has no
  // exact equivalent.
  constexpr uint32_t kFrameTimeMs = 23;
  uint32_t deltams = (kFrameTimeMs >> 2) + ((kFrameTimeMs * p.speed) >> 7);
  uint64_t deltat = (static_cast<uint64_t>(now_ms) >> 2) + ((static_cast<uint64_t>(now_ms) * p.speed) >> 7);
  uint32_t now2 = static_cast<uint32_t>(deltat);

  unsigned speedfactor1 = beatsin16(now2, 3, 179, 269);
  unsigned speedfactor2 = beatsin16(now2, 4, 179, 269);
  uint32_t deltams1 = (deltams * speedfactor1) / 256;
  uint32_t deltams2 = (deltams * speedfactor2) / 256;
  uint32_t deltams21 = (deltams1 + deltams2) / 2;
  sci1 += deltams1 * beatsin88_local(now2, 1011, 10, 13);
  sci2 -= deltams21 * beatsin88_local(now2, 777, 8, 11);
  sci3 -= deltams1 * beatsin88_local(now2, 501, 5, 7);
  sci4 -= deltams2 * beatsin88_local(now2, 257, 4, 6);
  state.aux0 = static_cast<uint16_t>(sci1);
  state.aux1 = static_cast<uint16_t>(sci2);
  state.step = (sci4 << 16) | (sci3 & 0xFFFFu);

  unsigned basethreshold = beatsin8(now2, 9, 55, 65);
  unsigned wave = beat8_local(now2, 7);

  for (int i = 0; i < width; i++) {
    Rgb acc{2, 6, 10};
    acc = add_clamped(acc, pacifica_one_layer(p, kPalette1, static_cast<uint16_t>(i), static_cast<uint16_t>(sci1),
                                               beatsin16(now2, 3, 11 * 256, 14 * 256), beatsin8(now2, 10, 70, 130),
                                               static_cast<uint16_t>(0 - beat16_local(now2, 301))));
    acc = add_clamped(acc, pacifica_one_layer(p, kPalette2, static_cast<uint16_t>(i), static_cast<uint16_t>(sci2),
                                               beatsin16(now2, 4, 6 * 256, 9 * 256), beatsin8(now2, 17, 40, 80),
                                               beat16_local(now2, 401)));
    acc = add_clamped(acc, pacifica_one_layer(p, kPalette3, static_cast<uint16_t>(i), static_cast<uint16_t>(sci3),
                                               6 * 256, beatsin8(now2, 9, 10, 38),
                                               static_cast<uint16_t>(0 - beat16_local(now2, 503))));
    acc = add_clamped(acc, pacifica_one_layer(p, kPalette3, static_cast<uint16_t>(i), static_cast<uint16_t>(sci4),
                                               5 * 256, beatsin8(now2, 8, 10, 28), beat16_local(now2, 601)));

    unsigned threshold = scale8(sin8(static_cast<uint8_t>(wave)), 20) + basethreshold;
    wave += 7;
    unsigned l = (static_cast<unsigned>(acc.r) + acc.g + acc.b) / 3;
    if (l > threshold) {
      unsigned overage = l - threshold;
      unsigned overage2 = qadd8(static_cast<uint8_t>(overage), static_cast<uint8_t>(overage));
      acc = add_clamped(acc, Rgb{static_cast<uint8_t>(overage), static_cast<uint8_t>(overage2),
                                  qadd8(static_cast<uint8_t>(overage2), static_cast<uint8_t>(overage2))});
    }
    fill_column(frame, i, acc);
  }
}
EFFECTS_REGISTER(Id::kPacifica, mode_pacifica)

// wled00/FX.cpp:3586 mode_candle_multi() -> wled00/FX.cpp:3498 candle(true).
// Real WLED stores pixel 0's flicker state in SEGENV.aux0/aux1/step (same
// fields this firmware's State has) and pixels 1..SEGLEN-1 in allocated
// data; ported the same split here rather than a uniform array, so this
// stays a direct port instead of a re-derivation. Upstream's i==0 loop
// iteration also repaints the *entire* strip with pixel 0's flicker value
// before later iterations overwrite pixels 1..N-1 with their own - that
// full-strip wash is a no-op by the end of the pass (everything past
// index 0 gets overwritten), so it's dropped here in favor of each pixel
// just drawing its own color directly; the final frame is identical.
struct CandleMultiState {
  uint32_t last_call_ms = 0;
  uint8_t px[GuDisplay::WIDTH - 1][3] = {{0}};  // s, s_target, fade_step per pixel 1..WIDTH-1
};
static_assert(sizeof(CandleMultiState) <= State::kDataSize, "CandleMultiState too big");

void mode_candle_multi(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  auto &s = *reinterpret_cast<CandleMultiState *>(state.data);

  if (now_ms - s.last_call_ms < 23) return;  // ~FRAMETIME_FIXED, see mode_pacifica's note on this guess
  s.last_call_ms = now_ms;

  unsigned valrange = p.intensity;
  unsigned rndval = valrange >> 1;

  unsigned speed_factor = 4;
  if (p.speed > 252) speed_factor = 1;
  else if (p.speed > 99) speed_factor = 2;
  else if (p.speed > 49) speed_factor = 3;

  for (int i = 0; i < width; i++) {
    unsigned sv, s_target, fade_step;
    if (i == 0) {
      sv = state.aux0;
      s_target = state.aux1;
      fade_step = state.step;
    } else {
      sv = s.px[i - 1][0];
      s_target = s.px[i - 1][1];
      fade_step = s.px[i - 1][2];
    }
    if (fade_step == 0) {
      sv = 128;
      s_target = 130 + random8(4);
      fade_step = 1;
    }

    bool new_target = false;
    if (s_target > sv) {
      sv = qadd8(static_cast<uint8_t>(sv), static_cast<uint8_t>(fade_step));
      if (sv >= s_target) new_target = true;
    } else {
      sv = qsub8(static_cast<uint8_t>(sv), static_cast<uint8_t>(fade_step));
      if (sv <= s_target) new_target = true;
    }

    if (new_target) {
      s_target = random8(static_cast<uint8_t>(rndval)) + random8(static_cast<uint8_t>(rndval));
      if (s_target < (rndval >> 1)) s_target = (rndval >> 1) + random8(static_cast<uint8_t>(rndval));
      s_target += (255 - valrange);

      unsigned dif = (s_target > sv) ? s_target - sv : sv - s_target;
      fade_step = dif >> speed_factor;
      if (fade_step == 0) fade_step = 1;
    }

    Rgb col = blend(p.secondary,
                     color_from_palette(p.palette_id, static_cast<uint8_t>(i * 255 / (width - 1)), p.primary,
                                         p.secondary, p.tertiary),
                     static_cast<uint8_t>(sv));
    fill_column(frame, i, col);

    if (i == 0) {
      state.aux0 = static_cast<uint16_t>(sv);
      state.aux1 = static_cast<uint16_t>(s_target);
      state.step = fade_step;
    } else {
      s.px[i - 1][0] = static_cast<uint8_t>(sv);
      s.px[i - 1][1] = static_cast<uint8_t>(s_target);
      s.px[i - 1][2] = static_cast<uint8_t>(fade_step);
    }
  }
}
EFFECTS_REGISTER(Id::kCandleMulti, mode_candle_multi)

// wled00/FX.cpp:3414 mode_solid_glitter() -> wled00/FX.cpp:3387
// glitter_base().
void mode_solid_glitter(uint32_t, const Params &p, State &, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  for (int x = 0; x < width; x++) fill_column(frame, x, p.primary);

  bool has_col2 = p.tertiary.r || p.tertiary.g || p.tertiary.b;
  Rgb glitter_col = has_col2 ? p.tertiary : Rgb{255, 255, 255};
  if (p.intensity > random8()) fill_column(frame, random16(width), glitter_col);
}
EFFECTS_REGISTER(Id::kSolidGlitter, mode_solid_glitter)

// wled00/FX.cpp:4259 mode_sunrise(). No separate wall-clock vs strip.now
// distinction here (this firmware only has one clock, now_ms), so the
// "start time" bookkeeping uses now_ms for both.
void mode_sunrise(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  if (state.call == 0 || p.speed != state.aux0) {
    state.step = now_ms;
    state.aux0 = p.speed;
  }

  for (int x = 0; x < width; x++) fill_column(frame, x, Rgb{0, 0, 0});

  unsigned stage = 0xFFFF;
  uint32_t s10_since_start = (now_ms - state.step) / 100;

  if (p.speed > 120) {
    unsigned counter = (now_ms >> 1) * (((p.speed - 120) >> 1) + 1);
    stage = triwave16(static_cast<uint16_t>(counter));
  } else if (p.speed) {
    unsigned dur_mins = p.speed;
    if (dur_mins > 60) dur_mins -= 60;
    uint32_t s10_target = dur_mins * 600;
    if (s10_since_start > s10_target) s10_since_start = s10_target;
    stage = static_cast<unsigned>((static_cast<uint64_t>(s10_since_start) * 0xFFFF) / (s10_target ? s10_target : 1));
    if (p.speed > 60) stage = 0xFFFF - stage;
  }

  for (int i = 0; i <= width / 2; i++) {
    unsigned wave = triwave16(static_cast<uint16_t>((i * stage) / width));
    wave = (wave >> 8) + ((wave * p.intensity) >> 15);
    Rgb c = color_from_palette(p.palette_id, static_cast<uint8_t>(wave > 240 ? 240 : wave), p.primary, p.secondary,
                                p.tertiary);
    fill_column(frame, i, c);
    fill_column(frame, width - i - 1, c);
  }
}
EFFECTS_REGISTER(Id::kSunrise, mode_sunrise)

// wled00/FX.cpp:4308 phased_base(), shared by mode_phased() (wled00/FX.cpp:
// 4332) and mode_phased_noise() (wled00/FX.cpp:4338). `perlin8()` (a real
// Perlin-noise lookup in WLED) is approximated below by `pseudo_noise8()`, a
// hashed-lattice value-noise stand-in - same smooth-pseudorandom shape,
// not the same curve.
uint8_t pseudo_noise8(uint16_t x) {
  auto hash = [](uint16_t n) -> uint8_t {
    uint32_t h = static_cast<uint32_t>(n) * 2654435761u;
    return static_cast<uint8_t>((h >> 16) & 0xFF);
  };
  uint16_t cell = x >> 4;
  uint8_t t = static_cast<uint8_t>((x & 0x0F) << 4);
  uint8_t a = hash(cell), b = hash(static_cast<uint16_t>(cell + 1));
  return static_cast<uint8_t>(a + (((static_cast<int>(b) - a) * t) / 255));
}

void phased_base(uint32_t now_ms, const Params &p, State &state, Frame frame, bool noise_mod) {
  constexpr int width = GuDisplay::WIDTH;
  constexpr unsigned allfreq = 16;
  float &phase = *reinterpret_cast<float *>(&state.step);
  unsigned cut_off = 255 - p.intensity;
  unsigned mod_val = 5;

  unsigned index = now_ms / 64;
  phase += p.speed / 32.0f;

  for (int i = 0; i < width; i++) {
    if (noise_mod) mod_val = pseudo_noise8(static_cast<uint16_t>(i * 10 + i * 10)) / 16;
    unsigned val = (i + 1) * allfreq;
    if (mod_val == 0) mod_val = 1;
    val += static_cast<unsigned>(phase * (i % mod_val + 1) / 2);
    unsigned b = cubicwave8(static_cast<uint8_t>(val));
    b = (b > cut_off) ? (b - cut_off) : 0;
    Rgb col = color_from_palette(p.palette_id, static_cast<uint8_t>(index), p.primary, p.secondary, p.tertiary);
    fill_column(frame, i, blend(p.secondary, col, static_cast<uint8_t>(b)));
    index += 256 / width;
  }
}

void mode_phased(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  phased_base(now_ms, p, state, frame, false);
}
EFFECTS_REGISTER(Id::kPhased, mode_phased)

// wled00/FX.cpp:4344 mode_twinkleup(). See SeededPrng's comment above for
// why this uses a small local LCG instead of reseeding effects::random8().
void mode_twinkleup(uint32_t now_ms, const Params &p, State &, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  SeededPrng prng(535);
  for (int i = 0; i < width; i++) {
    unsigned ranstart = prng.next();
    unsigned pix_bri = sin8(static_cast<uint8_t>(ranstart + 16 * now_ms / (256 - p.speed)));
    if (prng.next() > p.intensity) pix_bri = 0;
    Rgb col = color_from_palette(p.palette_id, static_cast<uint8_t>(prng.next() + now_ms / 100), p.primary,
                                  p.secondary, p.tertiary);
    fill_column(frame, i, blend(p.secondary, col, static_cast<uint8_t>(pix_bri)));
  }
}
EFFECTS_REGISTER(Id::kTwinkleup, mode_twinkleup)

// wled00/FX.cpp:4398 mode_sinewave()
void mode_sinewave(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  unsigned color_index = now_ms / 32;
  state.step += p.speed / 16;
  unsigned freq = p.intensity / 4;

  for (int i = 0; i < width; i++) {
    uint8_t pix_bri = cubicwave8(static_cast<uint8_t>((i * freq) + state.step));
    Rgb col = color_from_palette(p.palette_id, static_cast<uint8_t>(i * color_index / 255), p.primary, p.secondary,
                                  p.tertiary);
    fill_column(frame, i, blend(p.secondary, col, pix_bri));
  }
}
EFFECTS_REGISTER(Id::kSinewave, mode_sinewave)

// wled00/FX.cpp:4338 mode_phased_noise() -> shares phased_base() above.
void mode_phased_noise(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  phased_base(now_ms, p, state, frame, true);
}
EFFECTS_REGISTER(Id::kPhasednoise, mode_phased_noise)

// wled00/FX.cpp:4418 mode_flow(). Drops the SEGMENT.reverse branch - Params
// has no equivalent flag (see effects.h).
void mode_flow(uint32_t now_ms, const Params &p, State &, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  unsigned counter = 0;
  if (p.speed != 0) counter = (now_ms * ((p.speed >> 2) + 1)) >> 8;

  int max_zones = width / 6;
  int zones = (p.intensity * max_zones) >> 8;
  if (zones & 0x01) zones++;
  if (zones < 2) zones = 2;
  int zone_len = width / zones;
  int required_zones = (width + zone_len - 1) / zone_len;
  zones = required_zones + 2;
  int offset = (width - (zones * zone_len)) / 2;

  for (int z = 0; z < zones; z++) {
    int pos = offset + z * zone_len;
    for (int i = 0; i < zone_len; i++) {
      unsigned color_index = (i * 255 / zone_len) - counter;
      int led = (z & 0x01) ? i : (zone_len - 1) - i;
      int px = pos + led;
      if (px >= 0 && px < width)
        fill_column(frame, px,
                    color_from_palette(p.palette_id, static_cast<uint8_t>(color_index), p.primary, p.secondary,
                                        p.tertiary));
    }
  }
}
EFFECTS_REGISTER(Id::kFlow, mode_flow)

// wled00/FX.cpp:4455 mode_chunchun(). SEGMENT.fade_out() fades toward
// SEGCOLOR(1); wled_compat.h's fade_to_black_by() only fades toward black,
// so this approximates it (fine for chunchun's typical black background).
void mode_chunchun(uint32_t now_ms, const Params &p, State &, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  fade_to_black_by(frame, 40);

  unsigned counter = now_ms * (6 + (p.speed >> 4));
  unsigned num_birds = 2 + (width >> 3);
  unsigned span = (p.intensity << 8) / num_birds;

  for (unsigned i = 0; i < num_birds; i++) {
    counter -= span;
    unsigned megumin = static_cast<uint16_t>(sin16(static_cast<uint16_t>(counter)) + 0x8000);
    int bird = static_cast<int>((static_cast<uint32_t>(megumin) * width) >> 16);
    if (bird < 0) bird = 0;
    if (bird > width - 1) bird = width - 1;
    fill_column(frame, bird,
                color_from_palette(p.palette_id, static_cast<uint8_t>((i * 255) / num_birds), p.primary, p.secondary,
                                    p.tertiary));
  }
}
EFFECTS_REGISTER(Id::kChunchun, mode_chunchun)

// wled00/FX.cpp:4505 mode_dancing_shadows(). This is the classic
// spotlight-simulation port (no ParticleSystem/audio involved, despite
// living behind a `WLED_PS_DONT_REPLACE_1D_FX` build flag upstream that
// swaps it for a particle-system version by default) - the actual math
// ported here doesn't touch either.
// Zero-filled (not constructed) same as PopcornKernel above - width_px
// reads as 0 until first initialize/respawn sets it, which always happens
// before it's read (state.aux0 starts at 0 != num_spotlights, so
// `initialize` is true on frame 1).
struct Spotlight {
  float speed;
  uint8_t color_idx;
  int16_t position;
  uint32_t last_update_ms;
  uint8_t width_px;
  uint8_t type;
};
constexpr int kMaxSpotlights = 49;
struct DancingShadowsState {
  Spotlight spots[kMaxSpotlights];
};
static_assert(sizeof(DancingShadowsState) <= State::kDataSize, "DancingShadowsState too big");

void mode_dancing_shadows(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  auto &s = *reinterpret_cast<DancingShadowsState *>(state.data);

  unsigned num_spotlights = 2 + (p.intensity * (kMaxSpotlights - 2)) / 255;
  bool initialize = state.aux0 != num_spotlights;
  state.aux0 = static_cast<uint16_t>(num_spotlights);

  for (int x = 0; x < width; x++) fill_column(frame, x, Rgb{0, 0, 0});

  bool respawn = false;
  for (unsigned i = 0; i < num_spotlights; i++) {
    Spotlight &sp = s.spots[i];
    if (!initialize) {
      int delta = static_cast<int>((now_ms - sp.last_update_ms) * (sp.speed * ((1.0f + p.speed) / 100.0f)));
      if (delta >= 1 || delta <= -1) {
        sp.position = static_cast<int16_t>(sp.position + delta);
        sp.last_update_ms = now_ms;
      }
      respawn = (sp.speed > 0.0f && sp.position > width + 2) ||
                (sp.speed < 0.0f && sp.position < -(sp.width_px + 2));
    }

    if (initialize || respawn) {
      sp.color_idx = random8();
      sp.width_px = random8(1, 10);
      sp.speed = 1.0f / random8(4, 50);
      if (initialize) {
        sp.position = static_cast<int16_t>(random16(width));
        sp.speed *= random8(2) ? 1.0f : -1.0f;
      } else {
        if (random8(2)) {
          sp.position = static_cast<int16_t>(width + sp.width_px);
          sp.speed *= -1.0f;
        } else {
          sp.position = static_cast<int16_t>(-sp.width_px);
        }
      }
      sp.last_update_ms = now_ms;
      sp.type = random8(6);
    }

    Rgb color = color_from_palette(p.palette_id, sp.color_idx, p.primary, p.secondary, p.tertiary);
    int start = sp.position;

    auto blend_px = [&](int x, uint8_t amount) {
      if (x >= 0 && x < width) frame[0][x] = blend(frame[0][x], color, amount);
    };

    if (sp.width_px <= 1) {
      blend_px(start, 128);
    } else {
      switch (sp.type) {
        case 0:  // solid
          for (int j = 0; j < sp.width_px; j++) blend_px(start + j, 128);
          break;
        case 1:  // gradient
          for (int j = 0; j < sp.width_px; j++)
            blend_px(start + j, cubicwave8(static_cast<uint8_t>(j * 255 / (sp.width_px - 1 ? sp.width_px - 1 : 1))));
          break;
        case 2:  // 2x gradient
          for (int j = 0; j < sp.width_px; j++)
            blend_px(start + j,
                     cubicwave8(static_cast<uint8_t>(2 * (j * 255 / (sp.width_px - 1 ? sp.width_px - 1 : 1)))));
          break;
        case 3:  // 2x dot
          for (int j = 0; j < sp.width_px; j += 2) blend_px(start + j, 128);
          break;
        case 4:  // 3x dot
          for (int j = 0; j < sp.width_px; j += 3) blend_px(start + j, 128);
          break;
        case 5:  // 4x dot
          for (int j = 0; j < sp.width_px; j += 4) blend_px(start + j, 128);
          break;
      }
    }
  }

  for (int x = 0; x < width; x++) fill_column(frame, x, frame[0][x]);
}
EFFECTS_REGISTER(Id::kDancingShadows, mode_dancing_shadows)

// wled00/FX.cpp:4625 mode_washing_machine()
void mode_washing_machine(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  int speed = tristate_square8(static_cast<uint8_t>(now_ms >> 7), 90, 15);
  state.step += (speed * 2048) / (512 - p.speed);

  for (int i = 0; i < width; i++) {
    uint8_t col = sin8(static_cast<uint8_t>(((p.intensity / 25 + 1) * 255 * i / width) + (state.step >> 7)));
    fill_column(frame, i, color_from_palette(p.palette_id, col, p.primary, p.secondary, p.tertiary));
  }
}
EFFECTS_REGISTER(Id::kWashingMachine, mode_washing_machine)

}  // namespace
}  // namespace effects
