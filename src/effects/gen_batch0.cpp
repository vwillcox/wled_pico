#include "effects.h"
#include "wled_compat.h"
#include "palettes.h"
#include "display/gu_display.h"

#include <math.h>

namespace effects {
namespace {

// Shared utilities used by several effects below (each ported file defines
// its own copy of file-local helpers like this rather than sharing across
// translation units - see effects.cpp's identically-named color_wheel()).

uint8_t clamp_u8(int v) {
  if (v < 0) return 0;
  if (v > 255) return 255;
  return static_cast<uint8_t>(v);
}

// wled00/FX_fcn.cpp:1141 Segment::color_wheel() - the raw HSV wheel branch
// only (its `if (palette) return color_from_palette(...)` branch is dropped,
// same simplification effects.cpp's own color_wheel() already documents:
// "a plain full-saturation hue wheel, no palette involved").
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

// wled00/util.cpp:748 get_random_wheel_index()
uint8_t random_wheel_index(uint8_t pos) {
  uint8_t r = 0, x = 0, y = 0, d = 0;
  while (d < 42) {
    r = random8();
    x = pos > r ? static_cast<uint8_t>(pos - r) : static_cast<uint8_t>(r - pos);
    y = static_cast<uint8_t>(255 - x);
    d = x < y ? x : y;
  }
  return r;
}

// FastLED lib8tion/trig8.h triwave16(): triangle wave, period 65536.
uint16_t triwave16(uint16_t in) {
  if (in & 0x8000) in = static_cast<uint16_t>(65535 - in);
  return static_cast<uint16_t>(in << 1);
}

bool rgb_eq(const Rgb &a, const Rgb &b) { return a.r == b.r && a.g == b.g && a.b == b.b; }

// wled00/FX.h:61-62 FRAMETIME_FIXED (1000/WLED_FPS, WLED_FPS==42) - this
// firmware has no equivalent of strip.getFrameTime()'s measured frame time,
// so mode_blink() (the only effect in this batch that needs it) uses the
// fixed default instead.
constexpr uint32_t kFrameTimeMs = 1000 / 42;

// wled00/FX.cpp:195 blink(color1=primary, color2=secondary, strobe=false,
// do_palette=true), as used by mode_blink().
void mode_blink(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  uint32_t cycle_time = (255u - p.speed) * 20u;
  uint32_t on_time = kFrameTimeMs + ((cycle_time * p.intensity) >> 8);
  cycle_time += kFrameTimeMs * 2;
  uint32_t it = now_ms / cycle_time;
  uint32_t rem = now_ms % cycle_time;
  bool on = (it != state.step) || (rem <= on_time);
  state.step = it;

  if (on) {
    for (int i = 0; i < width; i++) {
      uint8_t pal_idx = static_cast<uint8_t>((i * 255) / width);
      fill_column(frame, i, color_from_palette(p.palette_id, pal_idx, p.primary, p.secondary, p.tertiary));
    }
  } else {
    for (int i = 0; i < width; i++) fill_column(frame, i, p.secondary);
  }
}
EFFECTS_REGISTER(Id::kBlink, mode_blink)

// wled00/FX.cpp:262 color_wipe(rev=false, useRandomColors=true), as used by
// mode_color_wipe_random(). SEGLEN<=1 fallback dropped (this board's width
// is always 32).
void mode_color_wipe_random(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  uint32_t cycle_time = 750 + (255 - p.speed) * 150;
  uint32_t perc = now_ms % cycle_time;
  uint32_t prog = (perc * 65535) / cycle_time;
  bool back = prog > 32767;
  if (back) {
    prog -= 32767;
    if (state.step == 0) state.step = 1;
  } else {
    if (state.step == 2) state.step = 3;
  }

  if (state.call == 0) {
    state.aux0 = random8();
    state.step = 3;
  }
  if (state.step == 1) {
    state.aux1 = random_wheel_index(static_cast<uint8_t>(state.aux0));
    state.step = 2;
  }
  if (state.step == 3) {
    state.aux0 = random_wheel_index(static_cast<uint8_t>(state.aux1));
    state.step = 0;
  }

  int led_index = static_cast<int>((prog * static_cast<uint32_t>(width)) >> 15);
  uint32_t rem = (prog * static_cast<uint32_t>(width) * 2) / (p.intensity + 1);
  if (rem > 255) rem = 255;

  Rgb col1 = color_wheel(static_cast<uint8_t>(state.aux1));
  Rgb col0 = color_wheel(static_cast<uint8_t>(state.aux0));
  for (int i = 0; i < width; i++) {
    Rgb px;
    if (i < led_index) {
      px = back ? col1 : col0;
    } else {
      px = back ? col0 : col1;
      if (i == led_index) px = blend(back ? col0 : col1, back ? col1 : col0, static_cast<uint8_t>(rem));
    }
    fill_column(frame, i, px);
  }
}
EFFECTS_REGISTER(Id::kColorWipeRandom, mode_color_wipe_random)

// wled00/FX.cpp:354 mode_random_color()
void mode_random_color(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  uint32_t cycle_time = 200 + (255 - p.speed) * 50;
  uint32_t it = now_ms / cycle_time;
  uint32_t rem = now_ms % cycle_time;
  uint32_t fadedur = (cycle_time * p.intensity) >> 8;

  uint32_t fade = 255;
  if (fadedur) {
    fade = (rem * 255) / fadedur;
    if (fade > 255) fade = 255;
  }

  if (state.call == 0) {
    state.aux0 = random8();
    state.step = 2;
  }
  if (it != state.step) {
    state.aux1 = state.aux0;
    state.aux0 = random_wheel_index(static_cast<uint8_t>(state.aux0));
    state.step = it;
  }

  Rgb color = blend(color_wheel(static_cast<uint8_t>(state.aux1)), color_wheel(static_cast<uint8_t>(state.aux0)),
                     static_cast<uint8_t>(fade));
  for (int x = 0; x < GuDisplay::WIDTH; x++) fill_column(frame, x, color);
}
EFFECTS_REGISTER(Id::kRandomColor, mode_random_color)

// wled00/FX.cpp:262 color_wipe(rev=true, useRandomColors=false), as used by
// mode_color_sweep(). SEGLEN<=1 fallback dropped.
void mode_color_sweep(uint32_t now_ms, const Params &p, State &, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  uint32_t cycle_time = 750 + (255 - p.speed) * 150;
  uint32_t perc = now_ms % cycle_time;
  uint32_t prog = (perc * 65535) / cycle_time;
  bool back = prog > 32767;
  if (back) prog -= 32767;

  int led_index = static_cast<int>((prog * static_cast<uint32_t>(width)) >> 15);
  uint32_t rem = (prog * static_cast<uint32_t>(width) * 2) / (p.intensity + 1);
  if (rem > 255) rem = 255;

  for (int i = 0; i < width; i++) {
    int index = back ? (width - 1 - i) : i;
    uint8_t pal_idx = static_cast<uint8_t>((index * 255) / width);
    Rgb col0 = color_from_palette(p.palette_id, pal_idx, p.primary, p.secondary, p.tertiary);
    Rgb px;
    if (i < led_index) {
      px = back ? p.secondary : col0;
    } else {
      px = back ? col0 : p.secondary;
      if (i == led_index) px = blend(back ? col0 : p.secondary, back ? p.secondary : col0, static_cast<uint8_t>(rem));
    }
    fill_column(frame, index, px);
  }
}
EFFECTS_REGISTER(Id::kColorSweep, mode_color_sweep)

struct DynamicState {
  uint8_t idx[GuDisplay::WIDTH];
};
static_assert(sizeof(DynamicState) <= State::kDataSize, "DynamicState must fit State::data");

// wled00/FX.cpp:386 mode_dynamic(). SEGENV.allocateData() failure fallback
// dropped (state.data is a fixed-size buffer, never fails to "allocate").
// state.data reinterpreted as DynamicState (32 bytes: one color-wheel index
// per column).
void mode_dynamic(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  auto &s = *reinterpret_cast<DynamicState *>(state.data);
  if (state.call == 0) {
    for (int i = 0; i < width; i++) s.idx[i] = random8();
  }

  uint32_t cycle_time = 50 + (255u - p.speed) * 15u;
  uint32_t it = now_ms / cycle_time;
  if (it != state.step && p.speed != 0) {
    for (int i = 0; i < width; i++) {
      if (random8() <= p.intensity) s.idx[i] = random8();
    }
    state.step = it;
  }

  for (int i = 0; i < width; i++) {
    Rgb c = color_wheel(s.idx[i]);
    if (p.option1) c = blend(frame[0][i], c, 16);
    fill_column(frame, i, c);
  }
}
EFFECTS_REGISTER(Id::kDynamic, mode_dynamic)

// wled00/FX.cpp:514 mode_rainbow()
void mode_rainbow(uint32_t now_ms, const Params &p, State &, Frame frame) {
  uint32_t counter = (now_ms * ((p.speed >> 2) + 2)) & 0xFFFFu;
  counter >>= 8;
  Rgb color = color_wheel(static_cast<uint8_t>(counter));
  if (p.intensity < 128) color = blend(color, Rgb{255, 255, 255}, static_cast<uint8_t>(128 - p.intensity));
  for (int x = 0; x < GuDisplay::WIDTH; x++) fill_column(frame, x, color);
}
EFFECTS_REGISTER(Id::kRainbow, mode_rainbow)

// wled00/FX.cpp:467 scan(dual) - shared helper for mode_scan()/
// mode_dual_scan(). SEGLEN<=1 fallback dropped. The mcol argument to the
// original's SEGMENT.color_from_palette() calls (SEGCOLOR(2)?2:0) only
// changes anything when palette_id==0, which this engine's
// color_from_palette() already aliases to PartyColors rather than a flat
// mcol color (see palettes.h) - dropped for consistency with that existing
// simplification.
void scan(uint32_t now_ms, const Params &p, Frame frame, bool dual) {
  constexpr int width = GuDisplay::WIDTH;
  uint32_t cycle_time = 750 + (255 - p.speed) * 150;
  uint32_t perc = now_ms % cycle_time;
  int prog = static_cast<int>((perc * 65535) / cycle_time);
  int size = 1 + ((p.intensity * width) >> 9);
  int led_index = (prog * ((width * 2) - size * 2)) >> 16;

  if (!p.option2) {
    for (int x = 0; x < width; x++) fill_column(frame, x, p.secondary);
  }

  int led_offset = led_index - (width - size);
  led_offset = led_offset < 0 ? -led_offset : led_offset;

  if (dual) {
    for (int j = led_offset; j < led_offset + size; j++) {
      int i2 = width - 1 - j;
      uint8_t pal_idx = static_cast<uint8_t>((i2 * 255) / width);
      fill_column(frame, i2, color_from_palette(p.palette_id, pal_idx, p.primary, p.secondary, p.tertiary));
    }
  }
  for (int j = led_offset; j < led_offset + size; j++) {
    uint8_t pal_idx = static_cast<uint8_t>((j * 255) / width);
    fill_column(frame, j, color_from_palette(p.palette_id, pal_idx, p.primary, p.secondary, p.tertiary));
  }
}

// wled00/FX.cpp:496 mode_scan()
void mode_scan(uint32_t now_ms, const Params &p, State &, Frame frame) { scan(now_ms, p, frame, false); }
EFFECTS_REGISTER(Id::kScan, mode_scan)

// wled00/FX.cpp:505 mode_dual_scan()
void mode_dual_scan(uint32_t now_ms, const Params &p, State &, Frame frame) { scan(now_ms, p, frame, true); }
EFFECTS_REGISTER(Id::kDualScan, mode_dual_scan)

// wled00/FX.cpp:453 mode_fade()
void mode_fade(uint32_t now_ms, const Params &p, State &, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  uint32_t counter = now_ms * ((p.speed >> 3) + 10);
  uint8_t lum = static_cast<uint8_t>(triwave16(static_cast<uint16_t>(counter)) >> 8);
  for (int i = 0; i < width; i++) {
    uint8_t pal_idx = static_cast<uint8_t>((i * 255) / width);
    Rgb c = color_from_palette(p.palette_id, pal_idx, p.primary, p.secondary, p.tertiary);
    fill_column(frame, i, blend(p.secondary, c, lum));
  }
}
EFFECTS_REGISTER(Id::kFade, mode_fade)

// wled00/FX.cpp:546 running(color1, color2, theatre) - shared helper for
// mode_theater_chase()/mode_theater_chase_rainbow(). Only the theatre=true
// branch is ported (both of this batch's callers use it; the non-theatre
// "Running Dual"-style branch belongs to mode_running_dual(), not in this
// batch). `use_palette` replaces the original's `color1 == SEGCOLOR(0)`
// pointer-equality check (this codebase's Rgb isn't compared that way) with
// an explicit flag - true for mode_theater_chase() (color1 is always
// primary), false for mode_theater_chase_rainbow() (color1 is a moving
// wheel color that generally isn't primary).
void running(uint32_t now_ms, const Params &p, State &state, Frame frame, Rgb color1, bool use_palette) {
  constexpr int width_px = GuDisplay::WIDTH;
  int width = 3 + (p.intensity >> 4);
  uint32_t cycle_time = 50 + (255 - p.speed);
  uint32_t it = now_ms / cycle_time;

  for (int i = 0; i < width_px; i++) {
    Rgb col = p.secondary;
    Rgb c1 = color1;
    if (use_palette) {
      uint8_t pal_idx = static_cast<uint8_t>((i * 255) / width_px);
      c1 = color_from_palette(p.palette_id, pal_idx, p.primary, p.secondary, p.tertiary);
    }
    if ((i % width) == state.aux0) col = c1;
    fill_column(frame, i, col);
  }

  if (it != state.step) {
    state.aux0 = static_cast<uint16_t>((state.aux0 + 1) % width);
    state.step = it;
  }
}

// wled00/FX.cpp:575 mode_theater_chase()
void mode_theater_chase(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  running(now_ms, p, state, frame, p.primary, true);
}
EFFECTS_REGISTER(Id::kTheaterChase, mode_theater_chase)

// wled00/FX.cpp:585 mode_theater_chase_rainbow()
void mode_theater_chase_rainbow(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  running(now_ms, p, state, frame, color_wheel(static_cast<uint8_t>(state.step)), false);
}
EFFECTS_REGISTER(Id::kTheaterChaseRainbow, mode_theater_chase_rainbow)

// wled00/FX.cpp:594 running_base(saw, dual=false) - shared helper for
// mode_running_lights()/mode_saw(). The dual=true branch (used by
// mode_running_dual(), not in this batch) is dropped; sin_gap(), which that
// branch alone needs, isn't ported either.
void running_base(uint32_t now_ms, const Params &p, Frame frame, bool saw) {
  constexpr int width = GuDisplay::WIDTH;
  int x_scale = p.intensity >> 2;
  uint32_t counter = (now_ms * p.speed) >> 9;

  for (int i = 0; i < width; i++) {
    uint32_t a = static_cast<uint32_t>(i * x_scale) - counter;
    if (saw) {
      uint8_t a8 = static_cast<uint8_t>(a);
      int av;
      if (a8 < 16) {
        av = 192 + a8 * 8;
      } else {
        av = 64 + ((a8 - 16) * 128) / 239;
      }
      a = static_cast<uint8_t>(255 - av);
    }
    uint8_t s = sin8(static_cast<uint8_t>(a));
    uint8_t pal_idx = static_cast<uint8_t>((i * 255) / width);
    Rgb ca = blend(p.secondary, color_from_palette(p.palette_id, pal_idx, p.primary, p.secondary, p.tertiary), s);
    fill_column(frame, i, ca);
  }
}

// wled00/FX.cpp:636 mode_running_lights()
void mode_running_lights(uint32_t now_ms, const Params &p, State &, Frame frame) {
  running_base(now_ms, p, frame, false);
}
EFFECTS_REGISTER(Id::kRunningLights, mode_running_lights)

// wled00/FX.cpp:645 mode_saw()
void mode_saw(uint32_t now_ms, const Params &p, State &, Frame frame) { running_base(now_ms, p, frame, true); }
EFFECTS_REGISTER(Id::kSaw, mode_saw)

// wled00/FX.cpp:655 mode_twinkle(). SEGENV.aux1's reseed from hw_random() (a
// 32-bit value in real WLED) uses this codebase's random16() instead - aux1
// itself is only 16 bits wide here, so the extra range would be truncated
// away regardless.
void mode_twinkle(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  fade_to_black_by(frame, 224);

  uint32_t cycle_time = 20 + (255 - p.speed) * 5;
  uint32_t it = now_ms / cycle_time;
  if (it != state.step) {
    uint16_t max_on = static_cast<uint16_t>((p.intensity * 31) / 255 + 1);
    if (state.aux0 >= max_on) {
      state.aux0 = 0;
      state.aux1 = random16();
    }
    state.aux0++;
    state.step = it;
  }

  uint16_t prng16 = state.aux1;
  for (uint16_t i = 0; i < state.aux0; i++) {
    prng16 = static_cast<uint16_t>(prng16 * 2053u + 13849u);
    uint32_t pos = static_cast<uint32_t>(width) * static_cast<uint32_t>(prng16);
    int j = static_cast<int>(pos >> 16);
    uint8_t pal_idx = static_cast<uint8_t>((j * 255) / width);
    fill_column(frame, j, color_from_palette(p.palette_id, pal_idx, p.primary, p.secondary, p.tertiary));
  }
}
EFFECTS_REGISTER(Id::kTwinkle, mode_twinkle)

struct DissolveState {
  Rgb pixels[GuDisplay::WIDTH];
};
static_assert(sizeof(DissolveState) <= State::kDataSize, "DissolveState must fit State::data");

// wled00/FX.cpp:688 dissolve(color) - shared helper for mode_dissolve()/
// mode_dissolve_random(). state.data reinterpreted as DissolveState (32
// Rgb, one per column, mirroring the original's per-pixel uint32_t array).
void dissolve(const Params &p, State &state, Frame frame, Rgb color) {
  constexpr int width = GuDisplay::WIDTH;
  auto &s = *reinterpret_cast<DissolveState *>(state.data);

  if (state.call == 0) {
    for (int i = 0; i < width; i++) s.pixels[i] = p.secondary;
    state.aux0 = 1;
  }

  for (int j = 0; j <= width / 15; j++) {
    if (random8() <= p.intensity) {
      for (int times = 0; times < 10; times++) {
        int i = random16(static_cast<uint16_t>(width));
        if (state.aux0) {
          if (rgb_eq(s.pixels[i], p.secondary)) {
            Rgb c;
            if (rgb_eq(color, p.primary)) {
              uint8_t pal_idx = static_cast<uint8_t>((i * 255) / width);
              c = color_from_palette(p.palette_id, pal_idx, p.primary, p.secondary, p.tertiary);
            } else {
              c = color;
            }
            if (p.option2 && rgb_eq(c, p.secondary)) c.b ^= 1;
            s.pixels[i] = c;
            break;
          }
        } else {
          if (!rgb_eq(s.pixels[i], p.secondary)) {
            s.pixels[i] = p.secondary;
            break;
          }
        }
      }
    }
  }

  uint32_t incomplete = 0;
  for (int i = 0; i < width; i++) {
    fill_column(frame, i, s.pixels[i]);
    if (p.option2) {
      if (state.aux0) {
        if (rgb_eq(s.pixels[i], p.secondary)) incomplete++;
      } else {
        if (!rgb_eq(s.pixels[i], p.secondary)) incomplete++;
      }
    }
  }

  if (state.step > static_cast<uint32_t>(255 - p.speed) + 15u) {
    state.aux0 = state.aux0 ? 0 : 1;
    state.step = 0;
  } else {
    if (p.option2) {
      if (incomplete == 0) state.step++;
    } else {
      state.step++;
    }
  }
}

// wled00/FX.cpp:746 mode_dissolve()
void mode_dissolve(uint32_t, const Params &p, State &state, Frame frame) {
  Rgb color = p.option1 ? color_wheel(random8()) : p.primary;
  dissolve(p, state, frame, color);
}
EFFECTS_REGISTER(Id::kDissolve, mode_dissolve)

// wled00/FX.cpp:755 mode_dissolve_random()
void mode_dissolve_random(uint32_t, const Params &p, State &state, Frame frame) {
  dissolve(p, state, frame, color_wheel(random8()));
}
EFFECTS_REGISTER(Id::kDissolveRandom, mode_dissolve_random)

// wled00/FX.cpp:764 mode_sparkle(). The mcol=1 argument to the original's
// background-fill SEGMENT.color_from_palette() call only changes anything
// when palette_id==0 (see mode_scan()'s comment above on why this port
// drops that) - dropped here too.
void mode_sparkle(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  if (!p.option2) {
    for (int i = 0; i < width; i++) {
      uint8_t pal_idx = static_cast<uint8_t>((i * 255) / width);
      fill_column(frame, i, color_from_palette(p.palette_id, pal_idx, p.primary, p.secondary, p.tertiary));
    }
  }
  uint32_t cycle_time = 10 + (255 - p.speed) * 2;
  uint32_t it = now_ms / cycle_time;
  if (it != state.step) {
    state.aux0 = random16(static_cast<uint16_t>(width));
    state.step = it;
  }
  fill_column(frame, state.aux0, p.primary);
}
EFFECTS_REGISTER(Id::kSparkle, mode_sparkle)

// wled00/FX.cpp:784 mode_flash_sparkle(). SEGENV.aux0/.step are re-purposed
// mid-effect the same way real WLED does (aux0 holds a "last update"
// timestamp only up to the point it's read below, then becomes a delay
// value; step mirrors the opposite) - ported as-is rather than adding extra
// state fields, since it's already algebraically equivalent to a standard
// "time since last event > delay" check.
void mode_flash_sparkle(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  if (!p.option2) {
    for (int i = 0; i < width; i++) {
      uint8_t pal_idx = static_cast<uint8_t>((i * 255) / width);
      fill_column(frame, i, color_from_palette(p.palette_id, pal_idx, p.primary, p.secondary, p.tertiary));
    }
  }

  if (now_ms - state.aux0 > state.step) {
    if (random8(static_cast<uint8_t>((255 - p.intensity) >> 4)) == 0) {
      fill_column(frame, random16(static_cast<uint16_t>(width)), p.secondary);
    }
    state.step = now_ms;
    state.aux0 = static_cast<uint16_t>(255 - p.speed);
  }
}
EFFECTS_REGISTER(Id::kFlashSparkle, mode_flash_sparkle)

}  // namespace
}  // namespace effects
