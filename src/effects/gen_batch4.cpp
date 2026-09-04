#include "effects.h"

#include <math.h>

#include "display/gu_display.h"
#include "palettes.h"
#include "wled_compat.h"

namespace effects {
namespace {

// --- shared helpers (several mode_ functions in this batch need the same
// WLED building blocks; duplicated locally since effects.cpp's own copies
// of some of these, e.g. color_wheel(), have internal linkage) ---

uint8_t clamp_u8(int v) {
  if (v < 0) return 0;
  if (v > 255) return 255;
  return static_cast<uint8_t>(v);
}

// wled00/FX_fcn.cpp:1141 Segment::color_wheel() - the plain HSV-wheel
// branch only (the "if (palette) return color_from_palette(...)" branch
// is dropped, matching this file's mode_rainbow_cycle() precedent in
// effects.cpp, which simplifies the same function the same way).
Rgb color_wheel(uint8_t pos) {
  float h = pos * 360.0f / 255.0f;
  float c = 1.0f;
  float x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
  float rp, gp, bp;
  if (h < 60)       { rp = c;  gp = x;  bp = 0; }
  else if (h < 120) { rp = x;  gp = c;  bp = 0; }
  else if (h < 180) { rp = 0;  gp = c;  bp = x; }
  else if (h < 240) { rp = 0;  gp = x;  bp = c; }
  else if (h < 300) { rp = x;  gp = 0;  bp = c; }
  else              { rp = c;  gp = 0;  bp = x; }
  return Rgb{clamp_u8(static_cast<int>(rp * 255)),
             clamp_u8(static_cast<int>(gp * 255)),
             clamp_u8(static_cast<int>(bp * 255))};
}

// wled00/FX_fcn.cpp:1158 Segment::color_from_palette() ported onto this
// codebase's effects::color_from_palette() - `mcol` (which segment color
// to fall back to for the "Default" palette) is dropped since our
// palette 0 always resolves the same way regardless of it (see
// palettes.h); `pbri` (post-lookup brightness scale) is applied here,
// per the porting cheat-sheet, since our color_from_palette() has no
// brightness argument of its own.
Rgb pal(const Params &p, uint8_t index, uint8_t bri = 255) {
  Rgb c = color_from_palette(p.palette_id, index, p.primary, p.secondary, p.tertiary);
  if (bri == 255) return c;
  return Rgb{scale8(c.r, bri), scale8(c.g, bri), scale8(c.b, bri)};
}

// The "mapping=true" case of the function above: palette index scaled by
// position-in-strip rather than used directly.
uint8_t map_index(int i, int len) {
  int v = (i * 255) / len;
  if (v < 0) return 0;
  if (v > 255) return 255;
  return static_cast<uint8_t>(v);
}

// FastLED triwave8()/triwave16(): a symmetrical rise/fall triangle wave
// over the full 8/16-bit input range.
uint8_t triwave8(uint8_t in) {
  uint8_t v = in;
  if (v & 0x80) v = static_cast<uint8_t>(255 - v);
  return static_cast<uint8_t>(v << 1);
}
uint16_t triwave16(uint16_t in) {
  uint16_t v = in;
  if (v & 0x8000) v = static_cast<uint16_t>(65535 - v);
  return static_cast<uint16_t>(v << 1);
}

// FastLED cubicwave8() = ease8InOutCubic(triwave8(x)). ease8InOutCubic's
// scale8_video (a "never rounds down to zero if the input wasn't zero"
// variant) is approximated with plain scale8 - a one-off rounding
// difference invisible in the trail-width curve this feeds.
uint8_t cubicwave8(uint8_t in) {
  uint8_t t = triwave8(in);
  uint8_t ii = scale8(t, t);
  uint8_t iii = scale8(ii, t);
  int r1 = 3 * static_cast<int>(ii) - 2 * static_cast<int>(iii);
  if (r1 < 0) r1 = 0;
  if (r1 > 255) r1 = 255;
  if (t == 0) r1 = 0;
  if (t == 255) r1 = 255;
  return static_cast<uint8_t>(r1);
}

// wled00/FX_fcn.cpp:1067 Segment::fade_out(): fades every pixel toward
// `target` (real WLED's SEGCOLOR(1)) rather than toward black the way
// wled_compat.h's fade_to_black_by() does - the two effects in this batch
// that call it (ripple, sinelon) both always target the segment's
// secondary color, so that's the only case ported.
void fade_out_toward(Frame frame, uint8_t rate, Rgb target) {
  constexpr int width = GuDisplay::WIDTH;
  int r2 = (256 - rate) >> 1;
  int mapped_rate = 256 / (r2 + 1);
  for (int x = 0; x < width; x++) {
    Rgb c = frame[0][x];
    if (c.r == target.r && c.g == target.g && c.b == target.b) continue;
    auto fade_channel = [&](uint8_t c1, uint8_t c2) -> uint8_t {
      int delta = (static_cast<int>(c2) - static_cast<int>(c1)) * mapped_rate / 256;
      if (delta == 0) delta += (c2 == c1) ? 0 : (c2 > c1 ? 1 : -1);
      int v = c1 + delta;
      if (v < 0) v = 0;
      if (v > 255) v = 255;
      return static_cast<uint8_t>(v);
    };
    fill_column(frame, x, Rgb{fade_channel(c.r, target.r), fade_channel(c.g, target.g),
                               fade_channel(c.b, target.b)});
  }
}

// wled00/FX_fcn.cpp:1108 Segment::blur() - the 1D (non-matrix) branch
// only; the effect in this batch that uses it (ripple) is ported as a
// de facto 1D effect (see mode_ripple()'s own comment).
void blur1d(Frame frame, uint8_t amount) {
  constexpr int width = GuDisplay::WIDTH;
  if (amount == 0) return;
  uint8_t keep = static_cast<uint8_t>(255 - amount);
  uint8_t seep = static_cast<uint8_t>(amount >> 1);
  Rgb buf[width];
  Rgb cur = frame[0][0];
  Rgb carry{scale8(cur.r, seep), scale8(cur.g, seep), scale8(cur.b, seep)};
  buf[0] = Rgb{scale8(cur.r, keep), scale8(cur.g, keep), scale8(cur.b, keep)};
  for (int i = 1; i < width; i++) {
    Rgb c = frame[0][i];
    Rgb part{scale8(c.r, seep), scale8(c.g, seep), scale8(c.b, seep)};
    Rgb kept{static_cast<uint8_t>(qadd8(scale8(c.r, keep), carry.r)),
             static_cast<uint8_t>(qadd8(scale8(c.g, keep), carry.g)),
             static_cast<uint8_t>(qadd8(scale8(c.b, keep), carry.b))};
    buf[i - 1] = Rgb{qadd8(buf[i - 1].r, part.r), qadd8(buf[i - 1].g, part.g),
                      qadd8(buf[i - 1].b, part.b)};
    buf[i] = kept;
    carry = part;
  }
  for (int i = 0; i < width; i++) fill_column(frame, i, buf[i]);
}

// wled00/FX.cpp:2378 mode_meteor() - merges real WLED's meteor/
// meteor_smooth (its own comment notes they were merged). SEGENV.data
// (a SEGLEN-sized trail buffer) becomes a fixed 32-byte array since this
// board's width is fixed at compile time.
struct MeteorState {
  uint8_t trail[GuDisplay::WIDTH];
};
static_assert(sizeof(MeteorState) <= State::kDataSize, "MeteorState too big");

void mode_meteor(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  auto &s = *reinterpret_cast<MeteorState *>(state.data);
  bool smooth = p.option3;
  int meteor_size = 1 + width / 20;
  int meteor_start;
  if (smooth) {
    meteor_start = static_cast<int>(((state.step >> 6) & 0xFF) * (width - 1) / 255);
  } else {
    uint32_t counter = now_ms * ((p.speed >> 2) + 8);
    meteor_start = static_cast<int>((counter * static_cast<uint32_t>(width)) >> 16);
  }
  int max_val = (p.palette_id == 5 || !p.option1) ? 240 : 255;

  for (int i = 0; i < width; i++) {
    if (random8() <= 255 - p.intensity) {
      Rgb col;
      if (smooth) {
        if (s.trail[i] > 0) {
          int change = s.trail[i] + 4 - random8(24);
          if (change < 0) change = 0;
          if (change > max_val) change = max_val;
          s.trail[i] = static_cast<uint8_t>(change);
        }
        col = p.option1 ? pal(p, map_index(i, width), s.trail[i]) : pal(p, s.trail[i], 255);
      } else {
        s.trail[i] = scale8(s.trail[i], static_cast<uint8_t>(128 + random8(127)));
        int index = s.trail[i];
        int bri = (p.palette_id == 35 || p.palette_id == 36) ? 255 : s.trail[i];
        if (!p.option1) {
          index = (i * max_val) / width;
          bri = s.trail[i];
        }
        col = pal(p, static_cast<uint8_t>(index), static_cast<uint8_t>(bri));
      }
      fill_column(frame, i, col);
    }
  }

  for (int j = 0; j < meteor_size; j++) {
    int index = (meteor_start + j) % width;
    if (smooth) {
      s.trail[index] = static_cast<uint8_t>(max_val);
      Rgb col = p.option1 ? pal(p, map_index(index, width), s.trail[index]) : pal(p, s.trail[index], 255);
      fill_column(frame, index, col);
    } else {
      int idx = max_val;
      s.trail[index] = static_cast<uint8_t>(max_val);
      if (!p.option1) idx = (index * max_val) / width;
      fill_column(frame, index, pal(p, static_cast<uint8_t>(idx), 255));
    }
  }

  state.step += p.speed + 1;
}
EFFECTS_REGISTER(Id::kMeteor, mode_meteor)

// wled00/FX.cpp:144 mode_copy_segment() - this firmware's whole 32x32
// matrix is a single segment, so `sourceid` (SEGMENT.custom3) never
// resolves to another active segment; WLED's own "invalid source"
// fallback branch (fadeToBlackBy(5)) is therefore the only branch that
// can ever run, so that's the entire port.
void mode_copy_segment(uint32_t, const Params &, State &, Frame frame) {
  fade_to_black_by(frame, 5);
}
EFFECTS_REGISTER(Id::kCopy, mode_copy_segment)

// wled00/FX.cpp:2446 mode_railway(). SEGENV.step += FRAMETIME (WLED's own
// measured last-frame time delta) has no equivalent here - render() only
// hands mode_ functions an absolute now_ms - so a stored phase-start
// timestamp replaces the per-frame accumulator and elapsed time since the
// last direction flip is computed directly; frame-rate independent besides.
struct RailwayState {
  uint32_t anchor_ms;
};
static_assert(sizeof(RailwayState) <= State::kDataSize, "RailwayState too big");

void mode_railway(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  auto &s = *reinterpret_cast<RailwayState *>(state.data);
  if (state.call == 0) s.anchor_ms = now_ms;

  uint32_t dur = (256u - p.speed) * 40u;
  uint32_t rampdur = (dur * p.intensity) >> 8;
  uint32_t elapsed = now_ms - s.anchor_ms;
  if (elapsed > dur) {
    s.anchor_ms = now_ms;
    state.aux0 = state.aux0 ? 0 : 1;
    elapsed = 0;
  }
  uint32_t pos = 255;
  if (rampdur != 0) {
    uint32_t p0 = (elapsed * 255u) / rampdur;
    if (p0 < 255) pos = p0;
  }
  if (state.aux0) pos = 255 - pos;

  for (int i = 0; i < width; i += 2) {
    fill_column(frame, i, pal(p, static_cast<uint8_t>(255 - pos)));
    if (i < width - 1) fill_column(frame, i + 1, pal(p, static_cast<uint8_t>(pos)));
  }
}
EFFECTS_REGISTER(Id::kRailway, mode_railway)

struct Ripple {
  uint8_t state;
  uint8_t color;
  uint16_t pos;
};
struct RippleState {
  static constexpr int kMaxRipples = 1 + (GuDisplay::WIDTH >> 2);
  Ripple ripples[kMaxRipples];
};
static_assert(sizeof(RippleState) <= State::kDataSize, "RippleState too big");

// wled00/FX.cpp:2492 ripple_base() + wled00/FX.cpp:2545 mode_ripple(). Only
// the 1D branch of ripple_base is ported - the 2D branch (SEGMENT.is2D(),
// drawn via Segment::drawCircle()/blur2D()) is a substantial separate port
// on its own that's out of scope for this single batch entry; the 1D
// per-pixel decay/propagation math below is otherwise unchanged. WLED's
// setPixelColor() silently no-ops an out-of-range index; ported here as
// an explicit bounds check since Frame is a fixed array.
void mode_ripple(uint32_t, const Params &p, State &state, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  constexpr uint32_t kIBN = 5100;
  auto &s = *reinterpret_cast<RippleState *>(state.data);

  if (p.custom1 || p.option2) {
    fade_out_toward(frame, 250, p.secondary);
  } else {
    for (int x = 0; x < width; x++) fill_column(frame, x, p.secondary);
  }

  for (int i = 0; i < RippleState::kMaxRipples; i++) {
    Ripple &r = s.ripples[i];
    if (r.state) {
      unsigned decay = (p.speed >> 4) + 1;
      Rgb col = pal(p, r.color);
      unsigned propagation = ((r.state / decay - 1) * (p.speed + 1));
      int prop_i = static_cast<int>(propagation >> 8);
      unsigned prop_f = propagation & 0xFF;
      unsigned amp = (r.state < 17) ? triwave8(static_cast<uint8_t>((r.state - 1) * 8))
                                     : static_cast<unsigned>((static_cast<int>(r.state) - 17) * (2 - 255) /
                                                                  (255 - 17) +
                                                              255);

      int left = r.pos - prop_i - 1;
      int right = r.pos + prop_i + 2;
      for (int v = 0; v < 4; v++) {
        uint8_t mag = scale8(cubicwave8(static_cast<uint8_t>((prop_f >> 2) + v * 64)),
                              static_cast<uint8_t>(amp));
        int lx = left + v;
        int rx = right - v;
        if (lx >= 0 && lx < width) fill_column(frame, lx, blend(frame[0][lx], col, mag));
        if (rx >= 0 && rx < width) fill_column(frame, rx, blend(frame[0][rx], col, mag));
      }
      unsigned newstate = r.state + decay;
      r.state = (newstate > 254) ? 0 : static_cast<uint8_t>(newstate);
    } else {
      if (random16(static_cast<uint16_t>(kIBN + 10000)) <= p.intensity) {
        r.state = 1;
        r.pos = random16(static_cast<uint16_t>(width));
        r.color = random8();
      }
    }
  }
  blur1d(frame, static_cast<uint8_t>(p.custom1 >> 1));
}
EFFECTS_REGISTER(Id::kRipple, mode_ripple)

// wled00/FX.cpp:2580 twinklefox_one_twinkle() + wled00/FX.cpp:2642
// twinklefox_base() (shared by mode_twinklefox()/mode_twinklecat() below).
// gamma8inv() (compensates for WLED's own gamma-corrected LED output) has
// no port here - this firmware's gamma table lives in the display driver,
// not in the effect - so the background-dimming divisors are applied
// directly via scale8 instead of through an inverse-gamma lookup: same
// target brightness tiers, slightly different roll-off curve.
Rgb twinklefox_one_twinkle(uint32_t clock30, uint8_t salt, bool cat, const Params &p, uint16_t speed_div) {
  unsigned ticks = clock30 / speed_div;
  unsigned fastcycle8 = static_cast<uint8_t>(ticks);
  uint16_t slowcycle16 = static_cast<uint16_t>((ticks >> 8) + salt);
  slowcycle16 = static_cast<uint16_t>(slowcycle16 + sin8(static_cast<uint8_t>(slowcycle16)));
  slowcycle16 = static_cast<uint16_t>(slowcycle16 * 2053) + 1384;
  uint8_t slowcycle8 = static_cast<uint8_t>((slowcycle16 & 0xFF) + (slowcycle16 >> 8));

  unsigned density = (p.intensity >> 5) + 1;
  unsigned bright = 0;
  if (((slowcycle8 & 0x0E) / 2) < density) {
    unsigned ph = fastcycle8;
    if (cat) {
      bright = 255 - ph;
      if (p.option2) bright = ph;
    } else if (ph < 86) {
      bright = ph * 3;
    } else {
      ph -= 86;
      bright = 255 - (ph + (ph / 2));
    }
  }
  if (bright == 0) return Rgb{0, 0, 0};

  uint8_t hue = static_cast<uint8_t>(slowcycle8 - salt);
  Rgb c = pal(p, hue, static_cast<uint8_t>(bright));
  if (!p.option1 && fastcycle8 >= 128) {
    unsigned cooling = (fastcycle8 - 128) >> 4;
    c.g = qsub8(c.g, static_cast<uint8_t>(cooling));
    c.b = qsub8(c.b, static_cast<uint8_t>(cooling * 2));
  }
  return c;
}

void twinklefox_base(uint32_t now_ms, const Params &p, State &state, Frame frame, bool cat) {
  constexpr int width = GuDisplay::WIDTH;
  uint16_t prng16 = 11337;

  state.aux0 = (p.speed > 100) ? static_cast<uint16_t>(3 + ((255 - p.speed) >> 3))
                                : static_cast<uint16_t>(22 + ((100 - p.speed) >> 1));

  Rgb bg = p.secondary;
  unsigned bglight = (bg.r + bg.g + bg.b) / 3;
  uint8_t divisor = bglight > 64 ? 16 : (bglight > 16 ? 64 : 86);
  bg = Rgb{scale8(bg.r, divisor), scale8(bg.g, divisor), scale8(bg.b, divisor)};
  bglight = (bg.r + bg.g + bg.b) / 3;

  for (int i = 0; i < width; i++) {
    prng16 = static_cast<uint16_t>(prng16 * 2053) + 1384;
    unsigned clock_offset = prng16;
    prng16 = static_cast<uint16_t>(prng16 * 2053) + 1384;
    unsigned speed_mult_q5_3 = ((((prng16 & 0xFF) >> 4) + (prng16 & 0x0F)) & 0x0F) + 0x08;
    uint32_t clock30 = static_cast<uint32_t>((now_ms * speed_mult_q5_3) >> 3) + clock_offset;
    unsigned salt = prng16 >> 8;

    Rgb c = twinklefox_one_twinkle(clock30, static_cast<uint8_t>(salt), cat, p, state.aux0);
    unsigned cbright = (c.r + c.g + c.b) / 3;
    int deltabright = static_cast<int>(cbright) - static_cast<int>(bglight);

    Rgb out;
    if (deltabright >= 32 || (bg.r == 0 && bg.g == 0 && bg.b == 0)) {
      out = c;
    } else if (deltabright > 0) {
      out = blend(bg, c, static_cast<uint8_t>(deltabright * 8));
    } else {
      out = bg;
    }
    fill_column(frame, i, out);
  }
}

// wled00/FX.cpp:2700
void mode_twinklefox(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  twinklefox_base(now_ms, p, state, frame, false);
}
EFFECTS_REGISTER(Id::kTwinklefox, mode_twinklefox)

// wled00/FX.cpp:2707
void mode_twinklecat(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  twinklefox_base(now_ms, p, state, frame, true);
}
EFFECTS_REGISTER(Id::kTwinklecat, mode_twinklecat)

struct HalloweenEyeData {
  uint8_t state;
  uint8_t color;
  uint16_t start_pos;
  uint16_t duration;
  uint32_t start_time;
  uint32_t blink_end_time;
  uint8_t row_y;
};
static_assert(sizeof(HalloweenEyeData) <= State::kDataSize, "HalloweenEyeData too big");

// wled00/FX.cpp:2714 mode_halloween_eyes(). Genuinely 2D on this board
// (strip.isMatrix is true for a 32x32 segment in real WLED too) - the eye
// pair is drawn on one row, addressed via frame[y][x] directly.
// SEGMENT.offset (WLED's own "reuse a field matrix modes don't need for
// anything else" hack for picking that row) becomes an explicit row_y
// field here instead.
void mode_halloween_eyes(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  constexpr int height = GuDisplay::HEIGHT;
  constexpr unsigned kEyeSpace = 2;  // MAX(2, WIDTH>>4) for WIDTH=32
  constexpr unsigned kEyeWidth = kEyeSpace / 2;
  constexpr unsigned kEyeLength = 2 * kEyeWidth + kEyeSpace;

  auto &d = *reinterpret_cast<HalloweenEyeData *>(state.data);
  if (!p.option2) {
    for (int x = 0; x < width; x++) fill_column(frame, x, p.secondary);
  }

  enum : uint8_t { kInitOn = 0, kOn, kBlink, kInitOff, kOff, kCount };
  d.state = static_cast<uint8_t>(d.state % kCount);
  uint32_t duration = d.duration ? d.duration : 1;
  uint32_t elapsed = now_ms - d.start_time;

  switch (d.state) {
    case kInitOn: {
      d.start_pos = random16(0, static_cast<uint16_t>(width - kEyeLength - 1));
      d.color = random8();
      d.row_y = static_cast<uint8_t>(random16(static_cast<uint16_t>(height - 1)));
      duration = 128u + random16(static_cast<uint16_t>(p.intensity * 64u));
      d.duration = static_cast<uint16_t>(duration);
      d.state = kOn;
      [[fallthrough]];
    }
    case kOn: {
      unsigned start2 = d.start_pos + kEyeWidth + kEyeSpace;
      uint32_t cap = 128u + p.intensity * 64u;
      duration = duration < cap ? duration : cap;

      constexpr uint32_t kMinOnBegin = 1024u;
      constexpr uint32_t kMinOnEnd = 1024u;
      uint32_t fade_in = elapsed * 256u * 8u / duration;
      Rgb eye_color = pal(p, d.color);
      Rgb c = eye_color;
      if (fade_in < 256u) {
        c = blend(p.secondary, eye_color, static_cast<uint8_t>(fade_in));
      } else if (elapsed > kMinOnBegin) {
        uint32_t remaining = (elapsed >= duration) ? 0u : (duration - elapsed);
        if (remaining > kMinOnEnd && random8() < 4u) {
          c = p.secondary;
          d.state = kBlink;
          d.blink_end_time = now_ms + random8(8, 128);
        }
      }

      bool same_as_bg = c.r == p.secondary.r && c.g == p.secondary.g && c.b == p.secondary.b;
      if (!same_as_bg) {
        for (unsigned i = 0; i < kEyeWidth; i++) {
          frame[d.row_y][d.start_pos + i] = c;
          frame[d.row_y][start2 + i] = c;
        }
      }
      break;
    }
    case kBlink:
      if (now_ms >= d.blink_end_time) d.state = kOn;
      break;
    case kInitOff: {
      uint32_t off_base = p.speed * 128u;
      duration = off_base + random16(static_cast<uint16_t>(off_base));
      d.duration = static_cast<uint16_t>(duration);
      d.state = kOff;
      [[fallthrough]];
    }
    case kOff: {
      uint32_t off_base = p.speed * 128u;
      uint32_t cap = 2u * off_base;
      duration = duration < cap ? duration : cap;
      break;
    }
    default:
      d.state = kInitOn;
      break;
  }

  if (elapsed > duration) {
    d.state = (d.state == kInitOn || d.state == kOn || d.state == kBlink) ? kInitOff : kInitOn;
    d.start_time = now_ms;
  }
}
EFFECTS_REGISTER(Id::kHalloweenEyes, mode_halloween_eyes)

// wled00/FX.cpp:2870 mode_static_pattern()
void mode_static_pattern(uint32_t, const Params &p, State &, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  int lit = 1 + p.speed;
  int unlit = 1 + p.intensity;
  bool drawing_lit = true;
  int cnt = 0;
  for (int i = 0; i < width; i++) {
    fill_column(frame, i, drawing_lit ? pal(p, map_index(i, width)) : p.secondary);
    cnt++;
    if (cnt >= (drawing_lit ? lit : unlit)) {
      cnt = 0;
      drawing_lit = !drawing_lit;
    }
  }
}
EFFECTS_REGISTER(Id::kStaticPattern, mode_static_pattern)

// wled00/FX.cpp:2889 mode_tri_static_pattern()
void mode_tri_static_pattern(uint32_t, const Params &p, State &, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  int seg_size = (p.intensity >> 5) + 1;
  int cur_seg = 0;
  int cur_count = 0;
  for (int i = 0; i < width; i++) {
    Rgb c = (cur_seg % 3 == 0) ? p.primary : (cur_seg % 3 == 1) ? p.secondary : p.tertiary;
    fill_column(frame, i, c);
    cur_count++;
    if (cur_count >= seg_size) {
      cur_seg++;
      cur_count = 0;
    }
  }
}
EFFECTS_REGISTER(Id::kTriStaticPattern, mode_tri_static_pattern)

// wled00/FX.cpp:2913 spots_base(), shared by mode_spots()/mode_spots_fade().
void spots_base(const Params &p, Frame frame, uint16_t threshold) {
  constexpr int width = GuDisplay::WIDTH;
  if (!p.option2) {
    for (int x = 0; x < width; x++) fill_column(frame, x, p.secondary);
  }
  int max_zones = width >> 2;
  int zones = 1 + ((p.intensity * max_zones) >> 8);
  int zone_len = width / zones;
  int offset = (width - zones * zone_len) >> 1;

  for (int z = 0; z < zones; z++) {
    int pos = offset + z * zone_len;
    for (int i = 0; i < zone_len; i++) {
      unsigned wave = triwave16(static_cast<uint16_t>((static_cast<uint32_t>(i) * 0xFFFFu) / zone_len));
      if (wave > threshold) {
        int index = pos + i;
        unsigned sc = (wave - threshold) * 255u / (0xFFFFu - threshold);
        if (index >= 0 && index < width) {
          fill_column(frame, index,
                      blend(pal(p, map_index(index, width)), p.secondary, static_cast<uint8_t>(255 - sc)));
        }
      }
    }
  }
}

// wled00/FX.cpp:2940 mode_spots()
void mode_spots(uint32_t, const Params &p, State &, Frame frame) {
  spots_base(p, frame, static_cast<uint16_t>((255 - p.speed) << 8));
}
EFFECTS_REGISTER(Id::kSpots, mode_spots)

// wled00/FX.cpp:2948 mode_spots_fade()
void mode_spots_fade(uint32_t now_ms, const Params &p, State &, Frame frame) {
  uint32_t counter = now_ms * ((p.speed >> 2) + 8);
  uint32_t t = triwave16(static_cast<uint16_t>(counter));
  uint32_t tr = (t >> 1) + (t >> 2);
  spots_base(p, frame, static_cast<uint16_t>(tr));
}
EFFECTS_REGISTER(Id::kSpotsFade, mode_spots_fade)

// wled00/FX.cpp:3387 glitter_base() + wled00/FX.cpp:3392 mode_glitter().
// paletteBlend (WLED's per-segment blend/wrap setting) isn't modeled in
// this firmware's Params, so the "cut off palette wrap at the end" edge
// case that depends on it is dropped - the scrolling background always
// wraps continuously here.
void mode_glitter(uint32_t now_ms, const Params &p, State &, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  if (!p.option2) {
    uint32_t counter = 0;
    if (p.speed != 0) {
      counter = (now_ms * ((p.speed >> 3) + 1)) & 0xFFFFu;
      counter >>= 8;
    }
    for (int i = 0; i < width; i++) {
      uint8_t color_index = static_cast<uint8_t>((i * 255 / width) - counter);
      fill_column(frame, i, pal(p, color_index));
    }
  }
  bool has_tertiary = p.tertiary.r || p.tertiary.g || p.tertiary.b;
  Rgb glitter_color = has_tertiary ? p.tertiary : Rgb{255, 255, 255};
  if (p.intensity > random8()) {
    int x = random16(static_cast<uint16_t>(width));
    fill_column(frame, x, glitter_color);
  }
}
EFFECTS_REGISTER(Id::kGlitter, mode_glitter)

struct CandleState {
  uint32_t last_call_ms;
};
static_assert(sizeof(CandleState) <= State::kDataSize, "CandleState too big");

// wled00/FX.cpp:3498 candle(multi=false) as used by wled00/FX.cpp:3579
// mode_candle() - the multi-candle branch (mode_candle_multi(), a
// different effect ID not in this batch) is not ported. FRAMETIME_FIXED
// (1000/WLED_FPS) becomes a plain constant here - it only rate-limits how
// often the flicker target updates, so the exact FPS assumed doesn't matter.
void mode_candle(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  constexpr uint32_t kFrameTimeFixedMs = 16;
  auto &s = *reinterpret_cast<CandleState *>(state.data);
  if (now_ms - s.last_call_ms < kFrameTimeFixedMs) return;
  s.last_call_ms = now_ms;

  unsigned valrange = p.intensity;
  unsigned rndval = valrange >> 1;
  unsigned speed_factor = 4;
  if (p.speed > 252) speed_factor = 1;
  else if (p.speed > 99) speed_factor = 2;
  else if (p.speed > 49) speed_factor = 3;

  unsigned sv = state.aux0, target = state.aux1, fade_step = state.step;
  if (fade_step == 0) {
    sv = 128;
    target = 130 + random8(4);
    fade_step = 1;
  }

  bool new_target = false;
  if (target > sv) {
    sv = qadd8(static_cast<uint8_t>(sv), static_cast<uint8_t>(fade_step));
    if (sv >= target) new_target = true;
  } else {
    sv = qsub8(static_cast<uint8_t>(sv), static_cast<uint8_t>(fade_step));
    if (sv <= target) new_target = true;
  }

  if (new_target) {
    target = random8(static_cast<uint8_t>(rndval)) + random8(static_cast<uint8_t>(rndval));
    if (target < (rndval >> 1)) target = (rndval >> 1) + random8(static_cast<uint8_t>(rndval));
    target += (255 - valrange);
    unsigned dif = (target > sv) ? target - sv : sv - target;
    fade_step = dif >> speed_factor;
    if (fade_step == 0) fade_step = 1;
  }

  for (int x = 0; x < width; x++) {
    fill_column(frame, x, blend(p.secondary, pal(p, map_index(x, width)), static_cast<uint8_t>(sv)));
  }

  state.aux0 = static_cast<uint16_t>(sv);
  state.aux1 = static_cast<uint16_t>(target);
  state.step = fade_step;
}
EFFECTS_REGISTER(Id::kCandle, mode_candle)

constexpr int kStarburstMaxFrag = 10;
struct Star {
  Rgb color;
  uint32_t birth;
  uint32_t last;
  float vel;
  uint16_t pos;
  float fragment[kStarburstMaxFrag];
};
struct StarburstState {
  static constexpr int kNumStars = 1 + (GuDisplay::WIDTH >> 3);
  Star stars[kNumStars];
};
static_assert(sizeof(StarburstState) <= State::kDataSize, "StarburstState too big");

// wled00/FX.cpp:3613 mode_starburst(). This is real WLED's original
// (pre-ParticleSystem) implementation, still compiled today under
// WLED_PS_DONT_REPLACE_1D_FX; it doesn't touch the ParticleSystem class at
// all (plain float structs), so it's in scope for this batch. The
// float->unsigned cast WLED relies on for `start`/`end` (a negative float
// assigned to an `unsigned`, technically UB) is replaced with an explicit
// int clamp to 0 - the "star straddles the left edge" case those lines
// exist for still renders identically. The ESP8266/ESP32 star-count memory
// budget calc is dropped in favor of a small fixed star count sized for
// this board.
void mode_starburst(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  constexpr float kMaxSpeed = 375.0f;
  constexpr float kIgnition = 250.0f;
  constexpr float kFadeTime = 1500.0f;
  auto &s = *reinterpret_cast<StarburstState *>(state.data);
  uint32_t it = now_ms;

  for (int j = 0; j < StarburstState::kNumStars; j++) {
    Star &st = s.stars[j];
    if (random8(static_cast<uint8_t>(144 - (p.speed >> 1))) == 0 && st.birth == 0) {
      unsigned start_pos = random16(static_cast<uint16_t>(width - 1));
      float multiplier = static_cast<float>(random8()) / 255.0f;
      st.color = color_wheel(random8());
      st.pos = static_cast<uint16_t>(start_pos);
      st.vel = kMaxSpeed * (static_cast<float>(random8()) / 255.0f) * multiplier;
      st.birth = it;
      st.last = it;
      int num = random8(3, static_cast<uint8_t>(6 + (p.intensity >> 5)));
      for (int i = 0; i < kStarburstMaxFrag; i++) {
        st.fragment[i] = (i < num) ? static_cast<float>(start_pos) : -1.0f;
      }
    }
  }

  if (!p.option2) {
    for (int x = 0; x < width; x++) fill_column(frame, x, p.secondary);
  }

  for (int j = 0; j < StarburstState::kNumStars; j++) {
    Star &st = s.stars[j];
    if (st.birth != 0) {
      float dt = static_cast<float>(it - st.last) / 1000.0f;
      for (int i = 0; i < kStarburstMaxFrag; i++) {
        float var = static_cast<float>(i >> 1);
        if (st.fragment[i] > 0) st.fragment[i] += st.vel * dt * var / 3.0f;
      }
      st.last = it;
      st.vel -= 3.0f * st.vel * dt;
    }

    Rgb c = st.color;
    float fade = 0.0f;
    float age = static_cast<float>(it - st.birth);

    if (age < kIgnition) {
      c = blend(Rgb{255, 255, 255}, c, static_cast<uint8_t>(254.5f * (age / kIgnition)));
    } else if (age > kIgnition + kFadeTime) {
      st.birth = 0;
      c = p.secondary;
    } else {
      age -= kIgnition;
      fade = age / kFadeTime;
      c = blend(c, p.secondary, static_cast<uint8_t>(254.5f * fade));
    }

    float particle_size = (1.0f - fade) * 2.0f;

    for (int index2 = 0; index2 < kStarburstMaxFrag * 2; index2++) {
      bool mirrored = index2 & 1;
      int i = index2 >> 1;
      if (st.fragment[i] > 0) {
        float loc = st.fragment[i];
        if (mirrored) loc -= (loc - st.pos) * 2.0f;
        int start = static_cast<int>(loc - particle_size);
        int end = static_cast<int>(loc + particle_size);
        if (start < 0) start = 0;
        if (start == end) end++;
        if (end > width) end = width;
        for (int px = start; px < end; px++) fill_column(frame, px, c);
      }
    }
  }
}
EFFECTS_REGISTER(Id::kStarburst, mode_starburst)

struct Ball {
  uint32_t last_bounce_ms;
  float impact_velocity;
  float height;
};
struct BouncingBallsState {
  static constexpr int kMaxBalls = 16;
  Ball balls[kMaxBalls];
};
static_assert(sizeof(BouncingBallsState) <= State::kDataSize, "BouncingBallsState too big");

// wled00/FX.cpp:2967 mode_bouncing_balls(). WLED's virtual-strip machinery
// (one independent ball simulation per matrix column, nrOfVStrips()) is
// dropped - this board's whole matrix is one segment/one "strip" here,
// same as every other de facto 1D effect in this batch, so only the
// single-strip ball simulation is ported.
void mode_bouncing_balls(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  constexpr float kGravity = -9.81f;
  auto &s = *reinterpret_cast<BouncingBallsState *>(state.data);
  int num_balls = (p.intensity * (BouncingBallsState::kMaxBalls - 1)) / 255 + 1;
  bool has_tertiary = p.tertiary.r || p.tertiary.g || p.tertiary.b;

  if (!p.option2) {
    for (int x = 0; x < width; x++) fill_column(frame, x, has_tertiary ? Rgb{0, 0, 0} : p.secondary);
  }

  if (state.call == 0) {
    for (auto &b : s.balls) b.last_bounce_ms = now_ms;
  }

  for (int i = 0; i < num_balls; i++) {
    Ball &b = s.balls[i];
    int divisor = (255 - p.speed) / 64 + 1;
    float time_since_bounce = static_cast<float>(now_ms - b.last_bounce_ms) / static_cast<float>(divisor);
    float time_sec = time_since_bounce / 1000.0f;
    b.height = (0.5f * kGravity * time_sec + b.impact_velocity) * time_sec;

    if (b.height <= 0.0f) {
      b.height = 0.0f;
      float dampening = 0.9f - static_cast<float>(i) / static_cast<float>(num_balls * num_balls);
      b.impact_velocity = dampening * b.impact_velocity;
      b.last_bounce_ms = now_ms;
      if (b.impact_velocity < 0.015f) {
        b.impact_velocity = sqrtf(-2.0f * kGravity) * static_cast<float>(random8(5, 11)) / 10.0f;
      }
    } else if (b.height > 1.0f) {
      continue;
    }

    Rgb color = p.primary;
    if (p.palette_id) {
      int wheel_step = num_balls > 8 ? num_balls : 8;
      color = color_wheel(static_cast<uint8_t>(i * (256 / wheel_step)));
    } else if (has_tertiary) {
      color = (i % 3 == 0) ? p.primary : (i % 3 == 1) ? p.secondary : p.tertiary;
    }

    int pos = static_cast<int>(roundf(b.height * (width - 1)));
    if (pos >= 0 && pos < width) fill_column(frame, pos, color);
  }
}
EFFECTS_REGISTER(Id::kBouncingballs, mode_bouncing_balls)

// wled00/FX.cpp:3336 sinelon_base(), shared by the three mode_sinelon*()
// variants below (wled00/FX.cpp:3369-3384).
void sinelon_base(uint32_t now_ms, const Params &p, State &state, Frame frame, bool dual, bool rainbow) {
  constexpr int width = GuDisplay::WIDTH;
  fade_out_toward(frame, p.intensity, p.secondary);

  int pos = beatsin16(now_ms, static_cast<uint8_t>(p.speed / 10), 0, width - 1);
  if (state.call == 0) state.aux0 = static_cast<uint16_t>(pos);

  Rgb color1 = pal(p, map_index(pos, width));
  Rgb color2 = p.tertiary;
  if (rainbow) color1 = color_wheel(static_cast<uint8_t>((pos & 0x07) * 32));

  fill_column(frame, pos, color1);
  if (dual) {
    bool has_tertiary = p.tertiary.r || p.tertiary.g || p.tertiary.b;
    if (!has_tertiary) color2 = pal(p, map_index(pos, width));
    if (rainbow) color2 = color1;
    fill_column(frame, width - 1 - pos, color2);
  }

  if (state.aux0 != static_cast<uint16_t>(pos)) {
    int prev = state.aux0;
    if (prev < pos) {
      for (int i = prev; i < pos; i++) {
        fill_column(frame, i, color1);
        if (dual) fill_column(frame, width - 1 - i, color2);
      }
    } else {
      for (int i = prev; i > pos; i--) {
        fill_column(frame, i, color1);
        if (dual) fill_column(frame, width - 1 - i, color2);
      }
    }
    state.aux0 = static_cast<uint16_t>(pos);
  }
}

// wled00/FX.cpp:3369
void mode_sinelon(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  sinelon_base(now_ms, p, state, frame, false, false);
}
EFFECTS_REGISTER(Id::kSinelon, mode_sinelon)

// wled00/FX.cpp:3375
void mode_sinelon_dual(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  sinelon_base(now_ms, p, state, frame, true, false);
}
EFFECTS_REGISTER(Id::kSinelonDual, mode_sinelon_dual)

// wled00/FX.cpp:3381
void mode_sinelon_rainbow(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  sinelon_base(now_ms, p, state, frame, false, true);
}
EFFECTS_REGISTER(Id::kSinelonRainbow, mode_sinelon_rainbow)

}  // namespace
}  // namespace effects
