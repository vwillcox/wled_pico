#include <cstdlib>

#include "effects.h"
#include "wled_compat.h"
#include "palettes.h"
#include "display/gu_display.h"

namespace effects {
namespace {

// ---------------------------------------------------------------------------
// Shared helpers this batch needs that wled_compat.h doesn't provide yet
// (kept file-local rather than touching that shared header - see batch 8's
// constraints). Several of the real WLED mode_ functions below call these.
// ---------------------------------------------------------------------------

constexpr int kW = GuDisplay::WIDTH;
constexpr int kH = GuDisplay::HEIGHT;

inline Rgb scale_rgb(Rgb c, uint8_t amount) {
  return Rgb{scale8(c.r, amount), scale8(c.g, amount), scale8(c.b, amount)};
}
// WLED's color_add(c1, c2, preserveCR): saturating per-channel add. We drop
// the preserveCR ratio-preserving overflow mode (wled00/colors.cpp:37) and
// just saturate each channel independently - a visually tiny difference
// only on already-overflowing (very bright) adds.
inline Rgb add_rgb(Rgb a, Rgb b) { return Rgb{qadd8(a.r, b.r), qadd8(a.g, b.g), qadd8(a.b, b.b)}; }
inline bool rgb_eq(Rgb a, Rgb b) { return a.r == b.r && a.g == b.g && a.b == b.b; }

inline void set_xy(Frame frame, int x, int y, Rgb c) {
  if (x < 0 || x >= kW || y < 0 || y >= kH) return;
  frame[y][x] = c;
}
inline void add_xy(Frame frame, int x, int y, Rgb c) {
  if (x < 0 || x >= kW || y < 0 || y >= kH) return;
  frame[y][x] = add_rgb(frame[y][x], c);
}

// Arduino's map(): linear remap, no clamping - used all over FX.cpp.
inline long map_val(long x, long in_min, long in_max, long out_min, long out_max) {
  return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

// wled00/FX_fcn.cpp:1108 Segment::blur() + wled00/FX_2Dfcn.cpp:246 blur2D(),
// merged into one function since every mode_ in this batch that blurs uses
// symmetrical blur_x==blur_y (SEGMENT.blur(amount) form only). Row pass then
// column pass, each a one-pixel-lookback box blur ("keep" + "seep" split).
void blur2d(Frame frame, uint8_t blur_amount, bool smear = false) {
  if (blur_amount == 0) return;
  const uint8_t keep = smear ? 255 : static_cast<uint8_t>(255 - blur_amount);
  const uint8_t seep = blur_amount >> 1;
  for (int y = 0; y < kH; y++) {
    Rgb carry = scale_rgb(frame[y][0], seep);
    frame[y][0] = scale_rgb(frame[y][0], keep);
    for (int x = 1; x < kW; x++) {
      Rgb cur = frame[y][x];
      Rgb part = scale_rgb(cur, seep);
      Rgb kept = scale_rgb(cur, keep);
      frame[y][x - 1] = add_rgb(frame[y][x - 1], part);
      frame[y][x] = add_rgb(kept, carry);
      carry = part;
    }
  }
  for (int x = 0; x < kW; x++) {
    Rgb carry = scale_rgb(frame[0][x], seep);
    frame[0][x] = scale_rgb(frame[0][x], keep);
    for (int y = 1; y < kH; y++) {
      Rgb cur = frame[y][x];
      Rgb part = scale_rgb(cur, seep);
      Rgb kept = scale_rgb(cur, keep);
      frame[y - 1][x] = add_rgb(frame[y - 1][x], part);
      frame[y][x] = add_rgb(kept, carry);
      carry = part;
    }
  }
}

// FastLED's beat8(bpm): a plain 0-255 sawtooth completing `bpm` cycles/min.
// Not bit-exact against FastLED's phase accumulator (same policy wled_compat.h
// documents for its own sin8/beatsin8) - matches the rate and shape.
uint8_t beat8_saw(uint32_t now_ms, uint8_t bpm) {
  if (bpm == 0) return 0;
  uint32_t period_ms = 60000u / bpm;
  return static_cast<uint8_t>(((now_ms % period_ms) * 256u) / period_ms);
}

// wled_compat.h's beatsin16() takes unsigned low/high, so it can't take
// mode_2Dtartan's beatsin16_t(bpm, -360, 360) directly. This is the signed
// version, built on the sin16() this file already has - only needed here.
int32_t beatsin16_signed(uint32_t now_ms, uint8_t bpm, int32_t low, int32_t high) {
  if (bpm == 0) return (low + high) / 2;
  uint32_t period_ms = 60000u / bpm;
  uint16_t theta = static_cast<uint16_t>((static_cast<uint64_t>(now_ms % period_ms) << 16) / period_ms);
  int32_t s = sin16(theta);
  int32_t mid = (low + high) / 2;
  int32_t amp = (high - low) / 2;
  return mid + (s * amp) / 32767;
}

// wled00/util.cpp:1132-1298 perlin16()/perlin8() (by @dedehai) - fixed-point
// Perlin noise standing in for FastLED's inoise16()/inoise8(). Only the 2D
// (perlin16) and 3D (perlin8) variants this batch's effects call are ported;
// the 1D variants real WLED also has are left out (nothing here uses them).
constexpr int32_t kPerlinShift = 1;

inline int32_t hash_to_gradient(uint32_t h) { return static_cast<int32_t>(h & 0x03u) - 2; }

inline int32_t gradient2d(uint32_t x0, int32_t dx, uint32_t y0, int32_t dy) {
  uint32_t h = (x0 * 0x27D4EB2Du) ^ (y0 * 0xB5297A4Du);
  h ^= h >> 15;
  h *= 0x92C3412Bu;
  h ^= h >> 13;
  return (hash_to_gradient(h) * dx + hash_to_gradient(h >> kPerlinShift) * dy) >> (1 + kPerlinShift);
}

inline int32_t gradient3d(uint32_t x0, int32_t dx, uint32_t y0, int32_t dy, uint32_t z0, int32_t dz) {
  uint32_t h = (x0 * 0x27D4EB2Du) ^ (y0 * 0xB5297A4Du) ^ (z0 * 0x1B56C4E9u);
  h ^= h >> 15;
  h *= 0x92C3412Bu;
  h ^= h >> 13;
  return ((hash_to_gradient(h) * dx + hash_to_gradient(h >> (1 + kPerlinShift)) * dy +
           hash_to_gradient(h >> (1 + 2 * kPerlinShift)) * dz) *
           85) >>
         (8 + kPerlinShift);
}

inline uint32_t perlin_smoothstep(uint32_t t) {
  uint32_t t2 = (t * t) >> 16;
  uint32_t factor = (3u << 16) - (t << 1);
  return (t2 * factor) >> 18;
}

inline int32_t perlin_lerp(int32_t a, int32_t b, int32_t t) { return a + (((b - a) * t) >> 14); }

int32_t perlin2d_raw(uint32_t x, uint32_t y) {
  int32_t x0 = static_cast<int32_t>(x >> 16), y0 = static_cast<int32_t>(y >> 16);
  int32_t x1 = x0 + 1, y1 = y0 + 1;
  int32_t dx0 = static_cast<int32_t>(x & 0xFFFFu), dy0 = static_cast<int32_t>(y & 0xFFFFu);
  int32_t dx1 = dx0 - 0x10000, dy1 = dy0 - 0x10000;
  int32_t g00 = gradient2d(x0, dx0, y0, dy0);
  int32_t g10 = gradient2d(x1, dx1, y0, dy0);
  int32_t g01 = gradient2d(x0, dx0, y1, dy1);
  int32_t g11 = gradient2d(x1, dx1, y1, dy1);
  int32_t tx = static_cast<int32_t>(perlin_smoothstep(static_cast<uint32_t>(dx0)));
  int32_t ty = static_cast<int32_t>(perlin_smoothstep(static_cast<uint32_t>(dy0)));
  int32_t nx0 = perlin_lerp(g00, g10, tx);
  int32_t nx1 = perlin_lerp(g01, g11, tx);
  return perlin_lerp(nx0, nx1, ty);
}

int32_t perlin3d_raw(uint32_t x, uint32_t y, uint32_t z, bool is16bit) {
  int32_t x0 = static_cast<int32_t>(x >> 16), y0 = static_cast<int32_t>(y >> 16), z0 = static_cast<int32_t>(z >> 16);
  int32_t x1 = x0 + 1, y1 = y0 + 1, z1 = z0 + 1;
  if (is16bit) {
    x1 &= 0xFF;
    y1 &= 0xFF;
    z1 &= 0xFF;
  }
  int32_t dx0 = static_cast<int32_t>(x & 0xFFFFu), dy0 = static_cast<int32_t>(y & 0xFFFFu),
          dz0 = static_cast<int32_t>(z & 0xFFFFu);
  int32_t dx1 = dx0 - 0x10000, dy1 = dy0 - 0x10000, dz1 = dz0 - 0x10000;
  int32_t g000 = gradient3d(x0, dx0, y0, dy0, z0, dz0), g001 = gradient3d(x0, dx0, y0, dy0, z1, dz1);
  int32_t g010 = gradient3d(x0, dx0, y1, dy1, z0, dz0), g011 = gradient3d(x0, dx0, y1, dy1, z1, dz1);
  int32_t g100 = gradient3d(x1, dx1, y0, dy0, z0, dz0), g101 = gradient3d(x1, dx1, y0, dy0, z1, dz1);
  int32_t g110 = gradient3d(x1, dx1, y1, dy1, z0, dz0), g111 = gradient3d(x1, dx1, y1, dy1, z1, dz1);
  int32_t tx = static_cast<int32_t>(perlin_smoothstep(static_cast<uint32_t>(dx0)));
  int32_t ty = static_cast<int32_t>(perlin_smoothstep(static_cast<uint32_t>(dy0)));
  int32_t tz = static_cast<int32_t>(perlin_smoothstep(static_cast<uint32_t>(dz0)));
  int32_t nx0 = perlin_lerp(g000, g100, tx);
  int32_t nx1 = perlin_lerp(g010, g110, tx);
  int32_t nx2 = perlin_lerp(g001, g101, tx);
  int32_t nx3 = perlin_lerp(g011, g111, tx);
  int32_t ny0 = perlin_lerp(nx0, nx1, ty);
  int32_t ny1 = perlin_lerp(nx2, nx3, ty);
  return perlin_lerp(ny0, ny1, tz);
}

uint16_t perlin16(uint32_t x, uint32_t y) {
  return static_cast<uint16_t>(((perlin2d_raw(x, y) * 1537) >> 10) + 32725);
}

uint8_t perlin8(uint16_t x, uint16_t y, uint16_t z) {
  int32_t raw = perlin3d_raw(static_cast<uint32_t>(x) << 8, static_cast<uint32_t>(y) << 8,
                              static_cast<uint32_t>(z) << 8, true);
  return static_cast<uint8_t>((((raw * 2015) >> 10) + 33168) >> 8);
}

// ---------------------------------------------------------------------------
// wled00/FX.cpp:5390 mode_2Dgameoflife()
// ---------------------------------------------------------------------------
// The real WLED Cell also carries an `edgeCell` flag so most cells can skip
// wrapping their neighbor coordinates (a perf-only optimization). We always
// wrap via modulo instead - identical output for interior cells (wrap is a
// no-op when already in range) and correct for edge cells, just simpler.
struct GolCell {
  uint8_t alive : 1;
  uint8_t faded : 1;
  uint8_t toggle_status : 1;
  uint8_t oscillator_check : 1;
  uint8_t spaceship_check : 1;
};
struct GameOfLifeState {
  GolCell cells[kW * kH];
};
static_assert(sizeof(GameOfLifeState) <= State::kDataSize, "GameOfLifeState too big for State::data");

// Several color_from_palette() calls in this batch's source pass a 4th
// ("mcol") argument that only matters when the segment's palette is 0 - and
// even then, real WLED's Segment::color_from_palette() collapses any mcol
// >= NUM_COLORS(3) to the same getCurrentColor(0) as mcol==0 (see
// wled00/FX.h:652's `i<NUM_COLORS?i:0`), so values like 255 seen below are
// inert upstream too. This port always resolves through our engine's own
// primary/secondary/tertiary handling and drops that argument everywhere.
void mode_2d_game_of_life(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  constexpr unsigned kMaxIndex = kW * kH;
  auto &s = *reinterpret_cast<GameOfLifeState *>(state.data);
  GolCell *cells = s.cells;

  uint16_t &generation = state.aux0;
  uint16_t &glider_length = state.aux1;
  bool mutate = p.option3;
  uint8_t blur_amt = static_cast<uint8_t>(map_val(p.custom1, 0, 255, 255, 4));

  Rgb bg = p.secondary;
  Rgb birth_color = color_from_palette(p.palette_id, 128, p.primary, p.secondary, p.tertiary);

  bool setup = state.call == 0;
  if (setup) {
    unsigned a = kH, b = kW;
    while (b) {
      unsigned t = b;
      b = a % b;
      a = t;
    }
    glider_length = static_cast<uint16_t>((kW * kH / a) << 2);
  }

  uint32_t diff = (now_ms > state.step) ? (now_ms - state.step) : (state.step - now_ms);
  if (diff > 2000) state.step = 0;
  bool paused = state.step > now_ms;

  if ((!paused && generation == 0) || setup) {
    state.step = now_ms + 1280;
    generation = 1;
    paused = true;
    for (unsigned i = 0; i < kMaxIndex; i++) cells[i] = GolCell{};
    for (unsigned i = 0; i < kMaxIndex; i++) {
      bool is_alive = (random8(3) == 0);
      cells[i].alive = is_alive;
      cells[i].faded = !is_alive;
      unsigned x = i % kW, y = i / kW;
      Rgb c = is_alive ? color_from_palette(p.palette_id, random8(), p.primary, p.secondary, p.tertiary) : bg;
      frame[y][x] = c;
    }
  }

  uint32_t update_interval = 1000u / static_cast<uint32_t>(map_val(p.speed, 0, 255, 1, 42));
  if (paused || (now_ms - state.step < update_interval)) {
    for (unsigned i = kMaxIndex; i-- > 0;) {
      if (!cells[i].alive) {
        unsigned x = i % kW, y = i / kW;
        Rgb cur = frame[y][x];
        if (!rgb_eq(cur, bg)) {
          if (cells[i].faded) {
            frame[y][x] = bg;
          } else {
            Rgb blended = blend(cur, bg, 2);
            if (rgb_eq(blended, cur)) {
              blended = bg;
              cells[i].faded = 1;
            }
            frame[y][x] = blended;
          }
        }
      }
    }
    return;
  }

  bool update_oscillator = (generation % 16 == 0);
  bool update_spaceship = glider_length && (generation % glider_length == 0);
  bool repeating_oscillator = true, repeating_spaceship = true, empty_grid = true;

  for (int idx = static_cast<int>(kMaxIndex) - 1; idx >= 0; idx--) {
    unsigned cidx = static_cast<unsigned>(idx);
    GolCell &cell = cells[cidx];
    unsigned x = cidx % kW, y = cidx / kW;

    if (cell.alive) empty_grid = false;
    if (cell.oscillator_check != cell.alive) repeating_oscillator = false;
    if (cell.spaceship_check != cell.alive) repeating_spaceship = false;
    if (update_oscillator) cell.oscillator_check = cell.alive;
    if (update_spaceship) cell.spaceship_check = cell.alive;

    unsigned neighbors = 0, alive_parents = 0;
    unsigned parent_idx[3] = {0, 0, 0};
    for (int dy = -1; dy <= 1; dy++)
      for (int dx = -1; dx <= 1; dx++)
        if (dx || dy) {
          unsigned nx = (x + dx + kW) % kW;
          unsigned ny = (y + dy + kH) % kH;
          unsigned nidx = nx + ny * kW;
          GolCell &nb = cells[nidx];
          if (nb.alive) {
            neighbors++;
            if (!nb.toggle_status && neighbors < 4 && alive_parents < 3) parent_idx[alive_parents++] = nidx;
          }
        }

    Rgb new_color{};
    bool needs_color = false;

    if (cell.alive && (neighbors < 2 || neighbors > 3)) {
      cell.toggle_status = 1;
      if (blur_amt == 255) cell.faded = 1;
      new_color = cell.faded ? bg : blend(frame[y][x], bg, blur_amt);
      needs_color = true;
    } else if (!cell.alive) {
      uint8_t mutation_roll = mutate ? random8(128) : 1;
      if ((neighbors == 3 && mutation_roll) || (mutate && neighbors == 2 && !mutation_roll)) {
        cell.toggle_status = 1;
        cell.faded = 0;
        if (alive_parents) {
          unsigned pidx = parent_idx[random8(static_cast<uint8_t>(alive_parents))];
          new_color = frame[pidx / kW][pidx % kW];
        } else {
          new_color = birth_color;
        }
        needs_color = true;
      } else if (!cell.faded) {
        Rgb cur = frame[y][x];
        Rgb blended = blend(cur, bg, blur_amt);
        if (rgb_eq(blended, cur)) {
          blended = bg;
          cell.faded = 1;
        }
        new_color = blended;
        needs_color = true;
      }
    }

    if (needs_color) frame[y][x] = new_color;
  }

  for (unsigned i = 0; i < kMaxIndex; i++) {
    cells[i].alive ^= cells[i].toggle_status;
    cells[i].toggle_status = 0;
  }

  if (repeating_oscillator || repeating_spaceship || empty_grid) {
    generation = 0;
    state.step += 1024;
  } else {
    generation++;
    state.step = now_ms;
  }
}
EFFECTS_REGISTER(Id::k2dgameoflife, mode_2d_game_of_life)

// wled00/FX.cpp:6049 mode_2Dtartan()
void mode_2d_tartan(uint32_t now_ms, const Params &p, State &, Frame frame) {
  int offset_x = static_cast<int>(beatsin16_signed(now_ms, 3, -360, 360));
  int offset_y = static_cast<int>(beatsin16_signed(now_ms, 2, -360, 360));
  int sharpness = p.custom3 / 8;

  for (int x = 0; x < kW; x++) {
    for (int y = 0; y < kH; y++) {
      uint8_t hue = static_cast<uint8_t>(x * beatsin16(now_ms, 10, 1, 10) + offset_y);
      uint8_t bri = sin8(static_cast<uint8_t>(x * p.speed / 2 + offset_x));
      uint32_t band = bri;
      for (int i = 0; i < sharpness; i++) band *= bri;
      band >>= 8 * sharpness;
      frame[y][x] = scale_rgb(color_from_palette(p.palette_id, hue, p.primary, p.secondary, p.tertiary),
                               static_cast<uint8_t>(band));

      hue = static_cast<uint8_t>(y * 3 + offset_x);
      bri = sin8(static_cast<uint8_t>(y * p.intensity / 2 + offset_y));
      band = bri;
      for (int i = 0; i < sharpness; i++) band *= bri;
      band >>= 8 * sharpness;
      Rgb c2 = scale_rgb(color_from_palette(p.palette_id, hue, p.primary, p.secondary, p.tertiary),
                          static_cast<uint8_t>(band));
      frame[y][x] = add_rgb(frame[y][x], c2);
    }
  }
}
EFFECTS_REGISTER(Id::k2dtartan, mode_2d_tartan)

// wled00/FX.cpp:5891 mode_2DPolarLights(). Real WLED derives its "height
// falloff" scale from the segment's actual rows/cols via float map()+fabsf()
// since panels can be any size; this board is always a fixed 32x32, so that
// reduces to plain integer arithmetic - kept as such rather than pulling in
// floats for a computation that's exact in integers here anyway.
void mode_2d_polar_lights(uint32_t /*now_ms*/, const Params &p, State &state, Frame frame) {
  int adjust_height = static_cast<int>(map_val(kH, 8, 32, 28, 12));
  unsigned adj_scale = static_cast<unsigned>(map_val(kW, 8, 64, 310, 63));
  unsigned scale = static_cast<unsigned>(map_val(p.intensity, 0, 255, 30, adj_scale));
  int speed = static_cast<int>(map_val(p.speed, 0, 255, 128, 16));
  if (speed <= 0) speed = 1;

  for (int x = 0; x < kW; x++) {
    for (int y = 0; y < kH; y++) {
      state.step++;
      int height_diff = (kH / 2) - y;
      if (height_diff < 0) height_diff = -height_diff;
      uint8_t falloff = static_cast<uint8_t>(height_diff * adjust_height);
      uint8_t noise = perlin8(static_cast<uint16_t>((state.step % 2) + x * scale),
                               static_cast<uint16_t>(y * 16 + state.step % 16),
                               static_cast<uint16_t>(state.step / static_cast<uint32_t>(speed)));
      uint8_t palindex = qsub8(noise, falloff);
      uint8_t palbrightness = palindex;
      if (p.option1) palindex = 255 - palindex;
      frame[y][x] =
          scale_rgb(color_from_palette(p.palette_id, palindex, p.primary, p.secondary, p.tertiary), palbrightness);
    }
  }
}
EFFECTS_REGISTER(Id::k2dpolarlights, mode_2d_polar_lights)

// wled00/FX.cpp:5366 mode_2DFrizzles()
void mode_2d_frizzles(uint32_t now_ms, const Params &p, State &, Frame frame) {
  fade_to_black_by(frame, static_cast<uint8_t>(16 + (p.option1 ? 10 : 0)));
  for (int i = 8; i > 0; i--) {
    int x = beatsin8(now_ms, static_cast<uint8_t>(p.speed / 8 + i), 0, kW - 1);
    int y = beatsin8(now_ms, static_cast<uint8_t>(p.intensity / 8 - i), 0, kH - 1);
    Rgb c = color_from_palette(p.palette_id, beatsin8(now_ms, 12, 0, 255), p.primary, p.secondary, p.tertiary);
    add_xy(frame, x, y, c);
  }
  blur2d(frame, static_cast<uint8_t>(p.custom1 >> (3 + (p.option1 ? 1 : 0))), p.option1);
}
EFFECTS_REGISTER(Id::k2dfrizzles, mode_2d_frizzles)

// wled00/FX.cpp:5855 mode_2DPlasmaball()
void mode_2d_plasma_ball(uint32_t now_ms, const Params &p, State &, Frame frame) {
  fade_to_black_by(frame, static_cast<uint8_t>(p.custom1 >> 2));
  uint32_t t = (now_ms * 8) / (256u - p.speed);

  for (int i = 0; i < kW; i++) {
    unsigned val = perlin8(static_cast<uint16_t>(i * 30), static_cast<uint16_t>(t), static_cast<uint16_t>(t));
    unsigned outer_max = static_cast<unsigned>(map_val(val, 0, 255, 0, kW - 1));
    for (int j = 0; j < kH; j++) {
      unsigned val2 = perlin8(static_cast<uint16_t>(t), static_cast<uint16_t>(j * 30), static_cast<uint16_t>(t));
      unsigned inner_max = static_cast<unsigned>(map_val(val2, 0, 255, 0, kH - 1));
      int x = i + static_cast<int>(inner_max) - kW / 2;
      int y = j + static_cast<int>(outer_max) - kW / 2;
      int cx = i + static_cast<int>(inner_max);
      int cy = j + static_cast<int>(outer_max);
      bool on = ((x - y > -2) && (x - y < 2)) || ((kW - 1 - x - y > -2) && (kW - 1 - x - y < 2)) ||
                (kW - cx == 0) || (kW - 1 - cx == 0) || ((kH - cy == 0) || (kH - 1 - cy == 0));
      Rgb c = on ? scale_rgb(color_from_palette(p.palette_id, beat8_saw(now_ms, 5), p.primary, p.secondary,
                                                 p.tertiary),
                              static_cast<uint8_t>(val))
                 : Rgb{0, 0, 0};
      add_xy(frame, i, j, c);
    }
  }
  blur2d(frame, static_cast<uint8_t>(p.custom2 >> 5));
}
EFFECTS_REGISTER(Id::k2dplasmaball, mode_2d_plasma_ball)

// wled00/FX.cpp:5068 mode_FlowStripe()
void mode_flow_stripe(uint32_t now_ms, const Params &p, State &, Frame frame) {
  const int hl = kW * 10 / 13;
  uint8_t hue = static_cast<uint8_t>(now_ms / (p.speed + 1));
  uint32_t t = now_ms / (p.intensity / 8 + 1);

  for (int i = 0; i < kW; i++) {
    int c = (std::abs(i - hl) * 127) / hl;
    c = sin8(static_cast<uint8_t>(c));
    c = sin8(static_cast<uint8_t>(c / 2 + t));
    uint8_t b = sin8(static_cast<uint8_t>(c + t / 8));
    Rgb color = color_from_palette(p.palette_id, static_cast<uint8_t>(b + hue), p.primary, p.secondary, p.tertiary);
    fill_column(frame, i, color);
  }
}
EFFECTS_REGISTER(Id::kFlowstripe, mode_flow_stripe)

// wled00/FX.cpp:5546 mode_2DHiphotic()
void mode_2d_hiphotic(uint32_t now_ms, const Params &p, State &, Frame frame) {
  const uint32_t a = now_ms / ((p.custom3 >> 1) + 1);
  for (int x = 0; x < kW; x++) {
    for (int y = 0; y < kH; y++) {
      uint8_t v = sin8(static_cast<uint8_t>(cos8(static_cast<uint8_t>(x * p.speed / 16 + a / 3)) +
                                             sin8(static_cast<uint8_t>(y * p.intensity / 16 + a / 4)) + a));
      frame[y][x] = color_from_palette(p.palette_id, v, p.primary, p.secondary, p.tertiary);
    }
  }
}
EFFECTS_REGISTER(Id::k2dhiphotic, mode_2d_hiphotic)

// wled00/FX.cpp:5943 mode_2DSindots()
void mode_2d_sindots(uint32_t now_ms, const Params &p, State &, Frame frame) {
  fade_to_black_by(frame, static_cast<uint8_t>((p.custom1 >> 3) + (p.option1 ? 24 : 0)));

  uint8_t t1 = static_cast<uint8_t>(now_ms / (257 - p.speed));
  uint8_t t2 = static_cast<uint8_t>((sin8(t1) / 4) * 2);
  for (int i = 0; i < 13; i++) {
    int x = sin8(static_cast<uint8_t>(t1 + i * p.intensity / 8)) * (kW - 1) / 255;
    int y = sin8(static_cast<uint8_t>(t2 + i * p.intensity / 8)) * (kH - 1) / 255;
    set_xy(frame, x, y,
           color_from_palette(p.palette_id, static_cast<uint8_t>(i * 255 / 13), p.primary, p.secondary,
                               p.tertiary));
  }
  blur2d(frame, static_cast<uint8_t>(p.custom2 >> (3 + (p.option1 ? 1 : 0))), p.option1);
}
EFFECTS_REGISTER(Id::k2dsindots, mode_2d_sindots)

// wled00/FX.cpp:5261 mode_2DDNASpiral()
void mode_2d_dna_spiral(uint32_t now_ms, const Params &p, State &, Frame frame) {
  unsigned speeds = p.speed / 2 + 7;
  unsigned freq = p.intensity / 8;
  uint32_t ms = now_ms / 20;
  fade_to_black_by(frame, 135);

  for (int i = 0; i < kH; i++) {
    int x = beatsin8(now_ms, static_cast<uint8_t>(speeds), 0, kW - 1, static_cast<uint16_t>(i * freq)) +
            beatsin8(now_ms, static_cast<uint8_t>(speeds - 7), 0, kW - 1, static_cast<uint16_t>(i * freq + 128));
    int x1 = beatsin8(now_ms, static_cast<uint8_t>(speeds), 0, kW - 1, static_cast<uint16_t>(128 + i * freq)) +
             beatsin8(now_ms, static_cast<uint8_t>(speeds - 7), 0, kW - 1,
                       static_cast<uint16_t>(128 + 64 + i * freq));
    unsigned hue = (i * 128 / kH) + ms;
    if ((i + ms / 8) & 3) {
      x /= 2;
      x1 /= 2;
      unsigned steps = static_cast<unsigned>(std::abs(x - x1)) + 1;
      bool positive = x1 >= x;
      for (unsigned k = 1; k <= steps; k++) {
        unsigned rate = k * 255 / steps;
        int dx = positive ? (x + static_cast<int>(k) - 1) : (x - static_cast<int>(k) + 1);
        Rgb col = color_from_palette(p.palette_id, static_cast<uint8_t>(hue), p.primary, p.secondary, p.tertiary);
        add_xy(frame, dx, i, col);
        if (dx >= 0 && dx < kW) frame[i][dx] = scale_rgb(frame[i][dx], static_cast<uint8_t>(rate));
      }
      set_xy(frame, x, i, Rgb{47, 79, 79});    // FastLED CRGB::DarkSlateGray
      set_xy(frame, x1, i, Rgb{255, 255, 255});
    }
  }
  blur2d(frame, static_cast<uint8_t>((static_cast<uint16_t>(p.custom1) * 3) / (6 + (p.option1 ? 1 : 0))),
         p.option1);
}
EFFECTS_REGISTER(Id::k2ddnaspiral, mode_2d_dna_spiral)

// wled00/FX.cpp:5161 mode_2DBlackHole()
void mode_2d_black_hole(uint32_t now_ms, const Params &p, State &, Frame frame) {
  fade_to_black_by(frame, static_cast<uint8_t>(16 + (p.speed >> 3)));
  uint32_t t = now_ms / 128;

  for (unsigned i = 0; i < 8; i++) {
    int x = beatsin8(now_ms, static_cast<uint8_t>(p.custom1 >> 3), 0, kW - 1,
                      static_cast<uint16_t>(((i % 2) ? 128 : 0) + t * i));
    int y = beatsin8(now_ms, static_cast<uint8_t>(p.intensity >> 3), 0, kH - 1,
                      static_cast<uint16_t>(((i % 2) ? 192 : 64) + t * i));
    add_xy(frame, x, y, color_from_palette(p.palette_id, static_cast<uint8_t>(i * 32), p.primary, p.secondary,
                                            p.tertiary));
  }
  for (unsigned i = 0; i < 4; i++) {
    int x = beatsin8(now_ms, static_cast<uint8_t>(p.custom2 >> 3), kW / 4, kW - 1 - kW / 4,
                      static_cast<uint16_t>(((i % 2) ? 128 : 0) + t * i));
    int y = beatsin8(now_ms, p.custom3, kH / 4, kH - 1 - kH / 4,
                      static_cast<uint16_t>(((i % 2) ? 192 : 64) + t * i));
    add_xy(frame, x, y,
           color_from_palette(p.palette_id, static_cast<uint8_t>(255 - i * 64), p.primary, p.secondary,
                               p.tertiary));
  }
  set_xy(frame, kW / 2, kH / 2, Rgb{255, 255, 255});
  if (p.option3) blur2d(frame, 16, (kW * kH) < 100);
}
EFFECTS_REGISTER(Id::k2dblackhole, mode_2d_black_hole)

// wled00/FX.cpp:5052 mode_wavesins()
void mode_wavesins(uint32_t now_ms, const Params &p, State &, Frame frame) {
  for (int x = 0; x < kW; x++) {
    uint8_t bri = sin8(static_cast<uint8_t>(now_ms / 4 + x * p.intensity));
    uint8_t index = beatsin8(now_ms, p.speed, p.custom1, static_cast<uint8_t>(p.custom1 + p.custom2),
                              static_cast<uint16_t>(x * (static_cast<uint32_t>(p.custom3) << 3)));
    Rgb color = color_from_palette(p.palette_id, index, p.primary, p.secondary, p.tertiary);
    fill_column(frame, x, scale_rgb(color, bri));
  }
}
EFFECTS_REGISTER(Id::kWavesins, mode_wavesins)

// wled00/FX.cpp:4962 mode_ColorClouds(). Real WLED's "no palette selected"
// branch renders a raw full-saturation HSV wheel instead of going through
// ColorFromPalette(); this codebase's palette 0 already resolves to a
// sensible default (PartyColors, see palettes.h), so - per this batch's
// porting cheat-sheet - we always go through color_from_palette() and skip
// that branch.
void mode_color_clouds(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  if (state.call == 0) {
    state.aux0 = random16();
    state.aux1 = random16();
  }
  const uint32_t vol_x0 = state.aux0;
  const uint32_t hue_x0 = state.aux1;
  const uint8_t hue_offset0 = static_cast<uint8_t>(vol_x0 + hue_x0);
  const bool cozy = p.option3;

  const uint32_t vol_speed = 1 + p.speed;
  const uint32_t hue_speed = 1 + p.intensity;
  const uint32_t vol_squeeze = 8 + p.custom1;
  const uint32_t hue_squeeze = p.custom2;
  const int32_t vol_cutoff = 12500 + static_cast<int32_t>(p.custom3) * 900;
  const int32_t vol_saturate = 52000;

  const uint32_t vol_t = now_ms * vol_speed / 8;
  const uint32_t hue_t = now_ms * hue_speed / 8;
  const uint8_t hue_offset = beat8_saw(now_ms, 64);

  for (int x = 0; x < kW; x++) {
    const uint32_t vol_x = static_cast<uint32_t>(x) * vol_squeeze * 64;
    int32_t vol = static_cast<int32_t>(perlin16(vol_x0 + vol_x, vol_t));
    vol = static_cast<int32_t>(map_val(vol, vol_cutoff, vol_saturate, 0, 255));
    if (vol < 0) vol = 0;
    if (vol > 255) vol = 255;

    const uint32_t hue_x = static_cast<uint32_t>(x) * hue_squeeze * 8;
    uint8_t hue = static_cast<uint8_t>(perlin16(hue_x0 + hue_x, hue_t) >> 7);
    hue = static_cast<uint8_t>(hue + hue_offset0);
    hue = static_cast<uint8_t>(hue + hue_offset);
    if (cozy) hue = cos8(static_cast<uint8_t>(128 + hue / 2));

    Rgb pixel = scale_rgb(color_from_palette(p.palette_id, hue, p.primary, p.secondary, p.tertiary),
                           static_cast<uint8_t>(vol));
    if (static_cast<int>(pixel.r) + pixel.g + pixel.b <= 2) pixel = Rgb{0, 0, 0};
    fill_column(frame, x, pixel);
  }
}
EFFECTS_REGISTER(Id::kColorclouds, mode_color_clouds)

// wled00/FX.cpp:10866 mode_slow_transition(). Real WLED blends 16 individual
// palette-entry colors (plus a separate white/CCT channel this codebase has
// no equivalent of) toward a new target over `speed` minutes, using a real
// wall-clock timestamp (explicitly not strip.now, per its own comment).
// This engine has no mutable per-entry palette buffer to blend piecewise (see
// palettes.h - palettes are computed on the fly from an id, not stored as 16
// editable stops) and no separate wall clock, only now_ms - so this instead
// tracks the previous and current whole `palette_id`, and blends the two
// *evaluated* colors per pixel by elapsed-time fraction. Visually this is
// the same slow crossfade the original produces, just computed post-lookup
// instead of pre-lookup; the "Sweep" checkbox's per-entry timing tweak has no
// equivalent here and is dropped.
struct SlowTransitionState {
  uint8_t start_palette_id;
  uint8_t end_palette_id;
  uint8_t start_speed;
  uint32_t start_time_ms;
};
static_assert(sizeof(SlowTransitionState) <= State::kDataSize, "SlowTransitionState too big for State::data");

void mode_slow_transition(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  auto &s = *reinterpret_cast<SlowTransitionState *>(state.data);

  if (state.call == 0) {
    s.start_palette_id = s.end_palette_id = p.palette_id;
    s.start_speed = p.speed;
    s.start_time_ms = now_ms;
  } else if (s.end_palette_id != p.palette_id || s.start_speed != p.speed) {
    s.start_palette_id = s.end_palette_id;
    s.end_palette_id = p.palette_id;
    s.start_speed = p.speed;
    s.start_time_ms = now_ms;
  }

  uint32_t duration = (p.speed == 0) ? 10000u : static_cast<uint32_t>(p.speed) * 60000u;
  uint32_t elapsed = now_ms - s.start_time_ms;
  uint32_t blend_amount = static_cast<uint32_t>((static_cast<uint64_t>(elapsed) * 255) / duration);
  if (blend_amount > 255) blend_amount = 255;

  for (int x = 0; x < kW; x++) {
    uint8_t idx = static_cast<uint8_t>(x * 255 / kW);
    Rgb a = color_from_palette(s.start_palette_id, idx, p.primary, p.secondary, p.tertiary);
    Rgb b = color_from_palette(s.end_palette_id, idx, p.primary, p.secondary, p.tertiary);
    fill_column(frame, x, blend(a, b, static_cast<uint8_t>(blend_amount)));
  }
}
EFFECTS_REGISTER(Id::kSlowTransition, mode_slow_transition)

}  // namespace
}  // namespace effects
