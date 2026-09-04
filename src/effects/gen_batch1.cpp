#include "effects.h"
#include "wled_compat.h"
#include "palettes.h"
#include "display/gu_display.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

// Shared simplifications used throughout this file (documented once here
// rather than repeated per effect):
//  - Real WLED's SEGMENT.color_from_palette(i, mapping, moving, mcol, pbri)
//    takes an extra `mcol` (which SEGCOLOR to substitute when palette==0)
//    and `pbri` (post-lookup brightness scale) that effects::color_from_palette
//    doesn't have room for - every call below just forwards palette_id/
//    primary/secondary/tertiary per the porting cheat-sheet and ignores
//    mcol/pbri (none of this batch's calls use a non-default pbri anyway).
//  - gamma8inv()/gamma32inv() (WLED's SEGMENT-level inverse-gamma, applied
//    before its *own* separate gamma-correction pass) have no equivalent
//    here - this display already applies its own gamma via gu_display.h's
//    LUT, so there's nothing upstream of it to pre-compensate. Treated as
//    identity/dropped at each call site below.
namespace effects {
namespace {

uint8_t clamp_u8(int v) {
  if (v < 0) return 0;
  if (v > 255) return 255;
  return static_cast<uint8_t>(v);
}

// wled00/FX_fcn.cpp:1141 Segment::color_wheel() - CHSV32(pos<<8,255,255), a
// plain full-saturation hue wheel. Reimplemented locally (same math as
// effects.cpp's private copy) since that copy is TU-private and several
// effects in this batch need it too.
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

// wled00/util.cpp:748 get_random_wheel_index() - picks a random hue at
// least 42/255 away from `pos` so consecutive random colors look distinct.
uint8_t get_random_wheel_index(uint8_t pos) {
  uint8_t r = 0, x = 0, y = 0, d = 0;
  while (d < 42) {
    r = random8();
    x = static_cast<uint8_t>(std::abs(static_cast<int>(pos) - static_cast<int>(r)));
    y = 255 - x;
    d = std::min(x, y);
  }
  return r;
}

// wled00/FX.h:61-63: FRAMETIME_FIXED == 1000/WLED_FPS (WLED_FPS==42). This
// firmware doesn't track a separately-measured frame time (real WLED's
// FRAMETIME macro), so the fixed default stands in everywhere FRAMETIME is
// read below.
constexpr uint32_t kFrameTimeMs = 23;

// wled00/FX.cpp:195 blink() - shared by mode_strobe/mode_strobe_rainbow/
// mode_blink_rainbow.
void blink(uint32_t now_ms, const Params &p, State &state, Frame frame, Rgb color1, Rgb color2,
           bool strobe, bool do_palette) {
  constexpr int width = GuDisplay::WIDTH;
  uint32_t cycle_time = (255u - p.speed) * 20u;
  uint32_t on_time = kFrameTimeMs;
  if (!strobe) on_time += (cycle_time * p.intensity) >> 8;
  cycle_time += kFrameTimeMs * 2;
  uint32_t it = now_ms / cycle_time;
  uint32_t rem = now_ms % cycle_time;

  bool on = (it != state.step) || (rem <= on_time);
  state.step = it;

  if (on && do_palette) {
    for (int i = 0; i < width; i++) {
      uint8_t idx = static_cast<uint8_t>(std::min((i * 255) / width, 255));
      fill_column(frame, i, color_from_palette(p.palette_id, idx, p.primary, p.secondary, p.tertiary));
    }
  } else {
    Rgb c = on ? color1 : color2;
    for (int i = 0; i < width; i++) fill_column(frame, i, c);
  }
}

// wled00/FX.cpp:233 mode_blink_rainbow()
void mode_blink_rainbow(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  blink(now_ms, p, state, frame, color_wheel(static_cast<uint8_t>(state.call & 0xFF)), p.secondary,
        false, false);
}
EFFECTS_REGISTER(Id::kBlinkRainbow, mode_blink_rainbow)

// wled00/FX.cpp:242 mode_strobe()
void mode_strobe(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  blink(now_ms, p, state, frame, p.primary, p.secondary, true, true);
}
EFFECTS_REGISTER(Id::kStrobe, mode_strobe)

// wled00/FX.cpp:251 mode_strobe_rainbow()
void mode_strobe_rainbow(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  blink(now_ms, p, state, frame, color_wheel(static_cast<uint8_t>(state.call & 0xFF)), p.secondary,
        true, false);
}
EFFECTS_REGISTER(Id::kStrobeRainbow, mode_strobe_rainbow)

// wled00/FX.cpp:262 color_wipe(rev, useRandomColors) - only the
// rev=true/useRandomColors=true path is needed (mode_color_sweep_random);
// effects.cpp's mode_color_wipe already ports the rev=false/
// useRandomColors=false path separately (it's a different TU, can't share
// this copy with it).
void color_wipe_random(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  uint32_t cycle_time = 750 + (255u - p.speed) * 150u;
  uint32_t perc = now_ms % cycle_time;
  unsigned prog = static_cast<unsigned>((static_cast<uint64_t>(perc) * 65535) / cycle_time);
  bool back = prog > 32767;
  if (back) {
    prog -= 32767;
    if (state.step == 0) state.step = 1;
  } else {
    if (state.step == 2) state.step = 3;  // trigger next color change
  }

  if (state.call == 0) {
    state.aux0 = random8();
    state.step = 3;
  }
  if (state.step == 1) {
    state.aux1 = get_random_wheel_index(static_cast<uint8_t>(state.aux0));
    state.step = 2;
  }
  if (state.step == 3) {
    state.aux0 = get_random_wheel_index(static_cast<uint8_t>(state.aux1));
    state.step = 0;
  }

  unsigned led_index = (prog * static_cast<unsigned>(width)) >> 15;
  uint32_t rem = (prog * static_cast<unsigned>(width)) * 2;
  rem /= (p.intensity + 1);
  if (rem > 255) rem = 255;

  Rgb col1 = color_wheel(static_cast<uint8_t>(state.aux1));
  for (int i = 0; i < width; i++) {
    int index = back ? (width - 1 - i) : i;
    Rgb col0 = color_wheel(static_cast<uint8_t>(state.aux0));
    Rgb px;
    if (static_cast<unsigned>(i) < led_index) {
      px = back ? col1 : col0;
    } else {
      px = back ? col0 : col1;
      if (static_cast<unsigned>(i) == led_index)
        px = blend(back ? col0 : col1, back ? col1 : col0, static_cast<uint8_t>(rem));
    }
    fill_column(frame, index, px);
  }
}

// wled00/FX.cpp:344 mode_color_sweep_random() - color_wipe(true, true)
void mode_color_sweep_random(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  color_wipe_random(now_ms, p, state, frame);
}
EFFECTS_REGISTER(Id::kColorSweepRandom, mode_color_sweep_random)

// wled00/FX.cpp:804 mode_hyper_sparkle()
void mode_hyper_sparkle(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  if (!p.option2) {  // check2 == "Overlay": when set, sparkles draw over whatever's already there
    for (int i = 0; i < width; i++) {
      uint8_t idx = static_cast<uint8_t>(std::min((i * 255) / width, 255));
      fill_column(frame, i, color_from_palette(p.palette_id, idx, p.primary, p.secondary, p.tertiary));
    }
  }
  if (now_ms - state.aux0 > state.step) {
    if (random8((255 - p.intensity) >> 4) == 0) {
      int len = std::max(1, width / 3);
      for (int i = 0; i < len; i++) fill_column(frame, random16(width), p.secondary);
    }
    state.step = now_ms;
    state.aux0 = static_cast<uint16_t>(255 - p.speed);
  }
}
EFFECTS_REGISTER(Id::kHyperSparkle, mode_hyper_sparkle)

// wled00/FX.cpp:826 mode_multi_strobe()
void mode_multi_strobe(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  for (int i = 0; i < width; i++) {
    uint8_t idx = static_cast<uint8_t>(std::min((i * 255) / width, 255));
    fill_column(frame, i, color_from_palette(p.palette_id, idx, p.primary, p.secondary, p.tertiary));
  }

  state.aux0 = static_cast<uint16_t>(50 + 20 * (255 - p.speed));
  unsigned count = 2 * ((p.intensity / 10) + 1);
  if (state.aux1 < count) {
    if ((state.aux1 & 1) == 0) {
      for (int i = 0; i < width; i++) fill_column(frame, i, p.primary);
      state.aux0 = 15;
    } else {
      state.aux0 = 50;
    }
  }

  if (now_ms - state.aux0 > state.step) {
    state.aux1++;
    if (state.aux1 > count) state.aux1 = 0;
    state.step = now_ms;
  }
}
EFFECTS_REGISTER(Id::kMultiStrobe, mode_multi_strobe)

// wled00/FX.cpp:854 mode_android()
struct AndroidState {
  uint32_t counter;
};
static_assert(sizeof(AndroidState) <= State::kDataSize, "AndroidState must fit in State::data");

void mode_android(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  auto &s = *reinterpret_cast<AndroidState *>(state.data);

  unsigned size = state.aux1 >> 1;
  unsigned shrinking = state.aux1 & 0x01;
  if (now_ms >= state.step) {
    state.step = now_ms + 3 + ((8u * (255 - p.speed)) / width);
    if (size > (p.intensity * static_cast<unsigned>(width)) / 255)
      shrinking = 1;
    else if (size < 2)
      shrinking = 0;
    if (!shrinking) {
      if ((s.counter % 3) == 1) state.aux0++;
      else size++;
    } else {
      state.aux0++;
      if ((s.counter % 3) != 1) size--;
    }
    state.aux1 = static_cast<uint16_t>((size << 1) | shrinking);
    s.counter++;
    if (state.aux0 >= width) state.aux0 = 0;
  }
  unsigned start = state.aux0;
  unsigned end = (state.aux0 + size) % width;
  for (int i = 0; i < width; i++) {
    if ((start < end && static_cast<unsigned>(i) >= start && static_cast<unsigned>(i) < end) ||
        (start >= end && (static_cast<unsigned>(i) >= start || static_cast<unsigned>(i) < end)))
      fill_column(frame, i, p.primary);
    else {
      uint8_t idx = static_cast<uint8_t>(std::min((i * 255) / width, 255));
      fill_column(frame, i, color_from_palette(p.palette_id, idx, p.primary, p.secondary, p.tertiary));
    }
  }
}
EFFECTS_REGISTER(Id::kAndroid, mode_android)

// wled00/FX.cpp:895 chase() - shared by mode_chase_color/mode_chase_random/
// mode_chase_rainbow/mode_chase_rainbow_white. The original special-cases
// on `SEGMENT.mode == FX_MODE_CHASE_RANDOM`; we don't track a "current
// effect ID" inside the helper, so that check becomes an explicit
// `chase_random` parameter set only by mode_chase_random's call site.
void chase(uint32_t now_ms, const Params &p, State &state, Frame frame, Rgb color1, Rgb color2,
           Rgb color3, bool do_palette, bool chase_random) {
  constexpr int width = GuDisplay::WIDTH;
  uint16_t counter = static_cast<uint16_t>(now_ms * ((p.speed >> 2) + 1));
  uint16_t a = static_cast<uint16_t>((static_cast<uint32_t>(counter) * width) >> 16);

  if (chase_random) {
    if (a < state.step) {
      state.aux1 = state.aux0;
      state.aux0 = get_random_wheel_index(static_cast<uint8_t>(state.aux0));
    }
    color1 = color_wheel(static_cast<uint8_t>(state.aux0));
  }
  state.step = a;

  unsigned size = 1 + ((static_cast<unsigned>(p.intensity) * width) >> 10);
  uint16_t b = static_cast<uint16_t>(a + size);
  if (b > width) b = static_cast<uint16_t>(b - width);
  uint16_t c = static_cast<uint16_t>(b + size);
  if (c > width) c = static_cast<uint16_t>(c - width);

  if (do_palette) {
    for (int i = 0; i < width; i++) {
      uint8_t idx = static_cast<uint8_t>(std::min((i * 255) / width, 255));
      fill_column(frame, i, color_from_palette(p.palette_id, idx, p.primary, p.secondary, p.tertiary));
    }
  } else {
    for (int i = 0; i < width; i++) fill_column(frame, i, color1);
  }

  if (chase_random) {
    Rgb old_color1 = color_wheel(static_cast<uint8_t>(state.aux1));
    for (int i = a; i < width; i++) fill_column(frame, i, old_color1);
  }

  if (a < b) {
    for (int i = a; i < b; i++) fill_column(frame, i, color2);
  } else {
    for (int i = a; i < width; i++) fill_column(frame, i, color2);
    for (int i = 0; i < b; i++) fill_column(frame, i, color2);
  }

  if (b < c) {
    for (int i = b; i < c; i++) fill_column(frame, i, color3);
  } else {
    for (int i = b; i < width; i++) fill_column(frame, i, color3);
    for (int i = 0; i < c; i++) fill_column(frame, i, color3);
  }
}

// wled00/FX.cpp:963 mode_chase_color()
void mode_chase_color(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  Rgb color3 = (p.tertiary.r || p.tertiary.g || p.tertiary.b) ? p.tertiary : p.primary;
  chase(now_ms, p, state, frame, p.secondary, color3, p.primary, true, false);
}
EFFECTS_REGISTER(Id::kChaseColor, mode_chase_color)

// wled00/FX.cpp:972 mode_chase_random()
void mode_chase_random(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  Rgb color3 = (p.tertiary.r || p.tertiary.g || p.tertiary.b) ? p.tertiary : p.primary;
  chase(now_ms, p, state, frame, p.secondary, color3, p.primary, false, true);
}
EFFECTS_REGISTER(Id::kChaseRandom, mode_chase_random)

// wled00/FX.cpp:981 mode_chase_rainbow()
void mode_chase_rainbow(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  unsigned color_sep = width >= 256 ? 1 : 256 / width;
  unsigned color_index = state.call & 0xFF;
  Rgb color = color_wheel(static_cast<uint8_t>(((state.step * color_sep) + color_index) & 0xFF));
  chase(now_ms, p, state, frame, color, p.primary, p.secondary, false, false);
}
EFFECTS_REGISTER(Id::kChaseRainbow, mode_chase_rainbow)

// wled00/FX.cpp:995 mode_chase_rainbow_white()
void mode_chase_rainbow_white(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  uint16_t n = static_cast<uint16_t>(state.step);
  uint16_t m = static_cast<uint16_t>((state.step + 1) % width);
  Rgb color2 = color_wheel(static_cast<uint8_t>(((n * 256 / width) + (state.call & 0xFF)) & 0xFF));
  Rgb color3 = color_wheel(static_cast<uint8_t>(((m * 256 / width) + (state.call & 0xFF)) & 0xFF));
  chase(now_ms, p, state, frame, p.primary, color2, color3, false, false);
}
EFFECTS_REGISTER(Id::kChaseRainbowWhite, mode_chase_rainbow_white)

// wled00/FX.cpp:1009 mode_colorful()
void mode_colorful(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  unsigned num_colors = 4;
  Rgb cols[9] = {{0xFF, 0x00, 0x00}, {0xEE, 0xBB, 0x00}, {0x00, 0xEE, 0x00}, {0x00, 0x77, 0xCC}};
  if (p.intensity > 160 || p.palette_id != 0) {
    if (p.palette_id == 0) {
      num_colors = 3;
      cols[0] = p.primary;
      cols[1] = p.secondary;
      cols[2] = p.tertiary;
    } else {
      unsigned fac = 80;
      if (p.palette_id == 52) {
        num_colors = 5;
        fac = 61;
      }
      for (unsigned i = 0; i < num_colors; i++)
        cols[i] = color_from_palette(p.palette_id, static_cast<uint8_t>(i * fac), p.primary, p.secondary,
                                      p.tertiary);
    }
  } else if (p.intensity < 80) {
    cols[0] = Rgb{0xFF, 0x80, 0x40};
    cols[1] = Rgb{0xE5, 0xD2, 0x41};
    cols[2] = Rgb{0x77, 0xFF, 0x77};
    cols[3] = Rgb{0x77, 0xF0, 0xF0};
  }
  for (unsigned i = num_colors; i < num_colors * 2 - 1; i++) cols[i] = cols[i - num_colors];

  uint32_t cycle_time = 50 + (8u * (255 - p.speed));
  uint32_t it = now_ms / cycle_time;
  if (it != state.step) {
    if (p.speed > 0) state.aux0++;
    if (state.aux0 >= num_colors) state.aux0 = 0;
    state.step = it;
  }

  for (int i = 0; i < width; i += static_cast<int>(num_colors)) {
    for (unsigned j = 0; j < num_colors; j++) {
      int x = i + static_cast<int>(j);
      if (x < width) fill_column(frame, x, cols[state.aux0 + j]);
    }
  }
}
EFFECTS_REGISTER(Id::kColorful, mode_colorful)

// wled00/FX.cpp:1052 mode_traffic_light()
void mode_traffic_light(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  for (int i = 0; i < width; i++) {
    uint8_t idx = static_cast<uint8_t>(std::min((i * 255) / width, 255));
    fill_column(frame, i, color_from_palette(p.palette_id, idx, p.primary, p.secondary, p.tertiary));
  }
  uint32_t mdelay = 500;
  constexpr Rgb kRed{0xFF, 0x00, 0x00};
  constexpr Rgb kAmber{0xEE, 0xCC, 0x00};  // gamma32inv(0x00EECC00) treated as identity, see file-top note
  constexpr Rgb kGreen{0x00, 0xFF, 0x00};
  for (int i = 0; i < width - 2; i += 3) {
    switch (state.aux0) {
      case 0:
        fill_column(frame, i, kRed);
        mdelay = 150 + (100u * (255 - p.speed));
        break;
      case 1:
        fill_column(frame, i, kRed);
        mdelay = 150 + (20u * (255 - p.speed));
        fill_column(frame, i + 1, kAmber);
        break;
      case 2:
        fill_column(frame, i + 2, kGreen);
        mdelay = 150 + (100u * (255 - p.speed));
        break;
      case 3:
        fill_column(frame, i + 1, kAmber);
        mdelay = 150 + (20u * (255 - p.speed));
        break;
    }
  }
  if (now_ms - state.step > mdelay) {
    state.aux0++;
    if (state.aux0 == 1 && p.intensity > 140) state.aux0 = 2;  // US-style: skip Red+Amber
    if (state.aux0 > 3) state.aux0 = 0;
    state.step = now_ms;
  }
}
EFFECTS_REGISTER(Id::kTrafficLight, mode_traffic_light)

constexpr int kFlashCount = 4;

// wled00/FX.cpp:1083 mode_chase_flash()
void mode_chase_flash(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  bool advance = true;
  unsigned flash_step = state.aux1 % ((kFlashCount * 2) + 1);
  if (now_ms < state.step) advance = false;
  else state.aux1++;

  for (int i = 0; i < width; i++) {
    uint8_t idx = static_cast<uint8_t>(std::min((i * 255) / width, 255));
    fill_column(frame, i, color_from_palette(p.palette_id, idx, p.primary, p.secondary, p.tertiary));
  }
  unsigned index = state.aux0;
  unsigned n = index;
  unsigned m = (index + 1) % width;

  uint32_t delay = 10 + ((30u * (255 - p.speed)) / width);
  if (flash_step < static_cast<unsigned>(kFlashCount * 2)) {
    if (flash_step % 2 == 0) {
      fill_column(frame, n, p.secondary);
      fill_column(frame, m, p.secondary);
      delay = 20;
    } else {
      delay = 30;
    }
  } else if (advance) {
    state.aux0 = static_cast<uint16_t>(m);
  }
  if (advance) state.step = now_ms + delay;
}
EFFECTS_REGISTER(Id::kChaseFlash, mode_chase_flash)

// wled00/FX.cpp:1121 mode_chase_flash_random()
void mode_chase_flash_random(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  bool advance = true;
  if (now_ms < state.step) {
    state.call--;  // revert the frame-counter advance, re-render the same frame
    advance = false;
  }
  unsigned flash_step = state.call % ((kFlashCount * 2) + 1);

  for (int i = 0; i < state.aux1 && i < width; i++) fill_column(frame, i, color_wheel(static_cast<uint8_t>(state.aux0)));

  uint32_t delay = 1 + ((10u * (255 - p.speed)) / width);
  if (flash_step < static_cast<unsigned>(kFlashCount * 2)) {
    unsigned n = state.aux1;
    unsigned m = (state.aux1 + 1) % width;
    if (flash_step % 2 == 0) {
      fill_column(frame, n, p.primary);
      fill_column(frame, m, p.primary);
      delay = 20;
    } else {
      fill_column(frame, n, color_wheel(static_cast<uint8_t>(state.aux0)));
      fill_column(frame, m, p.secondary);
      delay = 30;
    }
  } else if (advance) {
    state.aux1 = static_cast<uint16_t>((state.aux1 + 1) % width);
    if (state.aux1 == 0) state.aux0 = get_random_wheel_index(static_cast<uint8_t>(state.aux0));
  }
  if (advance) state.step = now_ms + delay;
}
EFFECTS_REGISTER(Id::kChaseFlashRandom, mode_chase_flash_random)

// wled00/FX.cpp:546 running() - shared by mode_running_color (theatre=false
// case only; mode_theater_chase/_rainbow use the theatre=true case but
// aren't in this batch). The original picks "use palette per-pixel instead
// of a flat fill" by comparing `color1 == SEGCOLOR(0)` at runtime; ported
// as an explicit `use_palette` bool set by the one call site instead, since
// that's exactly what the comparison always evaluates to there.
void running(uint32_t now_ms, const Params &p, State &state, Frame frame, Rgb color1, Rgb color2,
             bool use_palette) {
  constexpr int width = GuDisplay::WIDTH;
  int w = 1 + (p.intensity >> 4);
  uint32_t cycle_time = 50 + (255 - p.speed);
  uint32_t it = now_ms / cycle_time;
  int aux0 = state.aux0;

  for (int i = 0; i < width; i++) {
    Rgb col = color2;
    Rgb col1 = color1;
    if (use_palette) {
      uint8_t idx = static_cast<uint8_t>(std::min((i * 255) / width, 255));
      col1 = color_from_palette(p.palette_id, idx, p.primary, p.secondary, p.tertiary);
    }
    int pos = i % (w << 1);
    if ((pos < aux0 - w) || (pos >= aux0 && pos < aux0 + w)) col = col1;
    fill_column(frame, i, col);
  }

  if (it != state.step) {
    state.aux0 = static_cast<uint16_t>((aux0 + 1) % (w << 1));
    state.step = it;
  }
}

// wled00/FX.cpp:1164 mode_running_color()
void mode_running_color(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  running(now_ms, p, state, frame, p.primary, p.secondary, true);
}
EFFECTS_REGISTER(Id::kRunningColor, mode_running_color)

// wled00/FX.cpp:1173 mode_running_random()
void mode_running_random(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  uint32_t cycle_time = 25 + (3u * (255 - p.speed));
  uint32_t it = now_ms / cycle_time;
  if (state.call == 0) state.aux0 = random16();  // random seed for the PRNG below

  unsigned zone_size = ((255 - p.intensity) >> 4) + 1;
  uint16_t prng16 = state.aux0;

  unsigned z = it % zone_size;
  bool nzone = (!z && it != state.aux1);
  for (int i = width - 1; i >= 0; i--) {
    if (nzone || z >= zone_size) {
      unsigned lastrand = prng16 >> 8;
      int16_t diff = 0;
      while (std::abs(diff) < 42) {
        prng16 = static_cast<uint16_t>(prng16 * 2053) + 13849;
        diff = static_cast<int16_t>((prng16 >> 8) - lastrand);
      }
      if (nzone) {
        state.aux0 = prng16;
        nzone = false;
      }
      z = 0;
    }
    fill_column(frame, i, color_wheel(static_cast<uint8_t>(prng16 >> 8)));
    z++;
  }
  state.aux1 = static_cast<uint16_t>(it);
}
EFFECTS_REGISTER(Id::kRunningRandom, mode_running_random)

// wled00/FX.cpp:4808-4952 mode_aurora() and its AuroraWave helper class,
// converted to integer math by @dedehai. Ported with two drops:
//  - CRGBW's white channel -> plain Rgb (this board has no white channel).
//  - the `backlight` pre-glow (gamma8inv() of a 0-3 SEGCOLOR-set count, so
//    the background isn't pure black post-gamma) is dropped rather than
//    faked - see the file-top note on gamma8inv/gamma32inv.
constexpr int kAuroraMaxWaves = 20;    // W_MAX_COUNT (non-ESP8266 branch)
constexpr int kAuroraMaxSpeed = 6;     // W_MAX_SPEED
constexpr int kAuroraWidthFactor = 6;  // W_WIDTH_FACTOR
constexpr int kAwShift = 16;           // AW_SHIFT
constexpr uint32_t kAwScale = 1u << kAwShift;  // AW_SCALE

struct AuroraWave {
  int32_t center;               // scaled by kAwScale
  uint32_t age_factor_cached;    // scaled by kAwScale
  uint16_t ttl;
  uint16_t age;
  uint16_t width;
  uint16_t basealpha;      // scaled by kAwScale
  uint16_t speed_factor;   // scaled by kAwScale
  int16_t wave_start;
  int16_t wave_end;
  bool going_left;
  bool alive;
  Rgb basecolor;

