#include <cmath>
#include <cstdint>

#include "effects.h"
#include "palettes.h"
#include "wled_compat.h"

namespace effects {
namespace {

// Shared helpers below this point are effect-specific porting-support code
// that no other landed batch needs yet (real WLED's inoise8/16, a seedable
// PRNG, cubicwave8, beatsin88_t, HSV->RGB) - wled_compat.h only carries
// helpers shared by every batch, so these live here instead of duplicating
// the "don't reinvent, don't touch shared files" rule for something only
// this batch's effects use. Same "shape, not bit-exact" tolerance
// wled_compat.h documents for its own helpers applies to all of these.

constexpr uint32_t kFrametimeFixedMs = 1000 / 42;  // wled00/FX.h:62 FRAMETIME_FIXED, WLED_FPS=42

uint8_t palette_index_mapped(int pos, int len) {
  if (len <= 0) return 0;
  int v = (pos * 255) / len;
  return v > 255 ? 255 : static_cast<uint8_t>(v);
}

// wled00/FX_fcn.cpp:1166 Segment::color_from_palette()'s mapping=true branch:
// paletteIndex = min((i*255)/vLength(), 255).
uint8_t triwave8(uint8_t in) {
  if (in & 0x80) in = static_cast<uint8_t>(255 - in);
  return static_cast<uint8_t>(in << 1);
}
uint8_t ease8InOutCubic(uint8_t i) {
  uint16_t ii = scale8(i, i);
  uint16_t iii = scale8(static_cast<uint8_t>(ii), i);
  uint16_t r1 = static_cast<uint16_t>((3 * ii) - (2 * iii));
  return (r1 & 0x100) ? 255 : static_cast<uint8_t>(r1);
}
// FastLED's cubicwave8(), used by mode_lake().
uint8_t cubicwave8(uint8_t in) { return ease8InOutCubic(triwave8(in)); }

// FastLED's beatsin88_t()/beatsin16_t() family, ported to wled_compat.h's
// beatsin16()'s style (period_ms = 60000/bpm) but for a Q8.8 (bpm*256)
// input, needed by mode_colorwaves_pride_base()'s beatsin88_t(87,...) etc.
// calls - wled_compat.h's own beatsin16/beatsin8 only take a whole-bpm
// uint8_t.
uint16_t beatsin88(uint32_t now_ms, uint16_t bpm88, uint16_t low, uint16_t high,
                    uint16_t phase_offset = 0) {
  uint32_t period_ms = bpm88 ? static_cast<uint32_t>(60000ull * 256 / bpm88) : 60000;
  uint64_t pos = (static_cast<uint64_t>(now_ms % period_ms) * 65536ull) / period_ms;
  uint16_t beat = static_cast<uint16_t>(pos) + phase_offset;
  int16_t s = sin16(beat);
  uint16_t su = static_cast<uint16_t>(s + 32768);
  uint32_t range = static_cast<uint32_t>(high) - low;
  return static_cast<uint16_t>(low + (static_cast<uint32_t>(su) * range) / 65535);
}

// Standard HSV->RGB (h/s/v all 0-255), matching CHSV's conventional meaning
// closely enough for mode_pride_2015()'s CRGB(CHSV(hue8, sat8, bri8)).
Rgb hsv_to_rgb(uint8_t h, uint8_t s, uint8_t v) {
  if (s == 0) return Rgb{v, v, v};
  uint8_t region = static_cast<uint8_t>(h / 43);
  uint8_t remainder = static_cast<uint8_t>((h - region * 43) * 6);
  uint8_t p = static_cast<uint8_t>((v * (255 - s)) >> 8);
  uint8_t q = static_cast<uint8_t>((v * (255 - ((s * remainder) >> 8))) >> 8);
  uint8_t t = static_cast<uint8_t>((v * (255 - ((s * (255 - remainder)) >> 8))) >> 8);
  switch (region) {
    case 0: return Rgb{v, t, p};
    case 1: return Rgb{q, v, p};
    case 2: return Rgb{p, v, t};
    case 3: return Rgb{p, q, v};
    case 4: return Rgb{t, p, v};
    default: return Rgb{v, p, q};
  }
}

// A small seedable xorshift PRNG, used only by mode_random_chase() - WLED
// keys that effect off its own settable/gettable-seed `prng` (a segment-
// local PRNG kept deliberately separate from the shared random8()/random16()
// stream so unrelated effects' random calls don't perturb its sequence).
// wled_compat.h's random8/16() are hardware-RNG-backed and unseedable, so
// this ports the *idea* (a private, restartable PRNG) rather than WLED's
// exact algorithm.
uint16_t xorshift16(uint16_t &s) {
  s = static_cast<uint16_t>(s ^ (s << 7));
  s = static_cast<uint16_t>(s ^ (s >> 9));
  s = static_cast<uint16_t>(s ^ (s << 8));
  if (s == 0) s = 0xACE1;
  return s;
}
uint8_t seeded_random8(uint16_t &seed) { return static_cast<uint8_t>(xorshift16(seed)); }
uint8_t seeded_random8(uint16_t &seed, uint8_t max) {
  return max ? static_cast<uint8_t>(xorshift16(seed) % max) : 0;
}

// WLED's perlin16()/perlin8() (really FastLED's inoise16()/inoise8()) -
// classic lattice-gradient Perlin noise with permutation tables isn't
// ported bit-exact (see this file's top comment); this is a hash-based
// value-noise with trilinear interpolation + smoothstep easing instead. It
// takes coordinates in the same Q16.16 fixed-point convention FastLED's own
// noise functions use, and produces the same continuous, per-pixel-smooth
// character every mode_fillnoise8()/mode_noise16_*() below relies on.
uint32_t noise_hash(uint32_t x, uint32_t y, uint32_t z) {
  uint32_t h = x * 374761393u + y * 668265263u + z * 2147483647u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return h ^ (h >> 16);
}
double noise_lattice(int32_t xi, int32_t yi, int32_t zi) {
  uint32_t h = noise_hash(static_cast<uint32_t>(xi), static_cast<uint32_t>(yi),
                           static_cast<uint32_t>(zi));
  return (h & 0xFFFFFFu) / static_cast<double>(0xFFFFFFu);
}
double noise_smooth(double t) { return t * t * (3.0 - 2.0 * t); }
double value_noise3d(double x, double y, double z) {
  int32_t xi = static_cast<int32_t>(std::floor(x));
  int32_t yi = static_cast<int32_t>(std::floor(y));
  int32_t zi = static_cast<int32_t>(std::floor(z));
  double u = noise_smooth(x - xi), v = noise_smooth(y - yi), w = noise_smooth(z - zi);

  double c000 = noise_lattice(xi, yi, zi);
  double c100 = noise_lattice(xi + 1, yi, zi);
  double c010 = noise_lattice(xi, yi + 1, zi);
  double c110 = noise_lattice(xi + 1, yi + 1, zi);
  double c001 = noise_lattice(xi, yi, zi + 1);
  double c101 = noise_lattice(xi + 1, yi, zi + 1);
  double c011 = noise_lattice(xi, yi + 1, zi + 1);
  double c111 = noise_lattice(xi + 1, yi + 1, zi + 1);

  double x00 = c000 + u * (c100 - c000);
  double x10 = c010 + u * (c110 - c010);
  double x01 = c001 + u * (c101 - c001);
  double x11 = c011 + u * (c111 - c011);
  double y0 = x00 + v * (x10 - x00);
  double y1 = x01 + v * (x11 - x01);
  return y0 + w * (y1 - y0);
}
uint16_t perlin16(uint32_t x, uint32_t y = 0, uint32_t z = 0) {
  double n = value_noise3d(x / 65536.0, y / 65536.0, z / 65536.0);
  return static_cast<uint16_t>(n * 65535.0);
}
uint8_t perlin8(uint32_t x, uint32_t y = 0, uint32_t z = 0) {
  return static_cast<uint8_t>(perlin16(x, y, z) >> 8);
}

// wled00/FX.cpp:1625 mode_icu()
void mode_icu(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  uint16_t now16 = static_cast<uint16_t>(now_ms);
  unsigned space = (p.intensity >> 3) + 2;
  uint16_t st = static_cast<uint16_t>(state.step >> 16);
  uint16_t next_update = static_cast<uint16_t>(state.step & 0xFFFF);
  unsigned dest = state.aux1;

  unsigned denom = static_cast<unsigned>(width) - static_cast<unsigned>(width) / space;
  uint8_t pindex = denom ? static_cast<uint8_t>((dest * 255) / denom) : 0;
  Rgb col = color_from_palette(p.palette_id, pindex, p.primary, p.secondary, p.tertiary);
  Rgb bgcol = p.option2 ? Rgb{0, 0, 0} : p.secondary;
  for (int x = 0; x < width; x++) fill_column(frame, x, bgcol);

  if (st != 1) {
    fill_column(frame, static_cast<int>(dest % width), col);
    fill_column(frame, static_cast<int>((dest + width / space) % width), col);
    if (st == 3) {
      if (state.aux0 > state.aux1) dest++;
      else if (state.aux0 < state.aux1) dest--;
      fill_column(frame, static_cast<int>(dest % width), col);
      fill_column(frame, static_cast<int>((dest + width / space) % width), col);
    }
  }

  if (static_cast<int16_t>(now16 - next_update) >= 0) {
    switch (st) {
      case 0:
        st++;
        if (random8(6) == 0) {
          next_update = static_cast<uint16_t>(now16 + 200);
          break;
        }
        [[fallthrough]];
      case 1:
        next_update = static_cast<uint16_t>(now16 + 500 + random16(1000));
        st++;
        break;
      case 2:
        state.aux0 = random16(static_cast<uint16_t>(width - width / space));
        next_update = now16;
        st++;
        break;
      default: {
        unsigned speed_formula_l = 5 + (50u * (255u - p.speed)) / static_cast<unsigned>(width);
        state.aux1 = static_cast<uint16_t>(dest);
        next_update = static_cast<uint16_t>(now16 + speed_formula_l);
        if (state.aux0 == dest) {
          next_update = static_cast<uint16_t>(now16 + 500 + random16(1000));
          st = 0;
        }
        break;
      }
    }
  }
  state.step = (static_cast<uint32_t>(st) << 16) | next_update;
}
EFFECTS_REGISTER(Id::kIcu, mode_icu)

struct MultiCometState {
  uint16_t comets[8];
};
static_assert(sizeof(MultiCometState) <= State::kDataSize, "MultiCometState too big");

// wled00/FX.cpp:1778 mode_multi_comet()
void mode_multi_comet(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  uint32_t cycle_time = 10 + (255u - p.speed);
  uint32_t it = now_ms / cycle_time;
  if (state.step == it) return;

  fade_to_black_by(frame, static_cast<uint8_t>(p.intensity / 2 + 128));

  auto &s = *reinterpret_cast<MultiCometState *>(state.data);
  bool tertiary_set = p.tertiary.r || p.tertiary.g || p.tertiary.b;
  for (int i = 0; i < 8; i++) {
    if (s.comets[i] < width) {
      int index = s.comets[i];
      Rgb c = color_from_palette(p.palette_id, palette_index_mapped(index, width), p.primary,
                                  p.secondary, p.tertiary);
      Rgb col = (tertiary_set && (i % 2)) ? p.tertiary : c;
      fill_column(frame, index, col);
      s.comets[i]++;
    } else if (random16(static_cast<uint16_t>(width)) == 0) {
      s.comets[i] = 0;
    }
  }
  state.step = it;
}
EFFECTS_REGISTER(Id::kMultiComet, mode_multi_comet)

// wled00/FX.cpp:1209 mode_larson_scanner(), ported directly with check1
// forced true - matches wled00/FX.cpp:1256 mode_dual_larson_scanner()'s
// `SEGMENT.check1 = true; mode_larson_scanner();`. FRAMETIME (WLED's
// measured actual per-frame delta) has no equivalent here since this
// firmware isn't frame-stepped - approximated with WLED_FPS's fixed
// 1000/42ms (kFrametimeFixedMs above).
void mode_dual_larson_scanner(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  unsigned speed = kFrametimeFixedMs * (96u - (static_cast<unsigned>(p.speed) * 94u) / 255u);
  unsigned pixels = speed ? static_cast<unsigned>(width) / speed : 0;

  fade_to_black_by(frame, static_cast<uint8_t>(255 - p.intensity));

  if (state.step > now_ms) return;

  unsigned index = state.aux1 + pixels;
  if (pixels == 0) {
    unsigned frames = speed ? speed / static_cast<unsigned>(width) : 0;
    if (state.step++ < frames) return;
    state.step = 0;
    index++;
  }

  bool tertiary_set = p.tertiary.r || p.tertiary.g || p.tertiary.b;
  if (index > static_cast<unsigned>(width)) {
    state.aux0 = static_cast<uint16_t>(!state.aux0);
    state.aux1 = 0;
    if (state.aux0 || p.option2) state.step = now_ms + p.custom1 * 25u;
    else state.step = 0;
  } else {
    for (unsigned i = state.aux1; i < index; i++) {
      unsigned j = state.aux0 ? i : static_cast<unsigned>(width) - 1 - i;
      Rgb c = color_from_palette(p.palette_id, palette_index_mapped(static_cast<int>(j), width),
                                  p.primary, p.secondary, p.tertiary);
      fill_column(frame, static_cast<int>(j), c);
      fill_column(frame, width - 1 - static_cast<int>(j), tertiary_set ? p.tertiary : c);
    }
    state.aux1 = static_cast<uint16_t>(index);
  }
}
EFFECTS_REGISTER(Id::kDualLarsonScanner, mode_dual_larson_scanner)

// wled00/FX.cpp:1815 mode_random_chase(). WLED keys this off a
// segment-local seedable PRNG (Segment::prng) rather than the shared
// random8()/random16() stream, so ported using the xorshift16 helper above
// (seeded from state.aux0) instead - see that helper's comment.
void mode_random_chase(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  if (state.call == 0) {
    state.step = (static_cast<uint32_t>(random8()) << 16) | (static_cast<uint32_t>(random8()) << 8) |
                 random8();
    state.aux0 = random16();
  }
  uint32_t cycle_time = 25 + 3u * (255u - p.speed);
  uint32_t it = now_ms / cycle_time;
  uint32_t color = state.step;
  uint16_t seed = state.aux0;

  for (int i = width - 1; i >= 0; i--) {
    uint8_t r = seeded_random8(seed, 6) != 0 ? static_cast<uint8_t>((color >> 16) & 0xFF)
                                              : seeded_random8(seed);
    uint8_t g = seeded_random8(seed, 6) != 0 ? static_cast<uint8_t>((color >> 8) & 0xFF)
                                              : seeded_random8(seed);
    uint8_t b = seeded_random8(seed, 6) != 0 ? static_cast<uint8_t>(color & 0xFF) : seeded_random8(seed);
    color = (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | b;
    fill_column(frame, i, Rgb{r, g, b});
    if (i == width - 1 && state.aux1 != (it & 0xFFFFu)) {
      state.step = color;
      state.aux0 = seed;
    }
  }
  state.aux1 = static_cast<uint16_t>(it & 0xFFFFu);
}
EFFECTS_REGISTER(Id::kRandomChase, mode_random_chase)

struct Oscillator {
  uint16_t pos;
  uint8_t size;
  int8_t dir;
  uint8_t speed;
};
struct OscillateState {
  Oscillator osc[3];
};
static_assert(sizeof(OscillateState) <= State::kDataSize, "OscillateState too big");

// wled00/FX.cpp:1856 mode_oscillate()
void mode_oscillate(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  auto &s = *reinterpret_cast<OscillateState *>(state.data);
  if (state.call == 0) {
    s.osc[0] = {static_cast<uint16_t>(width / 4), static_cast<uint8_t>(width / 8), 1, 1};
    s.osc[1] = {static_cast<uint16_t>(width / 4 * 3), static_cast<uint8_t>(width / 8), 1, 2};
    s.osc[2] = {static_cast<uint16_t>(width / 4 * 2), static_cast<uint8_t>(width / 8), -1, 1};
  }
  uint32_t cycle_time = 20 + 2u * (255u - p.speed);
  uint32_t it = now_ms / cycle_time;

  for (int i = 0; i < 3; i++) {
    if (it != state.step) {
      s.osc[i].pos = static_cast<uint16_t>(s.osc[i].pos + s.osc[i].dir * s.osc[i].speed);
    }
    s.osc[i].size = static_cast<uint8_t>(width / (3 + p.intensity / 8));
    if (s.osc[i].dir == -1 && s.osc[i].pos > static_cast<uint16_t>(width << 1)) {
      s.osc[i].pos = 0;
      s.osc[i].dir = 1;
      s.osc[i].speed = p.speed > 100 ? random8(2, 4) : random8(1, 3);
    }
    if (s.osc[i].dir == 1 && s.osc[i].pos >= width - 1) {
      s.osc[i].pos = width - 1;
      s.osc[i].dir = -1;
      s.osc[i].speed = p.speed > 100 ? random8(2, 4) : random8(1, 3);
    }
  }

  Rgb colors[3] = {p.primary, p.secondary, p.tertiary};
  for (int x = 0; x < width; x++) {
    Rgb color{0, 0, 0};
    bool any = false;
    for (int j = 0; j < 3; j++) {
      if (x >= static_cast<int>(s.osc[j].pos) - s.osc[j].size && x <= s.osc[j].pos + s.osc[j].size) {
        color = any ? blend(color, colors[j], 128) : colors[j];
        any = true;
      }
    }
    fill_column(frame, x, color);
  }
  state.step = it;
}
EFFECTS_REGISTER(Id::kOscillate, mode_oscillate)

// wled00/FX.cpp:1948 mode_colorwaves_pride_base(), shared by
// mode_pride_2015() (wled00/FX.cpp:1999) and mode_colorwaves()
// (wled00/FX.cpp:2007). gamma32inv is dropped from the Pride2015 branch -
// this codebase has no equivalent display gamma table to invert against.
void colorwaves_pride_base(bool is_pride, uint32_t now_ms, const Params &p, State &state,
                            Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  unsigned duration = 10 + p.speed;
  unsigned s_pseudotime = state.step;
  unsigned s_hue16 = state.aux0;

  uint8_t sat8 = is_pride ? static_cast<uint8_t>(beatsin88(now_ms, 87, 220, 250)) : 255;
  unsigned brightdepth = beatsin88(now_ms, 341, 96, 224);
  unsigned brightnessthetainc16 = beatsin88(now_ms, 203, 25 * 256, 40 * 256);
  unsigned msmultiplier = beatsin88(now_ms, 147, 23, 60);

  unsigned hue16 = s_hue16;
  unsigned hueinc16 = is_pride ? beatsin88(now_ms, 113, 1, 3000)
                                : beatsin88(now_ms, 113, 60, 300) * p.intensity * 10u / 255u;

  s_pseudotime += duration * msmultiplier;
  s_hue16 += duration * beatsin88(now_ms, 400, 5, 9);
  unsigned brightnesstheta16 = s_pseudotime;

  for (int i = 0; i < width; i++) {
    hue16 += hueinc16;
    uint8_t hue8;
    if (is_pride) {
      hue8 = static_cast<uint8_t>(hue16 >> 8);
    } else {
      unsigned h16_128 = hue16 >> 7;
      hue8 = (h16_128 & 0x100) ? static_cast<uint8_t>(255 - (h16_128 >> 1))
                               : static_cast<uint8_t>(h16_128 >> 1);
    }

    brightnesstheta16 += brightnessthetainc16;
    unsigned b16 = static_cast<unsigned>(sin16(static_cast<uint16_t>(brightnesstheta16)) + 32768);
    unsigned bri16 = (b16 * b16) / 65536u;
    uint8_t bri8 = static_cast<uint8_t>((bri16 * brightdepth) / 65536u);
    bri8 = static_cast<uint8_t>(bri8 + (255 - brightdepth));

    Rgb newcolor;
    uint8_t blend_amount;
    if (is_pride) {
      newcolor = hsv_to_rgb(hue8, sat8, bri8);
      blend_amount = 64;
    } else {
      newcolor = color_from_palette(p.palette_id, hue8, p.primary, p.secondary, p.tertiary);
      newcolor.r = scale8(newcolor.r, bri8);
      newcolor.g = scale8(newcolor.g, bri8);
      newcolor.b = scale8(newcolor.b, bri8);
      blend_amount = 128;
    }
    Rgb blended = blend(frame[0][i], newcolor, blend_amount);
    fill_column(frame, i, blended);
  }

  state.step = s_pseudotime;
  state.aux0 = static_cast<uint16_t>(s_hue16);
}

// wled00/FX.cpp:1999 mode_pride_2015()
void mode_pride_2015(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  colorwaves_pride_base(true, now_ms, p, state, frame);
}
EFFECTS_REGISTER(Id::kPride2015, mode_pride_2015)

// wled00/FX.cpp:2014 mode_juggle(). WLED's CRGB::operator|=() ORs the two
// colors channel-by-channel (letting multiple dots' colors combine where
// they overlap) - ported as a plain bitwise OR per channel, matching the
// original. Segment::palette==0's raw-HSV special case is dropped -
// palettes.h already documents that this firmware always resolves palette 0
// to PartyColors rather than replicating every per-effect palette-0 quirk.
void mode_juggle(uint32_t now_ms, const Params &p, State &, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  fade_to_black_by(frame, static_cast<uint8_t>(192 - 3 * p.intensity / 4));
  uint8_t dothue = 0;
  for (int i = 0; i < 8; i++) {
    int index = beatsin88(now_ms, static_cast<uint16_t>((16 + p.speed) * (i + 7)), 0, width - 1);
    Rgb cur = frame[0][index];
    Rgb dot = color_from_palette(p.palette_id, dothue, p.primary, p.secondary, p.tertiary);
    Rgb out{static_cast<uint8_t>(cur.r | dot.r), static_cast<uint8_t>(cur.g | dot.g),
            static_cast<uint8_t>(cur.b | dot.b)};
    fill_column(frame, index, out);
    dothue = static_cast<uint8_t>(dothue + 32);
  }
}
EFFECTS_REGISTER(Id::kJuggle, mode_juggle)

// wled00/FX.cpp:2031 mode_palette(). Ported from WLED's own non-ESP8266
// float-math branch (the ESP8266 fixed-point branch is functionally
// identical, just faster on that particular MCU) addressing the full 32x32
// matrix directly - matches WLED's own isMatrix=true path.
void mode_palette(uint32_t now_ms, const Params &p, State &, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  constexpr int height = GuDisplay::HEIGHT;
  const int input_shift = p.speed;
  const int input_size = p.intensity;
  const int input_rotation = p.custom1;
  const bool animate_shift = p.option1;
  const bool animate_rotation = p.option2;
  const bool assume_square = p.option3;

  constexpr double kMaxAngle = M_PI / 256.0;
  double theta = !animate_rotation
                     ? (input_rotation + 128) * kMaxAngle
                     : (static_cast<uint32_t>(now_ms * ((input_rotation >> 4) + 1)) & 0xFFFFu) *
                           (2.0 * M_PI / 65536.0);
  double sin_theta = std::sin(theta);
  double cos_theta = std::cos(theta);

  double max_x = width - 1;
  double max_y = height - 1;
  double max_x_in = assume_square ? max_x : 1.0;
  double max_y_in = assume_square ? max_y : 1.0;
  double max_x_out = !assume_square ? max_x : 1.0;
  double max_y_out = !assume_square ? max_y : 1.0;
  double center_x = max_x_out / 2.0;
  double center_y = max_y_out / 2.0;
  double scale = std::fabs(sin_theta) + std::fabs(cos_theta) * max_y_out / max_x_out;

  for (int y = 0; y < height; y++) {
    double yt_cos_theta = (cos_theta * (y - center_y * max_y_in)) / (max_y_in * scale);
    for (int x = 0; x < width; x++) {
      double xt_sin_theta = (sin_theta * (x - center_x * max_x_in)) / (max_x_in * scale);
      double source_x = xt_sin_theta + yt_cos_theta + center_x;
      double clamped = source_x < 0 ? 0 : (source_x > max_x_out ? max_x_out : source_x);
      int color_index = static_cast<int>((clamped * 255.0) / max_x_out);
      if (input_size <= 128) {
        color_index = (color_index * input_size) / 128;
      } else {
        color_index = ((input_size - 112) * color_index) / 16;
      }
      int palette_offset =
          !animate_shift
              ? input_shift
              : static_cast<int>((static_cast<uint32_t>(now_ms * ((input_shift >> 3) + 1)) &
                                   0xFFFFu) >>
                                  8);
      color_index -= palette_offset;
      Rgb color = color_from_palette(p.palette_id, static_cast<uint8_t>(color_index), p.primary,
                                      p.secondary, p.tertiary);
      frame[y][x] = color;
    }
  }
}
EFFECTS_REGISTER(Id::kPalette, mode_palette)

struct Fire2012State {
  uint8_t heat[GuDisplay::WIDTH][GuDisplay::HEIGHT];
};
static_assert(sizeof(Fire2012State) <= State::kDataSize, "Fire2012State too big");

// wled00/FX.cpp:2157 mode_fire_2012(). WLED runs one independent 1D fire
// simulation per matrix column on hardware like this (Segment::nrOfVStrips()'s
// "virtual strip" trick) - ported directly as a per-column vertical fire,
// row HEIGHT-1 = base of the flame, row 0 = tip. The optional 2D side-blur
// (custom2) is dropped - heat[][] already fills all of State::data, leaving
// no room for anything else, and the per-column fire alone already reads as
// a real 2D flame on this 32x32 matrix.
void mode_fire_2012(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  constexpr int len = GuDisplay::HEIGHT;
  auto &s = *reinterpret_cast<Fire2012State *>(state.data);
  uint32_t it = now_ms >> 5;
  bool new_frame = (it != state.step);
  uint8_t ignition = len / 10 > 3 ? static_cast<uint8_t>(len / 10) : 3;

  for (int col = 0; col < width; col++) {
    uint8_t *heat = s.heat[col];

    for (int i = 0; i < len; i++) {
      uint8_t cool = new_frame ? random8(static_cast<uint8_t>(((20 + p.speed / 3) * 16) / len + 2))
                                : random8(4);
      uint8_t min_temp = (i < ignition) ? static_cast<uint8_t>((ignition - i) / 4 + 16) : 0;
      uint8_t temp = qsub8(heat[i], cool);
      heat[i] = temp < min_temp ? min_temp : temp;
    }

    if (new_frame) {
      for (int k = len - 1; k > 1; k--) {
        heat[k] = static_cast<uint8_t>((heat[k - 1] + (heat[k - 2] << 1)) / 3);
      }
      if (random8() <= p.intensity) {
        uint8_t y = random8(ignition);
        uint8_t boost = static_cast<uint8_t>((17 + p.custom3) * (ignition - y / 2) / ignition);
        heat[y] = qadd8(heat[y], random8(static_cast<uint8_t>(96 + 2 * boost),
                                          static_cast<uint8_t>(207 + boost)));
      }
    }

    for (int j = 0; j < len; j++) {
      uint8_t heat_val = heat[j] < 240 ? heat[j] : 240;
      Rgb color = color_from_palette(p.palette_id, heat_val, p.primary, p.secondary, p.tertiary);
      frame[len - 1 - j][col] = color;
    }
  }
  if (new_frame) state.step = it;
}
EFFECTS_REGISTER(Id::kFire2012, mode_fire_2012)

// wled00/FX.cpp:2007 mode_colorwaves()
void mode_colorwaves(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  colorwaves_pride_base(false, now_ms, p, state, frame);
}
EFFECTS_REGISTER(Id::kColorwaves, mode_colorwaves)

// wled00/FX.cpp:2216 mode_bpm()
void mode_bpm(uint32_t now_ms, const Params &p, State &, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  uint8_t stp = static_cast<uint8_t>((now_ms / 20) & 0xFF);
  uint8_t beat = beatsin8(now_ms, p.speed, 64, 255);
  for (int i = 0; i < width; i++) {
    uint8_t index = static_cast<uint8_t>(stp + i * 2);
    uint8_t bri = static_cast<uint8_t>(beat - stp + i * 10);
    Rgb color = color_from_palette(p.palette_id, index, p.primary, p.secondary, p.tertiary);
    color.r = scale8(color.r, bri);
    color.g = scale8(color.g, bri);
    color.b = scale8(color.b, bri);
    fill_column(frame, i, color);
  }
}
EFFECTS_REGISTER(Id::kBpm, mode_bpm)

// wled00/FX.cpp:2226 mode_fillnoise8()
void mode_fillnoise8(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  if (state.call == 0) state.step = random16();
  for (int i = 0; i < width; i++) {
    uint8_t index = perlin8(static_cast<uint32_t>(i) * width, state.step + static_cast<uint32_t>(i) * width);
    Rgb color = color_from_palette(p.palette_id, index, p.primary, p.secondary, p.tertiary);
    fill_column(frame, i, color);
  }
  state.step += beatsin8(now_ms, p.speed, 1, 6);
}
EFFECTS_REGISTER(Id::kFillnoise8, mode_fillnoise8)

// wled00/FX.cpp:4361 mode_noisepal(). Drops the "generate 2 random
// CRGBPalette16s and cross-fade between them every 4-6.5s" machinery real
// WLED falls back to only when no palette is explicitly selected (it uses
// SEGPALETTE directly otherwise) - this firmware's palette 0 is already a
// colorful default (same precedent as gen_batch2.cpp's mode_popcorn), so
// every pixel just looks its noise value up through whichever palette is
// selected, same shape as mode_fillnoise8() just above. Real WLED's Speed
// slider only controls how often a new random palette gets picked, which
// has nothing left to do here as a result - unused, same as its "Scale"
// (this port's Intensity) is the one that actually matters, per the
// original's own "@!,Scale" parameter labels.
void mode_noisepal(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  unsigned scale = 15 + (p.intensity >> 2);
  for (int i = 0; i < width; i++) {
    uint8_t index = perlin8(static_cast<uint32_t>(i) * scale, state.aux0 + static_cast<uint32_t>(i) * scale);
    fill_column(frame, i, color_from_palette(p.palette_id, index, p.primary, p.secondary, p.tertiary));
  }
  state.aux0 = static_cast<uint16_t>(state.aux0 + beatsin8(now_ms, 10, 1, 4));
}
EFFECTS_REGISTER(Id::kNoisepal, mode_noisepal)

// wled00/FX.cpp:2237 mode_noise16_1()
void mode_noise16_1(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  constexpr unsigned scale = 320;
  state.step += 1 + p.speed / 16;
  for (int i = 0; i < width; i++) {
    unsigned shift_x = beatsin8(now_ms, 11);
    unsigned shift_y = state.step / 42;
    uint32_t real_x = (static_cast<uint32_t>(i) + shift_x) * scale;
    uint32_t real_y = (static_cast<uint32_t>(i) + shift_y) * scale;
    uint32_t real_z = state.step;
    unsigned noise = perlin16(real_x, real_y, real_z) >> 8;
    uint8_t index = sin8(static_cast<uint8_t>(noise * 3));
    Rgb color = color_from_palette(p.palette_id, index, p.primary, p.secondary, p.tertiary);
    fill_column(frame, i, color);
  }
}
EFFECTS_REGISTER(Id::kNoise161, mode_noise16_1)

// wled00/FX.cpp:2256 mode_noise16_2()
void mode_noise16_2(uint32_t, const Params &p, State &state, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  constexpr unsigned scale = 1000;
  state.step += 1 + (p.speed >> 1);
  for (int i = 0; i < width; i++) {
    unsigned shift_x = state.step >> 6;
    uint32_t real_x = (static_cast<uint32_t>(i) + shift_x) * scale;
    unsigned noise = perlin16(real_x, 0, 4223) >> 8;
    uint8_t index = sin8(static_cast<uint8_t>(noise * 3));
    Rgb color = color_from_palette(p.palette_id, index, p.primary, p.secondary, p.tertiary);
    color.r = scale8(color.r, static_cast<uint8_t>(noise));
    color.g = scale8(color.g, static_cast<uint8_t>(noise));
    color.b = scale8(color.b, static_cast<uint8_t>(noise));
    fill_column(frame, i, color);
  }
}
EFFECTS_REGISTER(Id::kNoise162, mode_noise16_2)

// wled00/FX.cpp:2272 mode_noise16_3()
void mode_noise16_3(uint32_t, const Params &p, State &state, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  constexpr unsigned scale = 800;
  state.step += 1 + p.speed;
  for (int i = 0; i < width; i++) {
    constexpr unsigned shift_x = 4223;
    constexpr unsigned shift_y = 1234;
    uint32_t real_x = (static_cast<uint32_t>(i) + shift_x) * scale;
    uint32_t real_y = (static_cast<uint32_t>(i) + shift_y) * scale;
    uint32_t real_z = state.step * 8;
    unsigned noise = perlin16(real_x, real_y, real_z) >> 8;
    uint8_t index = sin8(static_cast<uint8_t>(noise * 3));
    Rgb color = color_from_palette(p.palette_id, index, p.primary, p.secondary, p.tertiary);
    color.r = scale8(color.r, static_cast<uint8_t>(noise));
    color.g = scale8(color.g, static_cast<uint8_t>(noise));
    color.b = scale8(color.b, static_cast<uint8_t>(noise));
    fill_column(frame, i, color);
  }
}
EFFECTS_REGISTER(Id::kNoise163, mode_noise16_3)

// wled00/FX.cpp:2292 mode_noise16_4()
void mode_noise16_4(uint32_t now_ms, const Params &p, State &, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  uint32_t stp = (now_ms * p.speed) >> 7;
  for (int i = 0; i < width; i++) {
    uint16_t index = perlin16(static_cast<uint32_t>(i) << 12, stp);
    Rgb color = color_from_palette(p.palette_id, static_cast<uint8_t>(index), p.primary, p.secondary,
                                    p.tertiary);
    fill_column(frame, i, color);
  }
}
EFFECTS_REGISTER(Id::kNoise164, mode_noise16_4)

struct ColortwinkleState {
  uint8_t bits[(GuDisplay::WIDTH + 7) / 8];
};
static_assert(sizeof(ColortwinkleState) <= State::kDataSize, "ColortwinkleState too big");

// wled00/FX.cpp:2303 mode_colortwinkle(). Drops strip.getBrightness() (no
// global-brightness readout available here - assumes the >28 branch, WLED's
// own default for anything but a very dim strip) and gamma8inv/color_fade's
// "video" scaling (approximated with plain scale8/qadd8, per this file's
// header note on non-bit-exact helpers).
void mode_colortwinkle(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  auto &s = *reinterpret_cast<ColortwinkleState *>(state.data);

  if (now_ms - state.step < kFrametimeFixedMs) return;
  state.step = now_ms;

  uint8_t fade_up_amount = static_cast<uint8_t>(8 + (p.speed >> 2));
  uint8_t fade_down_amount = static_cast<uint8_t>(8 + (p.speed >> 3));

  for (int x = 0; x < width; x++) {
    Rgb cur = frame[0][x];
    int index = x >> 3;
    int bit_num = x & 7;
    bool fade_up = (s.bits[index] >> bit_num) & 1;

    Rgb col;
    if (fade_up) {
      uint8_t inc_r = scale8(cur.r, fade_up_amount);
      uint8_t inc_g = scale8(cur.g, fade_up_amount);
      uint8_t inc_b = scale8(cur.b, fade_up_amount);
      col = Rgb{qadd8(cur.r, inc_r), qadd8(cur.g, inc_g), qadd8(cur.b, inc_b)};
      if (col.r == 255 || col.g == 255 || col.b == 255) {
        s.bits[index] = static_cast<uint8_t>(s.bits[index] & ~(1 << bit_num));
      }
      if (col.r == cur.r && col.g == cur.g && col.b == cur.b) {
        col = Rgb{qadd8(col.r, col.r), qadd8(col.g, col.g), qadd8(col.b, col.b)};
      }
    } else {
      uint8_t keep = static_cast<uint8_t>(255 - fade_down_amount);
      col = Rgb{scale8(cur.r, keep), scale8(cur.g, keep), scale8(cur.b, keep)};
    }
    fill_column(frame, x, col);
  }

  for (int j = 0; j <= width / 50; j++) {
    if (random8() <= p.intensity) {
      for (int t = 0; t < 5; t++) {
        int x = random16(static_cast<uint16_t>(width));
        Rgb cur = frame[0][x];
        if (cur.r == 0 && cur.g == 0 && cur.b == 0) {
          int index = x >> 3;
          int bit_num = x & 7;
          s.bits[index] = static_cast<uint8_t>(s.bits[index] | (1 << bit_num));
          Rgb spawn = color_from_palette(p.palette_id, random8(), p.primary, p.secondary, p.tertiary);
          spawn.r = scale8(spawn.r, 64);
          spawn.g = scale8(spawn.g, 64);
          spawn.b = scale8(spawn.b, 64);
          fill_column(frame, x, spawn);
          break;
        }
      }
    }
  }
}
EFFECTS_REGISTER(Id::kColortwinkle, mode_colortwinkle)

// wled00/FX.cpp:2359 mode_lake(). beatsin8_t(bpm,-64,64)'s -64 argument
// implicitly narrows to a uint8_t (192) at the call site in real WLED too -
// wled_compat.h's beatsin8() does the same `low + scale8(s, high-low)` math
// with the same uint8_t wraparound, so passing (uint8_t)-64 here reproduces
// the same wrapped-range trick without special-casing it.
void mode_lake(uint32_t now_ms, const Params &p, State &, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  uint8_t sp = static_cast<uint8_t>(p.speed / 10);
  int wave1 = beatsin8(now_ms, static_cast<uint8_t>(sp + 2), static_cast<uint8_t>(-64), 64);
  int wave2 = beatsin8(now_ms, static_cast<uint8_t>(sp + 1), static_cast<uint8_t>(-64), 64);
  int wave3 = beatsin8(now_ms, static_cast<uint8_t>(sp + 2), 0, 80);

  for (int i = 0; i < width; i++) {
    int index = cos8(static_cast<uint8_t>(i * 15 + wave1)) / 2 +
                cubicwave8(static_cast<uint8_t>(i * 23 + wave2)) / 2;
    uint8_t lum = index > wave3 ? static_cast<uint8_t>(index - wave3) : 0;
    Rgb color = color_from_palette(p.palette_id, static_cast<uint8_t>(index), p.primary, p.secondary,
                                    p.tertiary);
    color.r = scale8(color.r, lum);
    color.g = scale8(color.g, lum);
    color.b = scale8(color.b, lum);
    fill_column(frame, i, color);
  }
}
EFFECTS_REGISTER(Id::kLake, mode_lake)

}  // namespace
}  // namespace effects
