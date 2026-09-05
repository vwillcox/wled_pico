#include "effects.h"

#include <math.h>

#include "display/gu_display.h"
#include "image_data.h"
#include "palettes.h"
#include "wled_compat.h"

// This batch's ColorFromPalette() calls all drop WLED's extra "mcol"
// argument (which SEGCOLOR to fall back to when palette==0) - our
// color_from_palette() always takes primary/secondary/tertiary together,
// see palettes.h. Not called out per call site below, just once here.
namespace effects {
namespace {

// main.cpp's loop() calls effects::render() once per ~16ms tick (a fixed
// delay(16), not WLED's own dynamically-measured strip.getFrameTime()) -
// stand-in for FRAMETIME/FRAMETIME_FIXED wherever the original scales a
// per-frame accumulator by it.
constexpr uint32_t kFrameTimeMs = 16;

long map_range(long x, long in_min, long in_max, long out_min, long out_max) {
  if (in_max == in_min) return out_min;
  return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

uint8_t palette_index_mapped(int i, int len) {
  int v = (i * 255) / len;
  if (v < 0) v = 0;
  if (v > 255) v = 255;
  return static_cast<uint8_t>(v);
}

Rgb scale_rgb(Rgb c, uint8_t s) { return Rgb{scale8(c.r, s), scale8(c.g, s), scale8(c.b, s)}; }

Rgb seg_color3(const Params &p, int idx) {
  switch (idx % 3) {
    case 0: return p.primary;
    case 1: return p.secondary;
    default: return p.tertiary;
  }
}

bool is_black(Rgb c) { return c.r == 0 && c.g == 0 && c.b == 0; }
bool rgb_eq(Rgb a, Rgb b) { return a.r == b.r && a.g == b.g && a.b == b.b; }

// wled00/FX.cpp:90 sin_gap()
uint8_t sin_gap(uint16_t in) {
  if (in & 0x100) return 0;
  return sin8(static_cast<uint8_t>(in + 192));
}

// Coarse approximation of WLED's gamma8() LUT (led.cpp) - this codebase
// hasn't ported a gamma table, so a gamma-2.8 power curve stands in
// (matches the "shape, not bit-exact" tolerance wled_compat.h documents
// for its own sin8/beatsin8 approximations).
uint8_t gamma8_approx(uint8_t x) {
  return static_cast<uint8_t>(powf(x / 255.0f, 2.8f) * 255.0f + 0.5f);
}

// wled00/FX_fcn.cpp:1067 Segment::fade_out() - fades toward `bg`
// (SEGCOLOR(1) in the original), not toward black, so this can't reuse
// wled_compat.h's fade_to_black_by().
Rgb fade_out_pixel(Rgb color, Rgb bg, uint8_t rate) {
  if (rgb_eq(color, bg)) return color;
  uint8_t rate2 = static_cast<uint8_t>((256 - rate) >> 1);
  int mapped_rate = 256 / (rate2 + 1);
  auto fade_ch = [&](uint8_t c1, uint8_t c2) -> uint8_t {
    int delta = (static_cast<int>(c2) - c1) * mapped_rate / 256;
    if (delta == 0) delta = (c2 == c1) ? 0 : (c2 > c1 ? 1 : -1);
    int v = c1 + delta;
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    return static_cast<uint8_t>(v);
  };
  return Rgb{fade_ch(color.r, bg.r), fade_ch(color.g, bg.g), fade_ch(color.b, bg.b)};
}

void fade_out_frame(Frame frame, Rgb bg, uint8_t rate) {
  for (int y = 0; y < GuDisplay::HEIGHT; y++)
    for (int x = 0; x < GuDisplay::WIDTH; x++) frame[y][x] = fade_out_pixel(frame[y][x], bg, rate);
}

// wled00/FX_2Dfcn.cpp:246 Segment::blur2D(), symmetrical (blur_x==blur_y),
// smear=false - the shape mode_fireworks()/mode_rain() need via
// SEGMENT.blur(16) on a 2D segment.
void blur2d(Frame frame, uint8_t amount) {
  if (!amount) return;
  constexpr int width = GuDisplay::WIDTH;
  constexpr int height = GuDisplay::HEIGHT;
  uint8_t keep = static_cast<uint8_t>(255 - amount);
  uint8_t seep = static_cast<uint8_t>(amount >> 1);
  for (int y = 0; y < height; y++) {
    Rgb cur = frame[y][0];
    Rgb carry = scale_rgb(cur, seep);
    frame[y][0] = scale_rgb(cur, keep);
    for (int x = 1; x < width; x++) {
      cur = frame[y][x];
      Rgb part = scale_rgb(cur, seep);
      cur = scale_rgb(cur, keep);
      cur = Rgb{qadd8(cur.r, carry.r), qadd8(cur.g, carry.g), qadd8(cur.b, carry.b)};
      Rgb &prev = frame[y][x - 1];
      prev = Rgb{qadd8(prev.r, part.r), qadd8(prev.g, part.g), qadd8(prev.b, part.b)};
      frame[y][x] = cur;
      carry = part;
    }
  }
  for (int x = 0; x < width; x++) {
    Rgb cur = frame[0][x];
    Rgb carry = scale_rgb(cur, seep);
    frame[0][x] = scale_rgb(cur, keep);
    for (int y = 1; y < height; y++) {
      cur = frame[y][x];
      Rgb part = scale_rgb(cur, seep);
      cur = scale_rgb(cur, keep);
      cur = Rgb{qadd8(cur.r, carry.r), qadd8(cur.g, carry.g), qadd8(cur.b, carry.b)};
      Rgb &prev = frame[y - 1][x];
      prev = Rgb{qadd8(prev.r, part.r), qadd8(prev.g, part.g), qadd8(prev.b, part.b)};
      frame[y][x] = cur;
      carry = part;
    }
  }
}

// wled00/FX.cpp:1209 mode_larson_scanner()
void mode_larson_scanner(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  unsigned speed = kFrameTimeMs * static_cast<unsigned>(map_range(p.speed, 0, 255, 96, 2));
  unsigned pixels = width / (speed ? speed : 1);

  fade_out_frame(frame, p.secondary, static_cast<uint8_t>(255 - p.intensity));

  if (state.step > now_ms) return;

  unsigned index = state.aux1 + pixels;
  if (pixels == 0) {
    unsigned frames = speed / width;
    if (state.step++ < frames) return;
    state.step = 0;
    index++;
  }

  if (index > static_cast<unsigned>(width)) {
    state.aux0 = state.aux0 ? 0 : 1;  // flip direction
    state.aux1 = 0;
    if (state.aux0 || p.option2) state.step = now_ms + p.custom1 * 25;
    else state.step = 0;
  } else {
    for (unsigned i = state.aux1; i < index; i++) {
      unsigned j = state.aux0 ? i : static_cast<unsigned>(width) - 1 - i;
      if (j >= static_cast<unsigned>(width)) continue;
      Rgb c = color_from_palette(p.palette_id, palette_index_mapped(j, width), p.primary, p.secondary, p.tertiary);
      fill_column(frame, j, c);
      if (p.option1) {
        unsigned mirror = static_cast<unsigned>(width) - 1 - j;
        fill_column(frame, mirror, is_black(p.tertiary) ? c : p.tertiary);
      }
    }
    state.aux1 = static_cast<uint16_t>(index);
  }
}
EFFECTS_REGISTER(Id::kLarsonScanner, mode_larson_scanner)

// wled00/FX.cpp:1265 mode_comet()
void mode_comet(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  unsigned counter = static_cast<unsigned>(now_ms * ((p.speed >> 2) + 1)) & 0xFFFFu;
  unsigned index = (counter * static_cast<unsigned>(width)) >> 16;
  if (state.call == 0) state.aux0 = static_cast<uint16_t>(index);

  fade_out_frame(frame, p.secondary, p.intensity);

  fill_column(frame, index, color_from_palette(p.palette_id, palette_index_mapped(index, width), p.primary, p.secondary, p.tertiary));
  if (index > state.aux0) {
    for (unsigned i = state.aux0; i < index; i++)
      fill_column(frame, i, color_from_palette(p.palette_id, palette_index_mapped(i, width), p.primary, p.secondary, p.tertiary));
  } else if (index < state.aux0 && index < 10) {
    for (unsigned i = 0; i < index; i++)
      fill_column(frame, i, color_from_palette(p.palette_id, palette_index_mapped(i, width), p.primary, p.secondary, p.tertiary));
  }
  state.aux0 = static_cast<uint16_t>(index);
}
EFFECTS_REGISTER(Id::kComet, mode_comet)

// Shared by mode_fireworks()/mode_rain() - wled00/FX.cpp:1314's spark-spawn
// loop, factored out since both effects run it every frame.
void fireworks_spawn_sparks(const Params &p, State &state, Frame frame, int width, int height) {
  int tries = width / 20;
  if (tries < 1) tries = 1;
  for (int i = 0; i < tries; i++) {
    if (random8(static_cast<uint8_t>(129 - (p.intensity >> 1))) == 0) {
      uint16_t idx = random16(static_cast<uint16_t>(width * height));
      int x = idx % width, y = idx / width;
      frame[y][x] = color_from_palette(p.palette_id, random8(), p.primary, p.secondary, p.tertiary);
      state.aux1 = state.aux0;
      state.aux0 = idx;
    }
  }
}

// wled00/FX.cpp:1290 mode_fireworks(), 2D branch (this board is always
// SEGMENT.is2D()==true). `state.step` never gets set by this function so
// the blur/restore preamble always runs - unlike mode_rain() below, which
// shares this same original source function but keeps step nonzero.
void mode_fireworks(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  (void)now_ms;
  constexpr int width = GuDisplay::WIDTH;
  constexpr int height = GuDisplay::HEIGHT;
  if (state.call == 0) {
    state.aux0 = 0xFFFFu;
    state.aux1 = 0xFFFFu;
  }
  fade_out_frame(frame, p.secondary, 128);

  int x = state.aux0 % width, y = state.aux0 / width;
  bool valid1 = state.aux0 < width * height;
  bool valid2 = state.aux1 < width * height;
  Rgb sv1{0, 0, 0}, sv2{0, 0, 0};
  // Upstream reuses aux0's (x,y) for the aux1 restore too on 2D segments
  // (wled00/FX.cpp:1308) instead of recomputing aux1's own coordinates -
  // reproduced verbatim, not a typo on this end.
  if (valid1) sv1 = frame[y][x];
  if (valid2) sv2 = frame[y][x];
  blur2d(frame, 16);
  if (valid1) frame[y][x] = sv1;
  if (valid2) frame[y][x] = sv2;

  fireworks_spawn_sparks(p, state, frame, width, height);
}
EFFECTS_REGISTER(Id::kFireworks, mode_fireworks)

// wled00/FX.cpp:1330 mode_rain(), 2D branch. Its fireworks_spawn_sparks()
// call (mode_fireworks() in the original) never hits the blur/restore
// preamble in practice - `SEGENV.step` is unconditionally bumped by
// FRAMETIME above that check every frame, so it's never 0 when that branch
// is tested - so it's omitted here rather than ported dead.
void mode_rain(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  (void)now_ms;
  constexpr int width = GuDisplay::WIDTH;
  constexpr int height = GuDisplay::HEIGHT;
  state.step += kFrameTimeMs;
  uint32_t speed_formula_l = 5 + (50 * (255 - static_cast<uint32_t>(p.speed))) / width;
  if (state.call && state.step > speed_formula_l) {
    state.step = 1;
    for (int x = 0; x < width; x++) {
      Rgb bottom = frame[height - 1][x];
      for (int y = height - 1; y > 0; y--) frame[y][x] = frame[y - 1][x];
      frame[0][x] = bottom;
    }
    state.aux0 = static_cast<uint16_t>((state.aux0 % width) + (state.aux0 / width + 1) * width);
    state.aux1 = static_cast<uint16_t>((state.aux1 % width) + (state.aux1 / width + 1) * width);
    if (state.aux0 == 0) state.aux0 = 0xFFFFu;
    // wled00/FX.cpp:1355 checks aux1==0 but resets aux0 here too, not
    // aux1 - an upstream bug, reproduced as-is.
    if (state.aux1 == 0) state.aux0 = 0xFFFFu;
    if (state.aux0 >= width * height) state.aux0 = 0;
    if (state.aux1 >= width * height) state.aux1 = 0;
  }

  fade_out_frame(frame, p.secondary, 128);
  fireworks_spawn_sparks(p, state, frame, width, height);
}
EFFECTS_REGISTER(Id::kRain, mode_rain)

// wled00/FX.cpp:3961 mode_tetrix(). On a 2D segment WLED runs one
// independent falling-brick "virtual strip" per column (nrOfVStrips()) -
// this board is always 2D, so that's genuinely 32 independent columns,
// each SEGLEN==GuDisplay::HEIGHT tall, not a single 32-wide 1D effect.
struct TetrisDrop {
  float pos;
  float speed;
  uint8_t col;
  uint16_t brick;
  uint16_t stack;
  uint32_t step;
};
struct TetrixState {
  TetrisDrop drops[GuDisplay::WIDTH];
};
static_assert(sizeof(TetrixState) <= State::kDataSize, "TetrixState too big for State::data");

void mode_tetrix(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  constexpr int height = GuDisplay::HEIGHT;
  constexpr int width = GuDisplay::WIDTH;
  auto &s = *reinterpret_cast<TetrixState *>(state.data);

  for (int col = 0; col < width; col++) {
    TetrisDrop &drop = s.drops[col];

    if (state.call == 0) {
      drop.stack = 0;
      drop.step = now_ms + 2000;
      if (p.option1) drop.col = 0;
    }

    if (drop.step == 0) {
      int speed = p.speed ? p.speed : random8(1, 255);
      speed = static_cast<int>(map_range(speed, 1, 255, 5000, 250));
      drop.speed = static_cast<float>(height * kFrameTimeMs) / static_cast<float>(speed);
      drop.pos = height;
      if (!p.option1) drop.col = static_cast<uint8_t>(random8(0, 15) << 4);
      drop.step = 1;
      drop.brick = static_cast<uint16_t>((p.intensity ? (p.intensity >> 5) + 1 : random8(1, 5)) * (1 + (height >> 6)));
    }

    if (drop.step == 1) {
      if (random8() >> 6) drop.step = 2;
    }

    if (drop.step == 2) {
      if (drop.pos > drop.stack) {
        drop.pos -= drop.speed;
        if (static_cast<int>(drop.pos) < static_cast<int>(drop.stack)) drop.pos = drop.stack;
        for (int y = static_cast<int>(drop.pos); y < height; y++) {
          Rgb c = y < static_cast<int>(drop.pos) + drop.brick
                      ? color_from_palette(p.palette_id, drop.col, p.primary, p.secondary, p.tertiary)
                      : p.secondary;
          frame[y][col] = c;
        }
      } else {
        drop.step = 0;
        drop.stack = static_cast<uint16_t>(drop.stack + drop.brick);
        if (drop.stack >= height) drop.step = now_ms + 2000;
      }
    }

    if (drop.step > 2) {
      drop.brick = 0;
      if (drop.step > now_ms) {
        for (int y = 0; y < height; y++) frame[y][col] = blend(frame[y][col], p.secondary, 25);
      } else {
        drop.stack = 0;
        drop.step = 0;
        if (p.option1) drop.col = static_cast<uint8_t>(drop.col + 8);
      }
    }
  }
}
EFFECTS_REGISTER(Id::kTetrix, mode_tetrix)

// wled00/FX.cpp:1366 mode_fire_flicker()
void mode_fire_flicker(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  uint32_t cycle_time = 40 + (255 - p.speed);
  uint32_t it = now_ms / cycle_time;
  if (state.step == it) return;

  uint8_t r = p.primary.r, g = p.primary.g, b = p.primary.b;
  uint8_t lum = (p.palette_id == 0) ? std::max(r, std::max(g, b)) : 255;
  lum = static_cast<uint8_t>(lum / (((256 - p.intensity) / 16) + 1));

  for (int x = 0; x < width; x++) {
    uint8_t flicker = random8(lum);
    Rgb col;
    if (p.palette_id == 0) {
      col = Rgb{static_cast<uint8_t>(std::max(r - flicker, 0)), static_cast<uint8_t>(std::max(g - flicker, 0)),
                static_cast<uint8_t>(std::max(b - flicker, 0))};
    } else {
      col = scale_rgb(color_from_palette(p.palette_id, palette_index_mapped(x, width), p.primary, p.secondary, p.tertiary),
                       static_cast<uint8_t>(255 - flicker));
    }
    fill_column(frame, x, col);
  }
  state.step = it;
}
EFFECTS_REGISTER(Id::kFireFlicker, mode_fire_flicker)

// wled00/FX.cpp:1394 gradient_base(), shared by mode_gradient()/
// mode_loading(). Upstream's `int brd = 1 + loading ? intensity/2 :
// intensity/4;` is an operator-precedence bug (`+` binds tighter than
// `?:`, so it's really `(1+loading) ? intensity/2 : intensity/4`, and
// since `loading` is a bool the condition is always true) - both modes
// really use intensity/2, reproduced as one expression rather than the
// two-branch appearance of the original.
void gradient_base(bool loading, uint32_t now_ms, const Params &p, State &state, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  uint16_t counter = static_cast<uint16_t>(now_ms * ((p.speed >> 2) + 1));
  uint16_t pp = static_cast<uint16_t>((static_cast<uint32_t>(counter) * width) >> 16);
  if (state.call == 0) pp = 0;
  int brd = 1 + p.intensity / 2;
  int p1 = pp - width;
  int p2 = pp + width;

  for (int i = 0; i < width; i++) {
    int val;
    if (loading) {
      val = std::abs(((i > pp) ? p2 : pp) - i);
    } else {
      val = std::min(std::abs(pp - i), std::min(std::abs(p1 - i), std::abs(p2 - i)));
    }
    val = (brd > val) ? (val * 255) / brd : 255;
    Rgb palette_color = color_from_palette(p.palette_id, palette_index_mapped(i, width), p.primary, p.secondary, p.tertiary);
    fill_column(frame, i, blend(p.primary, palette_color, static_cast<uint8_t>(val)));
  }
}

// wled00/FX.cpp:1420 mode_gradient()
void mode_gradient(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  gradient_base(false, now_ms, p, state, frame);
}
EFFECTS_REGISTER(Id::kGradient, mode_gradient)

// wled00/FX.cpp:1429 mode_loading()
void mode_loading(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  gradient_base(true, now_ms, p, state, frame);
}
EFFECTS_REGISTER(Id::kLoading, mode_loading)

// wled00/FX.cpp:3052 mode_rolling_balls() (the classic physics version,
// gated behind WLED_PS_DONT_REPLACE_1D_FX upstream rather than the
// Particle System rewrite - no ParticleSystem class involved).
struct RollingBall {
  uint32_t last_bounce_update;
  float mass;
  float velocity;
  float height;
};
constexpr int kMaxRollingBalls = 16;
struct RollingBallsState {
  RollingBall balls[kMaxRollingBalls];
};
static_assert(sizeof(RollingBallsState) <= State::kDataSize, "RollingBallsState too big for State::data");

void mode_rolling_balls(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  auto &s = *reinterpret_cast<RollingBallsState *>(state.data);
  unsigned num_balls = p.intensity / 16 + 1;
  bool has_col2 = !is_black(p.tertiary);
  Rgb bg = has_col2 ? Rgb{0, 0, 0} : p.secondary;

  if (state.call == 0) {
    for (auto &ball : s.balls) {
      ball.last_bounce_update = now_ms;
      ball.velocity = 20.0f * static_cast<float>(random16(1000, 10000)) / 10000.0f;
      if (random8() < 128) ball.velocity = -ball.velocity;
      ball.height = static_cast<float>(random16(0, 10000)) / 10000.0f;
      ball.mass = static_cast<float>(random16(1000, 10000)) / 10000.0f;
    }
    for (int x = 0; x < width; x++) fill_column(frame, x, bg);
  }

  float cfac = static_cast<float>(scale8(8, static_cast<uint8_t>(255 - p.speed)) + 1) * 20000.0f;

  if (p.option3) {
    fade_out_frame(frame, Rgb{0, 0, 0}, 250);
  } else if (!p.option2) {
    for (int x = 0; x < width; x++) fill_column(frame, x, bg);
  }

  for (unsigned i = 0; i < num_balls; i++) {
    RollingBall &ball = s.balls[i];
    float since = static_cast<float>(now_ms - ball.last_bounce_update) / cfac;
    float this_height = ball.height + ball.velocity * since;
    if (this_height < -0.5f || this_height > 1.5f) {
      this_height = ball.height = static_cast<float>(random16(0, 10000)) / 10000.0f;
      ball.last_bounce_update = now_ms;
    }
    if ((this_height <= 0.0f && ball.velocity < 0.0f) || (this_height >= 1.0f && ball.velocity > 0.0f)) {
      ball.velocity = -ball.velocity;
      ball.last_bounce_update = now_ms;
      ball.height = this_height;
    }
    if (p.option1) {
      for (unsigned j = i + 1; j < num_balls; j++) {
        RollingBall &other = s.balls[j];
        if (other.velocity != ball.velocity) {
          float tcollided = (cfac * (ball.height - other.height) +
                              ball.velocity * static_cast<float>(other.last_bounce_update - ball.last_bounce_update)) /
                             (other.velocity - ball.velocity);
          if (tcollided > 2.0f && tcollided < static_cast<float>(now_ms - other.last_bounce_update)) {
            ball.height = ball.height +
                           ball.velocity * (tcollided + static_cast<float>(other.last_bounce_update - ball.last_bounce_update)) / cfac;
            other.height = ball.height;
            ball.last_bounce_update = static_cast<uint32_t>(tcollided + 0.5f) + other.last_bounce_update;
            other.last_bounce_update = ball.last_bounce_update;
            float vtmp = ball.velocity;
            ball.velocity = ((ball.mass - other.mass) * vtmp + 2.0f * other.mass * other.velocity) / (ball.mass + other.mass);
            other.velocity = ((other.mass - ball.mass) * other.velocity + 2.0f * ball.mass * vtmp) / (ball.mass + other.mass);
            this_height = ball.height + ball.velocity * static_cast<float>(now_ms - ball.last_bounce_update) / cfac;
          }
        }
      }
    }

    Rgb color = p.primary;
    if (p.palette_id) {
      color = color_from_palette(p.palette_id, static_cast<uint8_t>(i * 255 / num_balls), p.primary, p.secondary, p.tertiary);
    } else if (has_col2) {
      color = seg_color3(p, static_cast<int>(i));
    }

    if (this_height < 0.0f) this_height = 0.0f;
    if (this_height > 1.0f) this_height = 1.0f;
    int pos = static_cast<int>(std::lround(this_height * (width - 1)));
    fill_column(frame, pos, color);
    ball.last_bounce_update = now_ms;
    ball.height = this_height;
  }
}
EFFECTS_REGISTER(Id::kRollingballs, mode_rolling_balls)

// wled00/FX.cpp:1461 struct Flasher + mode_fairy()
struct Flasher {
  uint16_t state_start;
  uint8_t state_dur;
  bool state_on;
};
constexpr int kFairyMaxFlashers = GuDisplay::WIDTH / 1 + 1;
struct FairyState {
  Flasher flashers[kFairyMaxFlashers];
};
static_assert(sizeof(FairyState) <= State::kDataSize, "FairyState too big for State::data");
constexpr int kFlashersPerZone = 6;
constexpr int kMaxShimmer = 92;

void mode_fairy(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  uint16_t prng16 = 5100;  // + strip.getCurrSegmentId(), always 0 on this single-segment board
  for (int i = 0; i < width; i++) {
    prng16 = static_cast<uint16_t>(prng16 * 2053) + 1384;
    fill_column(frame, i, color_from_palette(p.palette_id, static_cast<uint8_t>(prng16 >> 8), p.primary, p.secondary, p.tertiary));
  }

  if (p.intensity == 0) return;
  unsigned flasher_distance = ((255 - p.intensity) / 28) + 1;
  unsigned num_flashers = (width / flasher_distance) + 1;
  if (num_flashers > static_cast<unsigned>(kFairyMaxFlashers)) num_flashers = kFairyMaxFlashers;

  auto &s = *reinterpret_cast<FairyState *>(state.data);
  Flasher *flashers = s.flashers;
  uint16_t now16 = static_cast<uint16_t>(now_ms & 0xFFFFu);

  unsigned zones = num_flashers / kFlashersPerZone;
  if (!zones) zones = 1;
  unsigned flashers_in_zone = num_flashers / zones;
  uint8_t flasher_bri[kFlashersPerZone * 2 - 1];

  for (unsigned z = 0; z < zones; z++) {
    unsigned flasher_bri_sum = 0;
    unsigned first_flasher = z * flashers_in_zone;
    if (z == zones - 1) flashers_in_zone = num_flashers - (flashers_in_zone * (zones - 1));

    for (unsigned f = first_flasher; f < first_flasher + flashers_in_zone; f++) {
      Flasher &fl = flashers[f];
      uint16_t state_time = static_cast<uint16_t>(now16 - fl.state_start);
      if (state_time > fl.state_dur * 10) {
        fl.state_on = !fl.state_on;
        if (fl.state_on) {
          fl.state_dur = static_cast<uint8_t>(12 + random8(12 + ((255 - p.speed) >> 2)));
        } else {
          fl.state_dur = static_cast<uint8_t>(20 + random8(6 + ((255 - p.speed) >> 2)));
        }
        fl.state_start = now16;
        if (state_time < 255) {
          fl.state_start = static_cast<uint16_t>(fl.state_start - (255 - state_time));
          fl.state_dur = static_cast<uint8_t>(fl.state_dur + 26 - state_time / 10);
          state_time = static_cast<uint16_t>(255 - state_time);
        } else {
          state_time = 0;
        }
      }
      if (state_time > 255) state_time = 255;
      flasher_bri[f - first_flasher] = fl.state_on ? static_cast<uint8_t>(state_time) : static_cast<uint8_t>(255 - state_time);
      flasher_bri_sum += flasher_bri[f - first_flasher];
    }
    unsigned avg_flasher_bri = flasher_bri_sum / flashers_in_zone;
    unsigned global_peak_bri = 255 - ((avg_flasher_bri * kMaxShimmer) >> 8);

    for (unsigned f = first_flasher; f < first_flasher + flashers_in_zone; f++) {
      uint8_t bri = static_cast<uint8_t>((flasher_bri[f - first_flasher] * global_peak_bri) / 255);
      prng16 = static_cast<uint16_t>(prng16 * 2053) + 1384;
      unsigned flasher_pos = f * flasher_distance;
      if (flasher_pos < static_cast<unsigned>(width)) {
        Rgb c = color_from_palette(p.palette_id, static_cast<uint8_t>(prng16 >> 8), p.primary, p.secondary, p.tertiary);
        fill_column(frame, flasher_pos, blend(p.secondary, c, bri));
      }
      for (unsigned i = flasher_pos + 1; i < flasher_pos + flasher_distance && i < static_cast<unsigned>(width); i++) {
        prng16 = static_cast<uint16_t>(prng16 * 2053) + 1384;
        Rgb c = scale_rgb(color_from_palette(p.palette_id, static_cast<uint8_t>(prng16 >> 8), p.primary, p.secondary, p.tertiary),
                           static_cast<uint8_t>(global_peak_bri));
        fill_column(frame, i, c);
      }
    }
  }
}
EFFECTS_REGISTER(Id::kFairy, mode_fairy)

// wled00/FX.cpp:1437 mode_two_dots()
void mode_two_dots(uint32_t now_ms, const Params &p, State &, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  unsigned delay = 1 + (kFrameTimeMs << 3) / width;
  uint32_t it = now_ms / static_cast<uint32_t>(map_range(p.speed, 0, 255, delay << 4, delay));
  unsigned offset = it % width;
  unsigned dot_width = (static_cast<unsigned>(width) * (p.intensity + 1)) >> 9;
  if (!dot_width) dot_width = 1;
  if (!p.option2) {
    for (int x = 0; x < width; x++) fill_column(frame, x, p.tertiary);
  }
  Rgb color1 = p.primary;
  Rgb color2 = rgb_eq(p.secondary, p.tertiary) ? color1 : p.secondary;
  for (unsigned i = 0; i < dot_width; i++) {
    unsigned index_r = (offset + i) % width;
    unsigned index_b = (offset + i + (width >> 1)) % width;
    fill_column(frame, index_r, color1);
    fill_column(frame, index_b, color2);
  }
}
EFFECTS_REGISTER(Id::kTwoDots, mode_two_dots)

// wled00/FX.cpp:1547 mode_fairytwinkle()
struct FairytwinkleState {
  Flasher flashers[GuDisplay::WIDTH];
};
static_assert(sizeof(FairytwinkleState) <= State::kDataSize, "FairytwinkleState too big for State::data");

void mode_fairytwinkle(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  auto &s = *reinterpret_cast<FairytwinkleState *>(state.data);
  Flasher *flashers = s.flashers;
  uint16_t now16 = static_cast<uint16_t>(now_ms & 0xFFFFu);
  uint16_t prng16 = 5100;

  unsigned rise_fall_time = 400 + (255 - p.speed) * 3;
  unsigned max_dur = rise_fall_time / 100 + ((255 - p.intensity) >> 2) + 13 + ((255 - p.intensity) >> 1);

  for (int f = 0; f < width; f++) {
    Flasher &fl = flashers[f];
    uint16_t state_time = static_cast<uint16_t>(now16 - fl.state_start);
    if (state_time > fl.state_dur * 100) {
      fl.state_on = !fl.state_on;
      bool init = !fl.state_dur;
      if (fl.state_on) {
        fl.state_dur = static_cast<uint8_t>(rise_fall_time / 100 + ((255 - p.intensity) >> 2) +
                                             random8(12 + ((255 - p.intensity) >> 1)) + 1);
      } else {
        fl.state_dur = static_cast<uint8_t>(rise_fall_time / 100 + random8(3 + ((255 - p.speed) >> 6)) + 1);
      }
      fl.state_start = now16;
      state_time = 0;
      if (init) {
        fl.state_start = static_cast<uint16_t>(fl.state_start - rise_fall_time);
        fl.state_dur = static_cast<uint8_t>(rise_fall_time / 100 + random8(12 + ((255 - p.intensity) >> 1)) + 5);
        state_time = static_cast<uint16_t>(rise_fall_time);
      }
    }
    if (fl.state_on && fl.state_dur > max_dur) fl.state_dur = static_cast<uint8_t>(max_dur);
    if (state_time > rise_fall_time) state_time = static_cast<uint16_t>(rise_fall_time);
    unsigned fadeprog = 255 - ((state_time * 255) / rise_fall_time);
    uint8_t flasher_bri = fl.state_on ? static_cast<uint8_t>(255 - gamma8_approx(static_cast<uint8_t>(fadeprog)))
                                       : gamma8_approx(static_cast<uint8_t>(fadeprog));
    uint16_t last_r = prng16;
    unsigned diff = 0;
    while (diff < 0x4000) {
      prng16 = static_cast<uint16_t>(prng16 * 2053) + 1384;
      diff = (prng16 > last_r) ? prng16 - last_r : last_r - prng16;
    }
    Rgb c = color_from_palette(p.palette_id, static_cast<uint8_t>(prng16 >> 8), p.primary, p.secondary, p.tertiary);
    fill_column(frame, f, blend(p.secondary, c, flasher_bri));
  }
}
EFFECTS_REGISTER(Id::kFairytwinkle, mode_fairytwinkle)

// wled00/FX.cpp:594 running_base(saw=false, dual=true), as used by
// wled00/FX.cpp:627 mode_running_dual()
void mode_running_dual(uint32_t now_ms, const Params &p, State &, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  unsigned x_scale = p.intensity >> 2;
  uint32_t counter = (now_ms * p.speed) >> 9;

  for (int i = 0; i < width; i++) {
    unsigned a = static_cast<unsigned>(i * x_scale - counter);
    uint8_t s = sin_gap(static_cast<uint16_t>(a));
    Rgb ca = blend(p.secondary, color_from_palette(p.palette_id, palette_index_mapped(i, width), p.primary, p.secondary, p.tertiary), s);

    unsigned b = static_cast<unsigned>((width - 1 - i) * x_scale - counter);
    uint8_t t = sin_gap(static_cast<uint16_t>(b));
    Rgb cb = blend(p.secondary, color_from_palette(p.palette_id, palette_index_mapped(i, width), p.primary, p.secondary, p.tertiary), t);

    fill_column(frame, i, blend(ca, cb, 127));
  }
}
EFFECTS_REGISTER(Id::kRunningDual, mode_running_dual)

// wled00/FX.cpp:4642 mode_image() - real WLED displays an uploaded GIF via
// WLED_ENABLE_GIF (filesystem-backed storage + a GIF decoder), which this
// firmware doesn't have. Instead of porting a GIF decoder onto an RP2040,
// image_data.h pushes the decode work onto the browser (see its own top
// comment) - including animation, decoded frame-by-frame there - and hands
// back a plain 32x32 RGB buffer per frame; display that if one's been
// uploaded, otherwise fall back to Solid, matching upstream's own
// FX_FALLBACK_STATIC behavior when WLED_ENABLE_GIF isn't compiled in.
void mode_image(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  update_animation(now_ms, state.call == 0);
  if (!has_image()) {
    for (int x = 0; x < GuDisplay::WIDTH; x++) fill_column(frame, x, p.primary);
    return;
  }
  for (int y = 0; y < GuDisplay::HEIGHT; y++) {
    for (int x = 0; x < GuDisplay::WIDTH; x++) frame[y][x] = image_pixel(x, y);
  }
}
EFFECTS_REGISTER(Id::kImage, mode_image)

// wled00/FX.cpp:1595 tricolor_chase(), as used by wled00/FX.cpp:1616
// mode_tricolor_chase() (color1=SEGCOLOR(2), color2=SEGCOLOR(0))
void mode_tricolor_chase(uint32_t now_ms, const Params &p, State &, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  uint32_t cycle_time = 50 + ((255 - p.speed) << 1);
  uint32_t it = now_ms / cycle_time;
  unsigned chase_width = 1 + (p.intensity >> 4);
  unsigned index = it % (chase_width * 3);

  for (int i = 0; i < width; i++, index++) {
    if (index > chase_width * 3 - 1) index = 0;
    Rgb color = p.tertiary;
    if (index > (chase_width << 1) - 1) color = color_from_palette(p.palette_id, palette_index_mapped(i, width), p.primary, p.secondary, p.tertiary);
    else if (index > chase_width - 1) color = p.primary;
    fill_column(frame, width - i - 1, color);
  }
}
EFFECTS_REGISTER(Id::kTricolorChase, mode_tricolor_chase)

// wled00/FX.cpp:1697 mode_tricolor_wipe()
void mode_tricolor_wipe(uint32_t now_ms, const Params &p, State &, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  uint32_t cycle_time = 1000 + (255 - p.speed) * 200;
  uint32_t perc = now_ms % cycle_time;
  unsigned prog = (perc * 65535) / cycle_time;
  unsigned led_index = (prog * width * 3) >> 16;
  unsigned led_offset = led_index;

  for (int i = 0; i < width; i++)
    fill_column(frame, i, color_from_palette(p.palette_id, palette_index_mapped(i, width), p.primary, p.secondary, p.tertiary));

  if (led_index < static_cast<unsigned>(width)) {
    for (int i = 0; i < width; i++) fill_column(frame, i, (static_cast<unsigned>(i) > led_offset) ? p.primary : p.secondary);
  } else if (led_index < static_cast<unsigned>(width) * 2) {
    led_offset = led_index - width;
    for (unsigned i = led_offset + 1; i < static_cast<unsigned>(width); i++) fill_column(frame, i, p.secondary);
  } else {
    led_offset = led_index - static_cast<unsigned>(width) * 2;
    for (unsigned i = 0; i <= led_offset && i < static_cast<unsigned>(width); i++) fill_column(frame, i, p.primary);
  }
}
EFFECTS_REGISTER(Id::kTricolorWipe, mode_tricolor_wipe)

// wled00/FX.cpp:1737 mode_tricolor_fade()
void mode_tricolor_fade(uint32_t now_ms, const Params &p, State &, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  uint16_t counter = static_cast<uint16_t>(now_ms * ((p.speed >> 3) + 1));
  uint32_t prog = (static_cast<uint32_t>(counter) * 768) >> 16;

  Rgb color1, color2;
  int stage;
  if (prog < 256) {
    color1 = p.primary;
    color2 = p.secondary;
    stage = 0;
  } else if (prog < 512) {
    color1 = p.secondary;
    color2 = p.tertiary;
    stage = 1;
  } else {
    color1 = p.tertiary;
    color2 = p.primary;
    stage = 2;
  }

  uint8_t stp = static_cast<uint8_t>(prog);
  for (int i = 0; i < width; i++) {
    Rgb color;
    Rgb palette_color = color_from_palette(p.palette_id, palette_index_mapped(i, width), p.primary, p.secondary, p.tertiary);
    if (stage == 2) color = blend(palette_color, color2, stp);
    else if (stage == 1) color = blend(color1, palette_color, stp);
    else color = blend(color1, color2, stp);
    fill_column(frame, i, color);
  }
}
EFFECTS_REGISTER(Id::kTricolorFade, mode_tricolor_fade)

// wled00/FX.cpp:1906 mode_lightning()
void mode_lightning(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  unsigned led_start = random16(width);
  unsigned led_len = 1 + random16(width - led_start);
  uint8_t bri = static_cast<uint8_t>(255 / random8(1, 3));

  if (state.aux1 == 0) {
    state.aux1 = static_cast<uint16_t>(random8(4, 4 + p.intensity / 20));
    state.aux1 = static_cast<uint16_t>(state.aux1 * 2);
    bri = 52;
    state.aux0 = 200;
  }

  if (!p.option2) {
    for (int x = 0; x < width; x++) fill_column(frame, x, p.secondary);
  }

  if (state.aux1 > 3 && !(state.aux1 & 0x01)) {
    for (unsigned i = led_start; i < led_start + led_len && i < static_cast<unsigned>(width); i++) {
      fill_column(frame, i,
                   scale_rgb(color_from_palette(p.palette_id, palette_index_mapped(i, width), p.primary, p.secondary, p.tertiary), bri));
    }
    state.aux1--;
    state.step = now_ms;
  } else {
    if (now_ms - state.step > state.aux0) {
      state.aux1--;
      if (state.aux1 < 2) state.aux1 = 0;
      state.aux0 = static_cast<uint16_t>(50 + random8(100));
      if (state.aux1 == 2) state.aux0 = static_cast<uint16_t>(random8(255 - p.speed) * 100);
      state.step = now_ms;
    }
  }
}
EFFECTS_REGISTER(Id::kLightning, mode_lightning)

}  // namespace
}  // namespace effects