  void init(uint32_t segment_length, Rgb color) {
    ttl = random16(500, 1501);
    basecolor = color;
    basealpha = static_cast<uint16_t>(random8(60, 100) * kAwScale / 100);
    age = 0;
    width = static_cast<uint16_t>(random16(segment_length / 20, segment_length / kAuroraWidthFactor) + 1);
    center = static_cast<int32_t>(((static_cast<uint32_t>(random8(101)) << kAwShift) / 100) * segment_length);
    going_left = (random8() & 0x01) != 0;
    speed_factor =
        static_cast<uint16_t>((static_cast<uint32_t>(random8(10, 31)) * kAuroraMaxSpeed << kAwShift) / (100 * 255));
    alive = true;
  }

  void update_cached_values() {
    uint32_t half_ttl = ttl >> 1;
    if (half_ttl == 0) half_ttl = 1;
    if (age < half_ttl) age_factor_cached = (static_cast<uint32_t>(age) << kAwShift) / half_ttl;
    else age_factor_cached = (static_cast<uint32_t>(ttl - age) << kAwShift) / half_ttl;
    if (age_factor_cached >= kAwScale) age_factor_cached = kAwScale - 1;

    int32_t center_led = center >> kAwShift;
    wave_start = static_cast<int16_t>(center_led - static_cast<int32_t>(width));
    wave_end = static_cast<int16_t>(center_led + static_cast<int32_t>(width));
  }

