#include "effects.h"
#include "wled_compat.h"
#include "palettes.h"
#include "display/gu_display.h"

#include <cmath>
#include <cstdlib>

namespace effects {
namespace {

// ---------------------------------------------------------------------------
// Shared helpers for this batch (2D blur, additive/fade pixel ops, Perlin
// noise, a couple of small FastLED-ish utilities). None of these exist in
// wled_compat.h yet, and this file can't add to it (other batches are
// landing there concurrently) - so they live here, private to this TU.
// ---------------------------------------------------------------------------

uint8_t clamp_u8(int v) {
  if (v < 0) return 0;
  if (v > 255) return 255;
  return static_cast<uint8_t>(v);
}

long map_range(long x, long in_min, long in_max, long out_min, long out_max) {
  if (in_max == in_min) return out_min;
  return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

Rgb scale_rgb(Rgb c, uint8_t s) { return Rgb{scale8(c.r, s), scale8(c.g, s), scale8(c.b, s)}; }

bool same_color(Rgb a, Rgb b) { return a.r == b.r && a.g == b.g && a.b == b.b; }

void set_pixel(Frame frame, int x, int y, Rgb c) {
  if (x < 0 || x >= GuDisplay::WIDTH || y < 0 || y >= GuDisplay::HEIGHT) return;
  frame[y][x] = c;
}

// WLED's Segment::addPixelColorXY(): saturating per-channel add onto whatever
// is already there, instead of overwriting - wled00/FX_fcn.cpp.
void add_pixel(Frame frame, int x, int y, Rgb c) {
  if (x < 0 || x >= GuDisplay::WIDTH || y < 0 || y >= GuDisplay::HEIGHT) return;
  Rgb &p = frame[y][x];
  p = Rgb{qadd8(p.r, c.r), qadd8(p.g, c.g), qadd8(p.b, c.b)};
}

// WLED's Segment::fadePixelColorXY(): fades one pixel toward black by
// fade_amount/255, same math fade_to_black_by() (wled_compat.h) applies to
// the whole frame.
void fade_pixel(Frame frame, int x, int y, uint8_t fade_amount) {
  if (x < 0 || x >= GuDisplay::WIDTH || y < 0 || y >= GuDisplay::HEIGHT) return;
  Rgb &p = frame[y][x];
  p = blend(p, Rgb{0, 0, 0}, fade_amount);
}

// FastLED's lerp8by8(): linear interpolate a->b by frac/256.
uint8_t lerp8by8(uint8_t a, uint8_t b, uint8_t frac) {
  if (b > a) return static_cast<uint8_t>(a + (((b - a) * frac) >> 8));
  return static_cast<uint8_t>(a - (((a - b) * frac) >> 8));
}

// FastLED's blur1d(), applied to one row/column of pixels in place.
void blur_line(Rgb *line, int len, uint8_t blur_amount) {
  if (blur_amount == 0 || len < 2) return;
  uint8_t keep = static_cast<uint8_t>(255 - blur_amount);
  uint8_t seep = static_cast<uint8_t>(blur_amount >> 1);
  Rgb carry{0, 0, 0};
  for (int i = 0; i < len; i++) {
    Rgb cur = line[i];
    Rgb part = scale_rgb(cur, seep);
    Rgb kept = scale_rgb(cur, keep);
    line[i] = Rgb{qadd8(kept.r, carry.r), qadd8(kept.g, carry.g), qadd8(kept.b, carry.b)};
    if (i > 0) {
      Rgb &prev = line[i - 1];
      prev = Rgb{qadd8(prev.r, part.r), qadd8(prev.g, part.g), qadd8(prev.b, part.b)};
    }
    carry = part;
  }
}

// FastLED's blur2d(): blur every row, then every column. WLED's
// Segment::blur() also takes a "smear" bool that changes edge handling
// (wrap vs clamp) for 1D segments mapped onto a 2D matrix - not replicated
// here, this always clamps at the matrix edges.
void blur2d(Frame frame, uint8_t blur_amount) {
  if (blur_amount == 0) return;
  for (int y = 0; y < GuDisplay::HEIGHT; y++) blur_line(frame[y], GuDisplay::WIDTH, blur_amount);
  Rgb col[GuDisplay::HEIGHT];
  for (int x = 0; x < GuDisplay::WIDTH; x++) {
    for (int y = 0; y < GuDisplay::HEIGHT; y++) col[y] = frame[y][x];
    blur_line(col, GuDisplay::HEIGHT, blur_amount);
    for (int y = 0; y < GuDisplay::HEIGHT; y++) frame[y][x] = col[y];
  }
}

// FastLED's HeatColor(): black -> red -> orange -> yellow -> white ramp.
Rgb heat_color(uint8_t temperature) {
  uint8_t t192 = static_cast<uint8_t>((static_cast<unsigned>(temperature) * 191u + 127u) / 255u);
  uint8_t ramp = static_cast<uint8_t>((t192 & 0x3F) << 2);
  if (t192 & 0x80) return Rgb{255, 255, ramp};
  if (t192 & 0x40) return Rgb{255, ramp, 0};
  return Rgb{ramp, 0, 0};
}

// Ken Perlin's classic gradient noise, hash-function flavor: instead of
// FastLED/WLED's fixed 256-entry permutation table (inoise8()/inoise16(),
// not reproduced bit-exact here - same "shape, not bit-exact" tolerance
// wled_compat.h's sin8/beatsin8 already document), each lattice corner's
// gradient index comes from mixing its integer coordinates through a
// standard integer hash. Same smooth 3D noise shape, no table needed.
int hash3(int x, int y, int z) {
  uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u +
               static_cast<uint32_t>(z) * 2147483647u;
  h = (h ^ (h >> 13)) * 1274126177u;
  h ^= h >> 16;
  return static_cast<int>(h & 0xFF);
}

float noise_fade(float t) { return t * t * t * (t * (t * 6 - 15) + 10); }
float noise_lerp(float a, float b, float t) { return a + t * (b - a); }

float noise_grad(int hash, float x, float y, float z) {
  int h = hash & 15;
  float u = h < 8 ? x : y;
  float v = h < 4 ? y : ((h == 12 || h == 14) ? x : z);
  return ((h & 1) ? -u : u) + ((h & 2) ? -v : v);
}

// Returns roughly [-1, 1].
float perlin_raw(float x, float y, float z) {
  int X = static_cast<int>(floorf(x));
  int Y = static_cast<int>(floorf(y));
  int Z = static_cast<int>(floorf(z));
  float fx = x - X, fy = y - Y, fz = z - Z;
  float u = noise_fade(fx), v = noise_fade(fy), w = noise_fade(fz);
  float c000 = noise_grad(hash3(X, Y, Z), fx, fy, fz);
  float c100 = noise_grad(hash3(X + 1, Y, Z), fx - 1, fy, fz);
  float c010 = noise_grad(hash3(X, Y + 1, Z), fx, fy - 1, fz);
  float c110 = noise_grad(hash3(X + 1, Y + 1, Z), fx - 1, fy - 1, fz);
  float c001 = noise_grad(hash3(X, Y, Z + 1), fx, fy, fz - 1);
  float c101 = noise_grad(hash3(X + 1, Y, Z + 1), fx - 1, fy, fz - 1);
  float c011 = noise_grad(hash3(X, Y + 1, Z + 1), fx, fy - 1, fz - 1);
  float c111 = noise_grad(hash3(X + 1, Y + 1, Z + 1), fx - 1, fy - 1, fz - 1);
  float x00 = noise_lerp(c000, c100, u);
  float x10 = noise_lerp(c010, c110, u);
  float x01 = noise_lerp(c001, c101, u);
  float x11 = noise_lerp(c011, c111, u);
  float y0 = noise_lerp(x00, x10, v);
  float y1 = noise_lerp(x01, x11, v);
  return noise_lerp(y0, y1, w);
}

// WLED's perlin8()/perlin16() (FX.cpp): fixed-point noise lookups, low byte
// (perlin8) or low 16 bits (perlin16) of each argument being the fractional
// lattice position - so passing a slowly-changing counter moves smoothly
// through noise space one step at a time, which is the whole point.
uint8_t perlin8(uint32_t x, uint32_t y = 0, uint32_t z = 0) {
  float n = perlin_raw(x / 256.0f, y / 256.0f, z / 256.0f);
  return clamp_u8(static_cast<int>((n * 0.5f + 0.5f) * 255.0f));
}
uint16_t perlin16(uint32_t x, uint32_t y = 0, uint32_t z = 0) {
  float n = perlin_raw(x / 65536.0f, y / 65536.0f, z / 65536.0f);
  int v = static_cast<int>((n * 0.5f + 0.5f) * 65535.0f);
  if (v < 0) v = 0;
  if (v > 65535) v = 65535;
  return static_cast<uint16_t>(v);
}

// ---------------------------------------------------------------------------
// id 144 kPuddlepeak "Puddlepeak" - SKIPPED. wled00/FX.cpp:7159
// mode_puddlepeak() -> mode_puddles_base(true) is audio-reactive
// (getAudioData()/volumeSmth/samplePeak drive the flash size and position);
// not portable without the audio pipeline, so nothing is registered for
// this id. See report.
// ---------------------------------------------------------------------------

// wled00/FX.cpp:5834 mode_2Dnoise()
void mode_2dnoise(uint32_t now_ms, const Params &p, State &, Frame frame) {
  constexpr int cols = GuDisplay::WIDTH;
  constexpr int rows = GuDisplay::HEIGHT;
  unsigned scale = static_cast<unsigned>(p.intensity) + 2;
  uint32_t t = now_ms / (16 - p.speed / 16);
  for (int y = 0; y < rows; y++) {
    for (int x = 0; x < cols; x++) {
      uint8_t hue = perlin8(static_cast<uint32_t>(x) * scale, static_cast<uint32_t>(y) * scale, t);
      frame[y][x] = color_from_palette(p.palette_id, hue, p.primary, p.secondary, p.tertiary);
    }
  }
}
EFFECTS_REGISTER(Id::k2dnoise, mode_2dnoise)

// wled00/FX.cpp:5036 mode_perlinmove(). A de facto 1D effect - each located
// pixel broadcasts down its column (fill_column), same as the four effects
// already in effects.cpp; the persistent frame buffer (only cleared on
// effect switch, see effects.cpp's render()) gives the fade_to_black_by()
// trail the same "slowly moving dots" look real WLED gets from fading its
// own persistent LED buffer.
void mode_perlinmove(uint32_t now_ms, const Params &p, State &, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  fade_to_black_by(frame, static_cast<uint8_t>(255 - p.custom1));
  int count = p.intensity / 16 + 1;
  uint32_t base = now_ms * 128 / (260 - p.speed);
  for (int i = 0; i < count; i++) {
    uint16_t locn = perlin16(base + static_cast<uint32_t>(i) * 15000u, base);
    long pixloc = map_range(locn, 50 * 256, 192 * 256, 0, width - 1);
    if (pixloc >= 0 && pixloc < width) {
      fill_column(frame, static_cast<int>(pixloc),
                  color_from_palette(p.palette_id, static_cast<uint8_t>(pixloc % 255), p.primary,
                                      p.secondary, p.tertiary));
    }
  }
}
EFFECTS_REGISTER(Id::kPerlinmove, mode_perlinmove)

// wled00/FX.cpp:5335 mode_2Dfirenoise()
void mode_2dfirenoise(uint32_t now_ms, const Params &p, State &, Frame frame) {
  constexpr int cols = GuDisplay::WIDTH;
  constexpr int rows = GuDisplay::HEIGHT;
  constexpr Rgb kFirePalette[16] = {
      {0, 0, 0},     {0, 0, 0},       {0, 0, 0},       {0, 0, 0},      {255, 0, 0},
      {255, 0, 0},   {255, 0, 0},     {255, 140, 0},   {255, 140, 0},  {255, 140, 0},
      {255, 165, 0}, {255, 165, 0},   {255, 255, 0},   {255, 165, 0},  {255, 255, 0},
      {255, 255, 0},
  };
  unsigned xscale = static_cast<unsigned>(p.intensity) * 4;
  unsigned yscale = static_cast<unsigned>(p.speed) * 8;
  for (int x = 0; x < cols; x++) {
    for (int y = 0; y < rows; y++) {
      unsigned index = perlin8(static_cast<uint32_t>(x) * yscale * rows / 255,
                                static_cast<uint32_t>(y) * xscale + now_ms / 4);
      unsigned heat = y * index / 11;
      if (heat > 225) heat = 225;
      Rgb color;
      if (p.option1) {
        color = color_from_palette(p.palette_id, static_cast<uint8_t>(heat), p.primary, p.secondary,
                                    p.tertiary);
      } else {
        int slot = static_cast<int>(heat) >> 4;
        int next = slot < 15 ? slot + 1 : 15;
        uint8_t frac = static_cast<uint8_t>((heat & 0x0F) * 17);
        color = blend(kFirePalette[slot], kFirePalette[next], frac);
      }
      frame[y][x] = scale_rgb(color, static_cast<uint8_t>(y * 255 / rows));
    }
  }
}
EFFECTS_REGISTER(Id::k2dfirenoise, mode_2dfirenoise)

// wled00/FX.cpp:5971 mode_2Dsquaredswirl()
void mode_2dsquaredswirl(uint32_t now_ms, const Params &p, State &, Frame frame) {
  constexpr int cols = GuDisplay::WIDTH;
  constexpr int rows = GuDisplay::HEIGHT;
  constexpr uint8_t kBorder = 2;

  fade_to_black_by(frame, static_cast<uint8_t>(1 + p.intensity / 5));
  blur2d(frame, static_cast<uint8_t>(p.custom3 >> 1));

  int i = beatsin8(now_ms, 19, kBorder, cols - kBorder);
  int j = beatsin8(now_ms, 22, kBorder, cols - kBorder);
  int k = beatsin8(now_ms, 17, kBorder, cols - kBorder);
  int m = beatsin8(now_ms, 18, kBorder, rows - kBorder);
  int n = beatsin8(now_ms, 15, kBorder, rows - kBorder);
  int q = beatsin8(now_ms, 20, kBorder, rows - kBorder);

  add_pixel(frame, i, m, color_from_palette(p.palette_id, static_cast<uint8_t>(now_ms / 29), p.primary,
                                              p.secondary, p.tertiary));
  add_pixel(frame, j, n, color_from_palette(p.palette_id, static_cast<uint8_t>(now_ms / 41), p.primary,
                                              p.secondary, p.tertiary));
  add_pixel(frame, k, q, color_from_palette(p.palette_id, static_cast<uint8_t>(now_ms / 73), p.primary,
                                              p.secondary, p.tertiary));
}
EFFECTS_REGISTER(Id::k2dsquaredswirl, mode_2dsquaredswirl)

// wled00/FX.cpp:3161 mode_pacman(). Fixed-size ghost[8]/dot[3] arrays
// replace WLED's single SEGENV.data blob sized to the *current*
// numGhosts/numPowerDots (it reallocates when those sliders change) - we
// just use the first numGhosts/numDots entries each frame instead. The
// live-slider-change immediate-reinit (WLED packs numPowerDots/numGhosts
// into SEGENV.aux0 and forces SEGENV.call back to 0 the instant either
// slider moves) isn't replicated either - changing custom3/intensity here
// takes effect next time this effect is (re)selected, not immediately.
// custom1-gated ghost-blink-start only applies on real WLED strips with
// SEGLEN>=64; this board's SEGLEN (32) always takes the SEGLEN/3 branch.
struct PacManChar {
  int pos = 0;
  int top_pos = 0;
  Rgb color{0, 0, 0};
  bool direction = true;
  bool blue = false;
  bool eaten = false;
};
struct PacManState {
  PacManChar pacman;
  PacManChar ghosts[8];
  PacManChar dots[3];
  uint32_t last_step_ms = 0;
  uint32_t tick = 0;
};
static_assert(sizeof(PacManState) <= State::kDataSize, "PacManState too big for state.data");

void mode_pacman(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  constexpr Rgb kOrangeYellow{255, 204, 0};
  constexpr Rgb kPurpleish{176, 0, 176};
  constexpr Rgb kOrangeish{255, 136, 0};
  constexpr Rgb kWhiteish{153, 153, 153};
  constexpr Rgb kBlack{0, 0, 0};
  constexpr Rgb kRed{255, 0, 0};
  constexpr Rgb kCyan{0, 255, 255};
  constexpr Rgb kYellow{255, 255, 0};
  constexpr Rgb kBlue{0, 0, 255};
  const Rgb ghost_colors[4] = {kRed, kPurpleish, kCyan, kOrangeish};

  auto &s = *reinterpret_cast<PacManState *>(state.data);
  constexpr int kMaxGhosts = 8;
  constexpr int kMaxDots = 3;  // min(SEGLEN/10, 255) with SEGLEN==32

  int num_ghosts = 2 + (p.custom3 * (kMaxGhosts - 2)) / 255;  // WLED: map(custom3,0,31,2,8); custom3 is 0-255 here
  if (num_ghosts > kMaxGhosts) num_ghosts = kMaxGhosts;
  int num_dots = 1 + (p.intensity * (kMaxDots - 1)) / 255;
  bool smear_mode = p.option2;

  int start_blink_pos = width / 3;

  if (state.call == 0) {
    s.pacman = PacManChar{};
    s.pacman.color = kYellow;
    for (int i = 0; i < kMaxGhosts; i++) {
      s.ghosts[i] = PacManChar{};
      s.ghosts[i].color = ghost_colors[i % 4];
      s.ghosts[i].pos = -2 * (i + 2);
    }
    for (int i = 0; i < kMaxDots; i++) {
      s.dots[i] = PacManChar{};
      s.dots[i].color = kOrangeYellow;
    }
    s.dots[0].pos = width - 1;
    s.tick = 0;
    s.last_step_ms = now_ms;
  }

  if (now_ms > s.last_step_ms) {
    s.last_step_ms = now_ms;
    s.tick++;
  }

  if (!smear_mode) {
    for (int x = 0; x < width; x++) fill_column(frame, x, kBlack);
  }

  if (p.option1) {
    int step = p.option3 ? 1 : 2;
    for (int i = width - 1; i > s.pacman.top_pos; i -= step) fill_column(frame, i, kWhiteish);
  }

  if (num_dots > 1) {
    uint32_t every = ((static_cast<uint32_t>(width) - 10u) << 8) / static_cast<uint32_t>(num_dots);
    for (int i = 1; i < num_dots; i++) s.dots[i].pos = 10 + static_cast<int>((static_cast<uint32_t>(i) * every) >> 8);
  }

  if (s.tick % 10 == 0) {
    Rgb dot_color = same_color(s.dots[0].color, kOrangeYellow) ? kBlack : kOrangeYellow;
    for (int i = 0; i < num_dots; i++) s.dots[i].color = dot_color;
  }

  if (s.tick % 15 == 0 && s.ghosts[0].blue && s.pacman.pos <= start_blink_pos) {
    Rgb c = same_color(s.ghosts[0].color, kBlue) ? kWhiteish : kBlue;
    for (int i = 0; i < num_ghosts; i++) s.ghosts[i].color = c;
  }

  for (int i = 0; i < num_dots; i++)
    if (!s.dots[i].eaten && s.dots[i].pos >= 0 && s.dots[i].pos < width) fill_column(frame, s.dots[i].pos, s.dots[i].color);

  for (int i = 0; i < num_dots; i++) {
    if (s.pacman.pos == s.dots[i].pos && !s.dots[i].eaten) {
      s.pacman.direction = false;
      for (int g = 0; g < num_ghosts; g++) {
        s.ghosts[g].direction = false;
        s.ghosts[g].color = kBlue;
        s.ghosts[g].blue = true;
      }
      s.dots[i].eaten = true;
      break;
    }
  }

  if (s.ghosts[0].blue && s.pacman.pos <= 0) {
    s.pacman.direction = true;
    for (int g = 0; g < num_ghosts; g++) {
      s.ghosts[g].direction = true;
      s.ghosts[g].color = ghost_colors[g % 4];
      s.ghosts[g].blue = false;
    }
    if (s.dots[0].eaten) {
      for (int i = 0; i < num_dots; i++) s.dots[i].eaten = false;
      s.pacman.top_pos = 0;
    }
  }

  int speed_div = 15 - (p.speed * 14) / 255;  // WLED: map(speed,0,255,15,1)
  if (speed_div < 1) speed_div = 1;
  if (s.tick % static_cast<uint32_t>(speed_div) == 0) {
    s.pacman.pos += s.pacman.direction ? 1 : -1;
    for (int i = 0; i < num_ghosts; i++) s.ghosts[i].pos += s.ghosts[i].direction ? 1 : -1;
  }

  if (s.pacman.pos >= 0 && s.pacman.pos < width) fill_column(frame, s.pacman.pos, s.pacman.color);
  for (int i = 0; i < num_ghosts; i++)
    if (s.ghosts[i].pos >= 0 && s.ghosts[i].pos < width) fill_column(frame, s.ghosts[i].pos, s.ghosts[i].color);

  if (s.pacman.top_pos < s.pacman.pos) s.pacman.top_pos = s.pacman.pos;
}
EFFECTS_REGISTER(Id::kPacman, mode_pacman)

// wled00/FX.cpp:5243 mode_2Ddna()
void mode_2ddna(uint32_t now_ms, const Params &p, State &, Frame frame) {
  constexpr int cols = GuDisplay::WIDTH;
  constexpr int rows = GuDisplay::HEIGHT;
  fade_to_black_by(frame, 64);
  for (int x = 0; x < cols; x++) {
    int y1 = beatsin8(now_ms, static_cast<uint8_t>(p.speed / 8), 0, rows - 1, static_cast<uint16_t>(x * 4));
    int y2 = beatsin8(now_ms, static_cast<uint8_t>(p.speed / 8), 0, rows - 1, static_cast<uint16_t>(x * 4 + 128));
    uint8_t bri1 = beatsin8(now_ms, 5, 55, 255, static_cast<uint16_t>(x * 10));
    uint8_t bri2 = beatsin8(now_ms, 5, 55, 255, static_cast<uint16_t>(x * 10 + 128));
    Rgb c1 = color_from_palette(p.palette_id, static_cast<uint8_t>(x * 5 + now_ms / 17), p.primary, p.secondary, p.tertiary);
    Rgb c2 = color_from_palette(p.palette_id, static_cast<uint8_t>(x * 5 + 128 + now_ms / 17), p.primary, p.secondary, p.tertiary);
    set_pixel(frame, x, y1, scale_rgb(c1, bri1));
    set_pixel(frame, x, y2, scale_rgb(c2, bri2));
  }
  blur2d(frame, static_cast<uint8_t>(p.intensity / (8 - (p.option1 ? 2 : 0))));
}
EFFECTS_REGISTER(Id::k2ddna, mode_2ddna)

// wled00/FX.cpp:5708 mode_2Dmatrix(). This board's matrix IS the whole
// segment (cols==maxWidth, rows==maxHeight), so the XY() lambda's
// col%cols/row%rows wraparound in the original never actually triggers here
// and is dropped; a plain row*cols+col index is equivalent.
struct MatrixState {
  uint8_t bits[(GuDisplay::WIDTH * GuDisplay::HEIGHT + 7) / 8] = {0};
  uint32_t last_step_ms = 0;
};
static_assert(sizeof(MatrixState) <= State::kDataSize, "MatrixState too big for state.data");

bool bit_get(const uint8_t *bits, int idx) { return (bits[idx >> 3] >> (idx & 7)) & 1u; }
void bit_set(uint8_t *bits, int idx) { bits[idx >> 3] |= static_cast<uint8_t>(1u << (idx & 7)); }
void bit_clear(uint8_t *bits, int idx) { bits[idx >> 3] &= static_cast<uint8_t>(~(1u << (idx & 7))); }

void mode_2dmatrix(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  constexpr int cols = GuDisplay::WIDTH;
  constexpr int rows = GuDisplay::HEIGHT;
  auto &s = *reinterpret_cast<MatrixState *>(state.data);
  if (state.call == 0) s.last_step_ms = 0;

  uint8_t fade = static_cast<uint8_t>(30 + p.custom1 * (250 - 30) / 255);
  // WLED: speed >> map(min(rows,150),0,150,0,3) - always shift-by-0 on this 32-row board.
  uint8_t speed_thresh = static_cast<uint8_t>(256 - p.speed);

  Rgb spawn_color = p.option1 ? p.primary : Rgb{175, 255, 175};
  Rgb trail_color = p.option1 ? p.secondary : Rgb{27, 130, 39};

  bool empty_screen = true;
  if (now_ms - s.last_step_ms >= speed_thresh) {
    s.last_step_ms = now_ms;
    fade_to_black_by(frame, fade);
    for (int row = rows - 1; row >= 0; row--) {
      for (int col = 0; col < cols; col++) {
        int idx = row * cols + col;
        if (bit_get(s.bits, idx)) {
          frame[row][col] = trail_color;
          bit_clear(s.bits, idx);
          if (row < rows - 1) {
            frame[row + 1][col] = spawn_color;
            bit_set(s.bits, (row + 1) * cols + col);
            empty_screen = false;
          }
        }
      }
    }
    if (random8() <= p.intensity || empty_screen) {
      int spawn_x = random8(cols);
      frame[0][spawn_x] = spawn_color;
      bit_set(s.bits, spawn_x);
    }
  }
}
EFFECTS_REGISTER(Id::k2dmatrix, mode_2dmatrix)

// wled00/FX.cpp:5777 mode_2Dmetaballs(). The 3 white marker pixels are
// hoisted out of the original's inner double loop (it redraws them every
// (x,y) iteration, which only ever leaves the same 3 pixels white) - same
// end result, once instead of cols*rows times.
void mode_2dmetaballs(uint32_t now_ms, const Params &p, State &, Frame frame) {
  constexpr int cols = GuDisplay::WIDTH;
  constexpr int rows = GuDisplay::HEIGHT;
  float speed = 0.25f * (1 + (p.speed >> 6));
  uint32_t t = static_cast<uint32_t>(now_ms * speed);

  int x2 = static_cast<int>(map_range(perlin8(t, 25355, 685), 0, 255, 0, cols - 1));
  int y2 = static_cast<int>(map_range(perlin8(t, 355, 11685), 0, 255, 0, rows - 1));
  int x3 = static_cast<int>(map_range(perlin8(t, 55355, 6685), 0, 255, 0, cols - 1));
  int y3 = static_cast<int>(map_range(perlin8(t, 25355, 22685), 0, 255, 0, rows - 1));
  int x1 = beatsin8(now_ms, static_cast<uint8_t>(23 * speed), 0, cols - 1);
  int y1 = beatsin8(now_ms, static_cast<uint8_t>(28 * speed), 0, rows - 1);

  for (int y = 0; y < rows; y++) {
    for (int x = 0; x < cols; x++) {
      int dx = x - x1, dy = y - y1;
      unsigned dist = 2u * static_cast<unsigned>(sqrtf(static_cast<float>(dx * dx + dy * dy)));
      dx = x - x2; dy = y - y2;
      dist += static_cast<unsigned>(sqrtf(static_cast<float>(dx * dx + dy * dy)));
      dx = x - x3; dy = y - y3;
      dist += static_cast<unsigned>(sqrtf(static_cast<float>(dx * dx + dy * dy)));

      int color = dist ? 1000 / static_cast<int>(dist) : 255;
      if (color > 0 && color < 60) {
        frame[y][x] = color_from_palette(p.palette_id, static_cast<uint8_t>(map_range(color * 9, 9, 531, 0, 255)),
                                          p.primary, p.secondary, p.tertiary);
      } else {
        frame[y][x] = color_from_palette(p.palette_id, 0, p.primary, p.secondary, p.tertiary);
      }
    }
  }
  set_pixel(frame, x1, y1, Rgb{255, 255, 255});
  set_pixel(frame, x2, y2, Rgb{255, 255, 255});
  set_pixel(frame, x3, y3, Rgb{255, 255, 255});
}
EFFECTS_REGISTER(Id::k2dmetaballs, mode_2dmetaballs)

// ---------------------------------------------------------------------------
// id 156/157/158 kGravcenter/kGravcentric/kGravfreq - SKIPPED. wled00/
// FX.cpp:6806 mode_gravcenter_base() (shared by all three, and by
// mode_gravimeter which isn't in this batch) is audio-reactive top to
// bottom - it reads getAudioData()'s volumeSmth (and, for Gravfreq,
// FFT_MajorPeak) to size and color the "falling sample" bars. Nothing is
// registered for these three ids. See report.
// ---------------------------------------------------------------------------

// wled00/FX.cpp:5089 mode_shimmer(). SEGENV.step/aux0/aux1 map directly
// onto this codebase's State::step/aux0/aux1 - only the uint32_t lastTime
// needs private scratch space.
struct ShimmerState {
  uint32_t last_time_ms = 0;
};
static_assert(sizeof(ShimmerState) <= State::kDataSize, "ShimmerState too big for state.data");

void mode_shimmer(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  auto &s = *reinterpret_cast<ShimmerState *>(state.data);

  uint32_t radius = (static_cast<uint32_t>(p.custom1) * width >> 7) + 1;
  uint32_t traversal_distance = (static_cast<uint32_t>(width) + 2 * radius) << 8;
  uint32_t traversal_time = 200 + static_cast<uint32_t>(255 - p.speed) * 80;
  uint32_t speed = (traversal_distance << 5) / traversal_time;
  int32_t position = static_cast<int32_t>(state.step);
  uint16_t input_state = static_cast<uint16_t>((static_cast<uint16_t>(p.intensity) << 8) | p.custom1);

  if (state.call == 0 || input_state != state.aux1) {
    position = -static_cast<int32_t>(radius << 8);
    state.aux0 = 0;
    s.last_time_ms = now_ms;
    state.aux1 = input_state;
  }

  if (p.speed) {
    uint32_t delta = (now_ms - s.last_time_ms) & 0x7Fu;
    s.last_time_ms = now_ms;
    if (state.aux0 > 0) {
      state.aux0 = static_cast<uint16_t>(state.aux0 > delta ? state.aux0 - delta : 0);
    } else {
      int32_t step = 1 + static_cast<int32_t>((speed * delta) >> 5);
      position += step;
      int end_position = static_cast<int>((width + radius) << 8);
      if (position > end_position) {
        state.aux0 = static_cast<uint16_t>(static_cast<uint32_t>(p.intensity) * 236);
        if (p.option3) state.aux0 = random16(static_cast<uint16_t>(state.aux0 + 1000));
        position = -static_cast<int32_t>(radius << 8);
      }
      state.step = static_cast<uint32_t>(position);
    }
    if (p.option2) position = (width << 8) - position;
  } else {
    position = width << 7;
  }

  for (int i = 0; i < width; i++) {
    uint32_t dist = static_cast<uint32_t>(std::abs(position - (i << 8)));
    Rgb px;
    if (dist < (radius << 8)) {
      Rgb color = color_from_palette(p.palette_id, static_cast<uint8_t>(i * 255 / width), p.primary,
                                      p.secondary, p.tertiary);
      uint8_t fade_amt = static_cast<uint8_t>(dist / radius);
      if (p.custom2) {
        uint8_t mod_val;
        if (p.option1) {
          uint16_t theta = static_cast<uint16_t>((i * p.custom2) << 6) + static_cast<uint16_t>((now_ms * p.custom3) << 5);
          mod_val = static_cast<uint8_t>((sin16(theta) >> 8) + 128);
        } else {
          mod_val = static_cast<uint8_t>(perlin16(static_cast<uint32_t>(i * p.custom2) << 7,
                                                    static_cast<uint32_t>(now_ms * p.custom3) << 5) >> 8);
        }
        color = scale_rgb(color, mod_val);
      }
      px = blend(color, p.secondary, fade_amt);
    } else {
      px = p.secondary;
    }
    fill_column(frame, i, px);
  }
}
EFFECTS_REGISTER(Id::kShimmer, mode_shimmer)

// wled00/FX.cpp:5923 mode_2DPulser()
void mode_2dpulser(uint32_t now_ms, const Params &p, State &, Frame frame) {
  constexpr int rows = GuDisplay::HEIGHT;
  constexpr int cols = GuDisplay::WIDTH;
  fade_to_black_by(frame, static_cast<uint8_t>(8 - (p.intensity >> 5)));
  uint32_t a = now_ms / (18 - p.speed / 16);
  int x = static_cast<int>((a / 14) % cols);
  int sum = sin8(static_cast<uint8_t>(a * 5)) + sin8(static_cast<uint8_t>(a * 4)) + sin8(static_cast<uint8_t>(a * 2));
  int y = static_cast<int>(map_range(sum, 0, 765, rows - 1, 0));
  set_pixel(frame, x, y,
            color_from_palette(p.palette_id, static_cast<uint8_t>(map_range(y, 0, rows - 1, 0, 255)), p.primary,
                                p.secondary, p.tertiary));
  blur2d(frame, static_cast<uint8_t>(p.intensity >> 4));
}
EFFECTS_REGISTER(Id::k2dpulser, mode_2dpulser)

// wled00/FX.cpp:5307 mode_2DDrift()
void mode_2ddrift(uint32_t now_ms, const Params &p, State &, Frame frame) {
  constexpr int cols = GuDisplay::WIDTH;
  constexpr int rows = GuDisplay::HEIGHT;
  const int cols_center = (cols >> 1) + (cols % 2);
  const int rows_center = (rows >> 1) + (rows % 2);
  fade_to_black_by(frame, 128);
  const float max_dim = (cols > rows ? cols : rows) / 2.0f;
  uint32_t t = now_ms / (32 - (p.speed >> 3));
  uint32_t t20 = t / 20;
  for (float i = 1.0f; i < max_dim; i += 0.25f) {
    float angle = (static_cast<float>(t) * (max_dim - i)) * (static_cast<float>(M_PI) / 180.0f);
    int my_sin = static_cast<int>(sinf(angle) * i);
    int my_cos = static_cast<int>(cosf(angle) * i);
    Rgb c = color_from_palette(p.palette_id, static_cast<uint8_t>(i * 20 + t20), p.primary, p.secondary, p.tertiary);
    set_pixel(frame, cols_center + my_sin, rows_center + my_cos, c);
    if (p.option1) set_pixel(frame, cols_center + my_cos, rows_center + my_sin, c);
  }
  blur2d(frame, static_cast<uint8_t>(p.intensity >> (3 - (p.option2 ? 1 : 0))));
}
EFFECTS_REGISTER(Id::k2ddrift, mode_2ddrift)

// wled00/FX.cpp:6001 mode_2DSunradiation(). bump[] is fully recomputed every
// frame in the original too (SEGENV.data there just avoids a per-frame
// heap alloc/free) - it needs no lifetime across frames, so it's a local
// stack array here instead of state.data (whose 1024-byte budget is
// actually smaller than this 34x34 buffer anyway).
void mode_2dsunradiation(uint32_t now_ms, const Params &p, State &, Frame frame) {
  constexpr int cols = GuDisplay::WIDTH;
  constexpr int rows = GuDisplay::HEIGHT;
  uint8_t bump[(cols + 2) * (rows + 2)];

  uint32_t t = now_ms / 4;
  unsigned some_val = p.speed / 4;
  int idx = 0;
  for (int j = 0; j < rows + 2; j++) {
    for (int i = 0; i < cols + 2; i++) {
      bump[idx++] = static_cast<uint8_t>((static_cast<int16_t>(perlin8(static_cast<uint32_t>(i) * some_val,
                                                                        static_cast<uint32_t>(j) * some_val, t)) -
                                           127) >> 2);
    }
  }

  int yindex = cols + 3;
  int vly = -(rows / 2 + 1);
  for (int y = 0; y < rows; y++) {
    vly++;
    int vlx = -(cols / 2 + 1);
    for (int x = 0; x < cols; x++) {
      vlx++;
      int nx = bump[x + yindex + 1] - bump[x + yindex - 1];
      int ny = bump[x + yindex + (cols + 2)] - bump[x + yindex - (cols + 2)];
      int difx = std::abs(vlx * 7 - nx);
      int dify = std::abs(vly * 7 - ny);
      int temp = difx * difx + dify * dify;
      int col = 255 - temp / 8;
      if (col < 0) col = 0;
      frame[y][x] = heat_color(static_cast<uint8_t>(col / (3.0f - static_cast<float>(p.intensity) / 128.0f)));
    }
    yindex += cols + 2;
  }
}
EFFECTS_REGISTER(Id::k2dsunradiation, mode_2dsunradiation)

// wled00/FX.cpp:5193 mode_2DColoredBursts()
void mode_2dcoloredbursts(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  constexpr int cols = GuDisplay::WIDTH;
  constexpr int rows = GuDisplay::HEIGHT;
  if (state.call == 0) state.aux0 = 0;

  bool dot = p.option3;
  bool grad = p.option1;
  int num_lines = p.intensity / 16 + 1;

  state.aux0++;
  fade_to_black_by(frame, static_cast<uint8_t>(40 - (p.option2 ? 8 : 0)));

  for (int i = 0; i < num_lines; i++) {
    uint8_t x1 = beatsin8(now_ms, static_cast<uint8_t>(2 + p.speed / 16), 0, cols - 1);
    uint8_t x2 = beatsin8(now_ms, static_cast<uint8_t>(1 + p.speed / 16), 0, rows - 1);
    uint8_t y1 = beatsin8(now_ms, static_cast<uint8_t>(5 + p.speed / 16), 0, cols - 1, static_cast<uint16_t>(i * 24));
    uint8_t y2 = beatsin8(now_ms, static_cast<uint8_t>(3 + p.speed / 16), 0, rows - 1, static_cast<uint16_t>(i * 48 + 64));
    Rgb color = color_from_palette(p.palette_id, static_cast<uint8_t>(i * 255 / num_lines + (state.aux0 & 0xFF)),
                                    p.primary, p.secondary, p.tertiary);

    int xsteps = std::abs(x1 - y1) + 1;
    int ysteps = std::abs(x2 - y2) + 1;
    int steps = xsteps >= ysteps ? xsteps : ysteps;
    for (int j = 1; j <= steps; j++) {
      uint8_t rate = static_cast<uint8_t>(j * 255 / steps);
      int dx = lerp8by8(x1, y1, rate);
      int dy = lerp8by8(x2, y2, rate);
      add_pixel(frame, dx, dy, color);
      if (grad) fade_pixel(frame, dx, dy, rate);
    }
    if (dot) {
      set_pixel(frame, x1, x2, Rgb{255, 255, 255});
      set_pixel(frame, y1, y2, Rgb{47, 79, 79});
    }
  }
  blur2d(frame, static_cast<uint8_t>(p.custom3 >> 1));
}
EFFECTS_REGISTER(Id::k2dcoloredbursts, mode_2dcoloredbursts)

// wled00/FX.cpp:5576 mode_2DJulia()
struct JuliaState {
  float xcen = 0.0f;
  float ycen = 0.0f;
  float xymag = 1.0f;
};
static_assert(sizeof(JuliaState) <= State::kDataSize, "JuliaState too big for state.data");

void mode_2djulia(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  constexpr int cols = GuDisplay::WIDTH;
  constexpr int rows = GuDisplay::HEIGHT;
  auto &j = *reinterpret_cast<JuliaState *>(state.data);

  // WLED also snaps custom1/2/3/intensity to 128/128/16/24 here so the UI
  // sliders start centered on first activation - params is caller-owned
  // and immutable here, so that slider-repositioning side effect is
  // dropped; only the fractal center/zoom (this struct) is reset.
  if (state.call == 0) {
    j.xcen = 0.0f;
    j.ycen = 0.0f;
    j.xymag = 1.0f;
  }

  j.xcen += (static_cast<float>(p.custom1) - 128.0f) / 100000.0f;
  j.ycen += (static_cast<float>(p.custom2) - 128.0f) / 100000.0f;
  j.xymag += static_cast<float>((static_cast<int>(p.custom3) - 16) << 3) / 100000.0f;
  if (j.xymag < 0.01f) j.xymag = 0.01f;
  if (j.xymag > 1.0f) j.xymag = 1.0f;

  float xmin = j.xcen - j.xymag, xmax = j.xcen + j.xymag;
  float ymin = j.ycen - j.xymag, ymax = j.ycen + j.xymag;
  xmin = xmin < -1.2f ? -1.2f : (xmin > 1.2f ? 1.2f : xmin);
  xmax = xmax < -1.2f ? -1.2f : (xmax > 1.2f ? 1.2f : xmax);
  ymin = ymin < -0.8f ? -0.8f : (ymin > 1.0f ? 1.0f : ymin);
  ymax = ymax < -0.8f ? -0.8f : (ymax > 1.0f ? 1.0f : ymax);

  int max_iterations = p.intensity / 2;
  if (max_iterations < 1) max_iterations = 1;
  constexpr float max_calc = 16.0f;

  float re = -0.94299f + static_cast<float>(sin16(static_cast<uint16_t>(now_ms * 34))) / 655340.0f;
  float im = 0.3162f + static_cast<float>(sin16(static_cast<uint16_t>(now_ms * 26))) / 655340.0f;

  float dx = (xmax - xmin) / cols;
  float dy = (ymax - ymin) / rows;

  float y = ymin;
  for (int row = 0; row < rows; row++) {
    float x = xmin;
    for (int col = 0; col < cols; col++) {
      float a = x, b = y;
      int iter = 0;
      while (iter < max_iterations) {
        float aa = a * a, bb = b * b;
        if (aa + bb > max_calc) break;
        b = 2 * a * b + im;
        a = aa - bb + re;
        iter++;
      }
      frame[row][col] = (iter == max_iterations)
                             ? Rgb{0, 0, 0}
                             : color_from_palette(p.palette_id, static_cast<uint8_t>(iter * 255 / max_iterations),
                                                   p.primary, p.secondary, p.tertiary);
      x += dx;
    }
    y += dy;
  }
  if (p.option1) blur2d(frame, 100);
}
EFFECTS_REGISTER(Id::k2djulia, mode_2djulia)

}  // namespace
}  // namespace effects