  Rgb color_for_led(int led_index) const {
    if (led_index < wave_start || led_index > wave_end) return Rgb{0, 0, 0};
    int32_t offset = (static_cast<int32_t>(led_index) << kAwShift) - center;
    if (offset < 0) offset = -offset;
    uint32_t offset_factor = static_cast<uint32_t>(offset) / (width ? width : 1);
    if (offset_factor > kAwScale) return Rgb{0, 0, 0};
    uint32_t brightness_factor = kAwScale - offset_factor;
    brightness_factor = (brightness_factor * age_factor_cached) >> kAwShift;
    brightness_factor = (brightness_factor * basealpha) >> kAwShift;
    return Rgb{static_cast<uint8_t>((basecolor.r * brightness_factor) >> kAwShift),
               static_cast<uint8_t>((basecolor.g * brightness_factor) >> kAwShift),
               static_cast<uint8_t>((basecolor.b * brightness_factor) >> kAwShift)};
  }

  void update(uint32_t segment_length, uint32_t speed) {
    int32_t step = static_cast<int32_t>(speed_factor) * static_cast<int32_t>(speed);
    center += going_left ? -step : step;
    age++;
    if (age > ttl) {
      alive = false;
      return;
    }
    int32_t width_scaled = static_cast<int32_t>(width) << kAwShift;
    int32_t segment_length_scaled = static_cast<int32_t>(segment_length) << kAwShift;
    if (going_left) {
      if (center < -width_scaled) alive = false;
    } else {
      if (center > segment_length_scaled + width_scaled) alive = false;
    }
  }

  bool still_alive() const { return alive; }
};

struct AuroraState {
  AuroraWave waves[kAuroraMaxWaves];
};
static_assert(sizeof(AuroraState) <= State::kDataSize, "AuroraState must fit in State::data");

void mode_aurora(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  (void)now_ms;
  constexpr int width = GuDisplay::WIDTH;
  auto &s = *reinterpret_cast<AuroraState *>(state.data);

  state.aux1 = static_cast<uint16_t>(2 + (p.intensity * (kAuroraMaxWaves - 2)) / 255);  // map(intensity,0,255,2,kAuroraMaxWaves)
  for (int i = 0; i < state.aux1; i++) {
    s.waves[i].update(width, p.speed);
    if (!s.waves[i].still_alive()) {
      s.waves[i].init(width, color_from_palette(p.palette_id, random8(), p.primary, p.secondary, p.tertiary));
    }
    s.waves[i].update_cached_values();
  }

  for (int x = 0; x < width; x++) {
    unsigned r = 0, g = 0, b = 0;
    for (int j = 0; j < state.aux1; j++) {
      Rgb c = s.waves[j].color_for_led(x);
      r = qadd8(static_cast<uint8_t>(r), c.r);
      g = qadd8(static_cast<uint8_t>(g), c.g);
      b = qadd8(static_cast<uint8_t>(b), c.b);
    }
    fill_column(frame, x, Rgb{static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b)});
  }
}
EFFECTS_REGISTER(Id::kAurora, mode_aurora)

}  // namespace
}  // namespace effects
