#include "effects.h"
#include "wled_compat.h"
#include "palettes.h"
#include "display/gu_display.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

// Batch 6 assignment also included id 129 (Pixelwave), 131 (Matripix), 132
// (Gravimeter) and 134 (Puddles). All four turned out to be Andrew
// Tuline's audio-reactive routines (mode_gravcenter_base()/getAudioData()/
// volumeSmth/volumeRaw) despite the classifier tagging them non-audio -
// out of scope per this batch's brief, so none of the four are registered
// here. id 122 (Scrolling Text, mode_2Dscrollingtext()) is also skipped:
// it renders through WLED's FontManager/glyph-cache subsystem (RTC time
// tokens, UTF-8 decoding, variable-width bitmap fonts), none of which
// exists anywhere in this codebase - there's no font/glyph infrastructure
// to port it onto.

namespace effects {
namespace {

// ---------------------------------------------------------------------
// Shared helpers for this batch (local to this file - see effects.cpp's
// own private color_wheel()/clamp_u8() for the precedent of duplicating
// small helpers per translation unit rather than growing wled_compat.h).
// ---------------------------------------------------------------------

uint8_t clamp_u8(int v) {
  if (v < 0) return 0;
  if (v > 255) return 255;
  return static_cast<uint8_t>(v);
}

int iabs(int v) { return v < 0 ? -v : v; }

// General HSV->RGB (WLED/FastLED's CHSV, full range h/s/v) - effects.cpp's
// private color_wheel() only covers the s=255,v=255 case; this batch also
// needs partial saturation (Crazy Bees). Float trig, not FastLED's exact
// integer algorithm - same "matches the shape" tolerance wled_compat.h
// documents for its own math helpers.
Rgb hsv2rgb(uint8_t h, uint8_t s, uint8_t v) {
  float hf = h * 360.0f / 255.0f;
  float sf = s / 255.0f;
  float vf = v / 255.0f;
  float c = vf * sf;
  float x = c * (1.0f - fabsf(fmodf(hf / 60.0f, 2.0f) - 1.0f));
  float m = vf - c;
  float rp, gp, bp;
  if (hf < 60)       { rp = c;  gp = x;  bp = 0; }
  else if (hf < 120) { rp = x;  gp = c;  bp = 0; }
  else if (hf < 180) { rp = 0;  gp = c;  bp = x; }
  else if (hf < 240) { rp = 0;  gp = x;  bp = c; }
  else if (hf < 300) { rp = x;  gp = 0;  bp = c; }
  else               { rp = c;  gp = 0;  bp = x; }
  return Rgb{clamp_u8(static_cast<int>((rp + m) * 255.0f)),
             clamp_u8(static_cast<int>((gp + m) * 255.0f)),
             clamp_u8(static_cast<int>((bp + m) * 255.0f))};
}

// Recovers just the hue of an RGB triple (WLED's rgb2hsv().h, used by
// Distortion Waves' palette-index-from-hue path).
uint8_t rgb_to_hue(uint8_t r, uint8_t g, uint8_t b) {
  uint8_t maxc = std::max({r, g, b});
  uint8_t minc = std::min({r, g, b});
  int delta = maxc - minc;
  if (delta == 0) return 0;
  float h;
  if (maxc == r)      h = 60.0f * fmodf(static_cast<float>(g - b) / delta, 6.0f);
  else if (maxc == g) h = 60.0f * (static_cast<float>(b - r) / delta + 2.0f);
  else                h = 60.0f * (static_cast<float>(r - g) / delta + 4.0f);
  if (h < 0) h += 360.0f;
  return static_cast<uint8_t>(h * 255.0f / 360.0f);
}

Rgb scale_rgb(Rgb c, uint8_t scale) {
  return Rgb{scale8(c.r, scale), scale8(c.g, scale), scale8(c.b, scale)};
}

// WLED's color_add(c1, c2, false): plain per-channel saturating add. Used
// by blur2D()/box-filter style code.
Rgb add_rgb_sat(Rgb a, Rgb b) {
  return Rgb{qadd8(a.r, b.r), qadd8(a.g, b.g), qadd8(a.b, b.b)};
}

// WLED's color_add(c1, c2, true) (Segment::addPixelColorXY()'s default):
// ratio-preserving add - scales all channels down together on overflow
// instead of clipping each independently, to avoid a hue shift.
Rgb add_rgb_ratio(Rgb a, Rgb b) {
  if (a.r == 0 && a.g == 0 && a.b == 0) return b;
  if (b.r == 0 && b.g == 0 && b.b == 0) return a;
  unsigned r = static_cast<unsigned>(a.r) + b.r;
  unsigned g = static_cast<unsigned>(a.g) + b.g;
  unsigned bl = static_cast<unsigned>(a.b) + b.b;
  unsigned m = std::max({r, g, bl});
  if (m > 255) {
    r = r * 255 / m;
    g = g * 255 / m;
    bl = bl * 255 / m;
  }
  return Rgb{static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(bl)};
}

// wled00/FX_2Dfcn.cpp:246 Segment::blur2D(), ported onto `Frame` directly.
void blur2d(Frame frame, uint8_t blur_x, uint8_t blur_y, bool smear) {
  constexpr int W = GuDisplay::WIDTH;
  constexpr int H = GuDisplay::HEIGHT;
  if (blur_x) {
    uint8_t keep = smear ? 255 : static_cast<uint8_t>(255 - blur_x);
    uint8_t seep = blur_x >> 1;
    for (int y = 0; y < H; y++) {
      Rgb cur = frame[y][0];
      Rgb carry = scale_rgb(cur, seep);
      frame[y][0] = scale_rgb(cur, keep);
      for (int x = 1; x < W; x++) {
        cur = frame[y][x];
        Rgb part = scale_rgb(cur, seep);
        cur = add_rgb_sat(scale_rgb(cur, keep), carry);
        frame[y][x - 1] = add_rgb_sat(frame[y][x - 1], part);
        frame[y][x] = cur;
        carry = part;
      }
    }
  }
  if (blur_y) {
    uint8_t keep = smear ? 255 : static_cast<uint8_t>(255 - blur_y);
    uint8_t seep = blur_y >> 1;
    for (int x = 0; x < W; x++) {
      Rgb cur = frame[0][x];
      Rgb carry = scale_rgb(cur, seep);
      frame[0][x] = scale_rgb(cur, keep);
      for (int y = 1; y < H; y++) {
        cur = frame[y][x];
        Rgb part = scale_rgb(cur, seep);
        cur = add_rgb_sat(scale_rgb(cur, keep), carry);
        frame[y - 1][x] = add_rgb_sat(frame[y - 1][x], part);
        frame[y][x] = cur;
        carry = part;
      }
    }
  }
}

// wled00/FX_2Dfcn.cpp:365/392/423 Segment::moveX/moveY/move().
void move_x(Frame frame, int delta, bool wrap) {
  constexpr int W = GuDisplay::WIDTH;
  constexpr int H = GuDisplay::HEIGHT;
  if (!delta) return;
  int absDelta = iabs(delta);
  if (absDelta >= W) return;
  int newDelta, stop = W, start = 0;
  if (wrap) newDelta = ((delta % W) + W) % W;
  else {
    if (delta < 0) start = absDelta;
    stop = W - absDelta;
    newDelta = delta > 0 ? delta : 0;
  }
  for (int y = 0; y < H; y++) {
    Rgb row[W];
    for (int x = 0; x < stop; x++) {
      int srcX = x + newDelta;
      if (wrap) srcX %= W;
      row[x] = frame[y][srcX];
    }
    for (int x = 0; x < stop; x++) frame[y][x + start] = row[x];
  }
}

void move_y(Frame frame, int delta, bool wrap) {
  constexpr int W = GuDisplay::WIDTH;
  constexpr int H = GuDisplay::HEIGHT;
  if (!delta) return;
  int absDelta = iabs(delta);
  if (absDelta >= H) return;
  int newDelta, stop = H, start = 0;
  if (wrap) newDelta = ((delta % H) + H) % H;
  else {
    if (delta < 0) start = absDelta;
    stop = H - absDelta;
    newDelta = delta > 0 ? delta : 0;
  }
  for (int x = 0; x < W; x++) {
    Rgb col[H];
    for (int y = 0; y < stop; y++) {
      int srcY = y + newDelta;
      if (wrap) srcY %= H;
      col[y] = frame[srcY][x];
    }
    for (int y = 0; y < stop; y++) frame[y + start][x] = col[y];
  }
}

void move_frame(Frame frame, int dir, int delta, bool wrap = false) {
  switch (dir) {
    case 0: move_x(frame, delta, wrap); break;
    case 1: move_x(frame, delta, wrap); move_y(frame, delta, wrap); break;
    case 2: move_y(frame, delta, wrap); break;
    case 3: move_x(frame, -delta, wrap); move_y(frame, delta, wrap); break;
    case 4: move_x(frame, -delta, wrap); break;
    case 5: move_x(frame, -delta, wrap); move_y(frame, -delta, wrap); break;
    case 6: move_y(frame, -delta, wrap); break;
    case 7: move_x(frame, delta, wrap); move_y(frame, -delta, wrap); break;
  }
}

// wled00/FX_2Dfcn.cpp:493 Segment::fillCircle(), hard-edge (soft=false)
// case only - the only one this batch's callers use.
void fill_circle(Frame frame, int cx, int cy, int radius, Rgb c) {
  constexpr int W = GuDisplay::WIDTH;
  constexpr int H = GuDisplay::HEIGHT;
  if (radius <= 0) return;
  int radius2 = radius * radius + radius;
  for (int y = -radius; y <= radius; y++) {
    for (int x = -radius; x <= radius; x++) {
      if (x * x + y * y < radius2 && cx + x >= 0 && cy + y >= 0 && cx + x < W && cy + y < H)
        frame[cy + y][cx + x] = c;
    }
  }
}

// wled00/FX_2Dfcn.cpp:567 Segment::wu_pixel(). x,y are 24.8 fixed-point
// pixel coordinates, matching the original.
void wu_pixel(Frame frame, uint32_t x, uint32_t y, Rgb c) {
  constexpr int W = GuDisplay::WIDTH;
  constexpr int H = GuDisplay::HEIGHT;
  unsigned xx = x & 0xff, yy = y & 0xff, ix = 255 - xx, iy = 255 - yy;
  uint8_t wu[4] = {
      static_cast<uint8_t>((ix * iy + ix + iy) >> 8),
      static_cast<uint8_t>((xx * iy + xx + iy) >> 8),
      static_cast<uint8_t>((ix * yy + ix + yy) >> 8),
      static_cast<uint8_t>((xx * yy + xx + yy) >> 8),
  };
  for (int i = 0; i < 4; i++) {
    int wx = static_cast<int>(x >> 8) + (i & 1);
    int wy = static_cast<int>(y >> 8) + ((i >> 1) & 1);
    if (static_cast<unsigned>(wx) >= static_cast<unsigned>(W) ||
        static_cast<unsigned>(wy) >= static_cast<unsigned>(H))
      continue;
    Rgb &led = frame[wy][wx];
    led.r = qadd8(led.r, static_cast<uint8_t>((c.r * wu[i]) >> 8));
    led.g = qadd8(led.g, static_cast<uint8_t>((c.g * wu[i]) >> 8));
    led.b = qadd8(led.b, static_cast<uint8_t>((c.b * wu[i]) >> 8));
  }
}

// Stand-in for FastLED's inoise8()/perlin16() (a lattice-gradient Perlin
// implementation this codebase doesn't have): hash-based trilinear value
// noise instead. Smooth and continuous like the original, not bit-exact -
// same tolerance wled_compat.h documents for its own math helpers. x,y,z
// are 24.8 fixed-point noise-space coordinates.
uint32_t lattice_hash(int32_t x, int32_t y, int32_t z) {
  uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u +
               static_cast<uint32_t>(z) * 2147483647u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return h ^ (h >> 16);
}

uint16_t lerp16(uint16_t a, uint16_t b, uint32_t f) {
  return static_cast<uint16_t>(a + (((static_cast<int32_t>(b) - static_cast<int32_t>(a)) *
                                      static_cast<int32_t>(f)) >>
                                     8));
}

uint16_t value_noise16(int32_t x, int32_t y, int32_t z) {
  int32_t xi = x >> 8, yi = y >> 8, zi = z >> 8;
  uint32_t xf = static_cast<uint32_t>(x) & 0xFF;
  uint32_t yf = static_cast<uint32_t>(y) & 0xFF;
  uint32_t zf = static_cast<uint32_t>(z) & 0xFF;
  uint16_t c000 = static_cast<uint16_t>(lattice_hash(xi, yi, zi));
  uint16_t c100 = static_cast<uint16_t>(lattice_hash(xi + 1, yi, zi));
  uint16_t c010 = static_cast<uint16_t>(lattice_hash(xi, yi + 1, zi));
  uint16_t c110 = static_cast<uint16_t>(lattice_hash(xi + 1, yi + 1, zi));
  uint16_t c001 = static_cast<uint16_t>(lattice_hash(xi, yi, zi + 1));
  uint16_t c101 = static_cast<uint16_t>(lattice_hash(xi + 1, yi, zi + 1));
  uint16_t c011 = static_cast<uint16_t>(lattice_hash(xi, yi + 1, zi + 1));
  uint16_t c111 = static_cast<uint16_t>(lattice_hash(xi + 1, yi + 1, zi + 1));
  uint16_t x00 = lerp16(c000, c100, xf), x10 = lerp16(c010, c110, xf);
  uint16_t x01 = lerp16(c001, c101, xf), x11 = lerp16(c011, c111, xf);
  uint16_t y0 = lerp16(x00, x10, yf), y1 = lerp16(x01, x11, yf);
  return lerp16(y0, y1, zf);
}

uint8_t value_noise8(int32_t x, int32_t y, int32_t z) {
  return static_cast<uint8_t>(value_noise16(x, y, z) >> 8);
}

// FastLED math8.h's triwave8()/ease8InOutQuad()/quadwave8()/
// ease8InOutCubic() - small standalone integer easing curves Blends and
// Soap need.
uint8_t triwave8(uint8_t in) {
  if (in & 0x80) in = static_cast<uint8_t>(255 - in);
  return static_cast<uint8_t>(in << 1);
}
uint8_t ease8InOutQuad(uint8_t i) {
  uint8_t j = i;
  if (j & 0x80) j = static_cast<uint8_t>(255 - j);
  uint8_t jj = scale8(j, j);
  uint8_t jj2 = static_cast<uint8_t>(jj << 1);
  if (i & 0x80) jj2 = static_cast<uint8_t>(255 - jj2);
  return jj2;
}
uint8_t quadwave8(uint8_t in) { return ease8InOutQuad(triwave8(in)); }
uint8_t ease8InOutCubic(uint8_t i) {
  uint8_t ii = scale8(i, i);
  uint8_t iii = scale8(ii, i);
  uint16_t r1 = static_cast<uint16_t>((3u * ii) - (2u * iii));
  if (r1 & 0x100) return 255;
  return static_cast<uint8_t>(r1);
}

// Arduino's map(), integer, not clamped.
long map_range(long x, long in_min, long in_max, long out_min, long out_max) {
  return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

// ---------------------------------------------------------------------
// Effects
// ---------------------------------------------------------------------

// wled00/FX.cpp:6600 mode_2Dplasmarotozoom(). Real WLED's per-pixel
// plasma[] byte map (SEGMENT.length() bytes, exactly 1024 on this board)
// plus a persisted rotation-angle float would overflow State::kDataSize
// by 4 bytes, so the angle is packed into state.step's bit pattern
// (memcpy, not a strict-aliasing reinterpret) instead of taking data[]
// space, leaving the full 1024 bytes for the plasma map.
void mode_2dplasmarotozoom(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  constexpr int cols = GuDisplay::WIDTH;
  constexpr int rows = GuDisplay::HEIGHT;
  struct PlasmaState {
    uint8_t plasma[cols * rows];
  };
  static_assert(sizeof(PlasmaState) <= State::kDataSize, "PlasmaState too big");
  auto &s = *reinterpret_cast<PlasmaState *>(state.data);
  float a;
  std::memcpy(&a, &state.step, sizeof(float));

  unsigned ms = now_ms / 15;
  for (int j = 0; j < rows; j++) {
    int index = j * cols;
    for (int i = 0; i < cols; i++) {
      if (p.option1)
        s.plasma[index + i] = static_cast<uint8_t>(static_cast<uint8_t>(i * 4 ^ j * 4) + static_cast<uint8_t>(ms / 6));
      else
        s.plasma[index + i] = value_noise8(i * 40, j * 40, static_cast<int32_t>(ms));
    }
  }

  float f = (sinf(a / 2.0f) + ((128 - p.intensity) / 128.0f) + 1.1f) / 1.5f;
  float kosinus = cosf(a) * f;
  float sinus = sinf(a) * f;
  for (int i = 0; i < cols; i++) {
    float u1 = i * kosinus;
    float v1 = i * sinus;
    for (int j = 0; j < rows; j++) {
      uint8_t u = static_cast<uint8_t>(iabs(static_cast<int8_t>(u1 - j * sinus))) % cols;
      uint8_t v = static_cast<uint8_t>(iabs(static_cast<int8_t>(v1 + j * kosinus))) % rows;
      frame[j][i] = color_from_palette(p.palette_id, s.plasma[v * cols + u], p.primary, p.secondary, p.tertiary);
    }
  }
  a -= 0.03f + (static_cast<float>(static_cast<int>(p.speed) - 128) * 0.0002f);
  if (a < -6283.18530718f) a += 6283.18530718f;
  std::memcpy(&state.step, &a, sizeof(float));
}
EFFECTS_REGISTER(Id::k2dplasmarotozoom, mode_2dplasmarotozoom)

// wled00/FX.cpp:4659 mode_blends(). pixelLen is min(SEGLEN,255); on this
// board SEGLEN==WIDTH==32 so the buffer is exactly one column per pixel -
// the wraparound `offset` indexing the real source uses to fit longer
// strips into a <=255-entry buffer never triggers here.
void mode_blends(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  struct BlendsState {
    Rgb pixels[width];
  };
  static_assert(sizeof(BlendsState) <= State::kDataSize, "BlendsState too big");
  auto &s = *reinterpret_cast<BlendsState *>(state.data);

  uint8_t blendSpeed = static_cast<uint8_t>(map_range(p.intensity, 0, 255, 10, 128));
  uint32_t shift = (now_ms * ((p.speed >> 3) + 1)) >> 8;

  for (int i = 0; i < width; i++) {
    Rgb target = color_from_palette(p.palette_id, static_cast<uint8_t>(shift + quadwave8(static_cast<uint8_t>((i + 1) * 16))),
                                     p.primary, p.secondary, p.tertiary);
    s.pixels[i] = blend(s.pixels[i], target, blendSpeed);
    shift += 3;
  }
  for (int i = 0; i < width; i++) fill_column(frame, i, s.pixels[i]);
}
EFFECTS_REGISTER(Id::kBlends, mode_blends)

// wled00/FX.cpp:4706 mode_tv_simulator(). `elapsed` and `pixelNum` are
// struct fields in real WLED but elapsed is fully recomputed from
// scratch every call (no cross-frame dependency) and pixelNum is unused
// there too - both dropped, kept as a local instead of state.
struct TvSimState {
  uint32_t totalTime = 0, fadeTime = 0, startTime = 0;
  uint32_t sceeneStart = 0, sceeneDuration = 0;
  uint16_t sliderValues = 0;
  uint16_t sceeneColorHue = 0;
  uint8_t sceeneColorSat = 0, sceeneColorBri = 0;
  uint8_t actualColorR = 0, actualColorG = 0, actualColorB = 0;
  uint16_t pr = 0, pg = 0, pb = 0;
};
static_assert(sizeof(TvSimState) <= State::kDataSize, "TvSimState too big");

void mode_tv_simulator(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  auto &s = *reinterpret_cast<TvSimState *>(state.data);

  uint8_t colorSpeed = static_cast<uint8_t>(map_range(p.speed, 0, 255, 1, 20));
  uint8_t colorIntensity = static_cast<uint8_t>(map_range(p.intensity, 0, 255, 10, 30));

  int slider = (static_cast<int>(p.speed) << 8) | p.intensity;
  if (slider != s.sliderValues) {
    s.sliderValues = static_cast<uint16_t>(slider);
    state.aux1 = 0;
  }

  if ((now_ms - s.sceeneStart) >= s.sceeneDuration || state.aux1 == 0) {
    s.sceeneStart = now_ms;
    s.sceeneDuration = random16(static_cast<uint16_t>(60u * 250u * colorSpeed), static_cast<uint16_t>(60u * 750u * colorSpeed));
    s.sceeneColorHue = random16(0, 768);
    s.sceeneColorSat = random8(100, static_cast<uint8_t>(130 + colorIntensity));
    s.sceeneColorBri = random8(200, 240);
    state.aux1 = 1;
    state.aux0 = 0;
  }

  if (state.aux0 == 0) {
    int j = random8(static_cast<uint8_t>(4 * colorIntensity));
    int hue = (random8() < 128) ? ((j < s.sceeneColorHue) ? s.sceeneColorHue - j : 767 - s.sceeneColorHue - j)
                                 : ((j + s.sceeneColorHue) < 767 ? s.sceeneColorHue + j : s.sceeneColorHue + j - 767);
    j = random8(static_cast<uint8_t>(2 * colorIntensity));
    uint8_t sat = (s.sceeneColorSat - j) < 0 ? 0 : static_cast<uint8_t>(s.sceeneColorSat - j);
    j = random8(100);
    uint8_t bri = (s.sceeneColorBri - j) < 0 ? 0 : static_cast<uint8_t>(s.sceeneColorBri - j);

    uint8_t temp[5];
    uint8_t n = static_cast<uint8_t>((hue >> 8) % 3);
    uint8_t x = static_cast<uint8_t>(((((hue & 255) * sat) >> 8) * bri) >> 8);
    uint8_t base = static_cast<uint8_t>(((256 - sat) * bri) >> 8);
    temp[0] = temp[3] = base;
    temp[1] = temp[4] = static_cast<uint8_t>(x + base);
    temp[2] = static_cast<uint8_t>(bri - x);
    s.actualColorR = temp[n + 2];
    s.actualColorG = temp[n + 1];
    s.actualColorB = temp[n];
  }

  int nr = s.actualColorR * 257;
  int ng = s.actualColorG * 257;
  int nb = s.actualColorB * 257;

  if (state.aux0 == 0) {
    state.aux0 = 1;
    s.totalTime = random16(250, 2500);
    s.fadeTime = random16(0, static_cast<uint16_t>(s.totalTime));
    if (random8(10) < 3) s.fadeTime = 0;
    s.startTime = now_ms;
  }

  uint32_t elapsed = now_ms - s.startTime;
  int r, g, b;
  if (elapsed < s.fadeTime) {
    r = static_cast<int>(map_range(elapsed, 0, s.fadeTime, s.pr, nr));
    g = static_cast<int>(map_range(elapsed, 0, s.fadeTime, s.pg, ng));
    b = static_cast<int>(map_range(elapsed, 0, s.fadeTime, s.pb, nb));
  } else {
    r = nr;
    g = ng;
    b = nb;
  }

  Rgb c{static_cast<uint8_t>(r >> 8), static_cast<uint8_t>(g >> 8), static_cast<uint8_t>(b >> 8)};
  for (int x = 0; x < width; x++) fill_column(frame, x, c);

  if (elapsed >= s.totalTime) {
    s.pr = static_cast<uint16_t>(nr);
    s.pg = static_cast<uint16_t>(ng);
    s.pb = static_cast<uint16_t>(nb);
    state.aux0 = 0;
  }
}
EFFECTS_REGISTER(Id::kTvSimulator, mode_tv_simulator)

// wled00/FX.cpp:420 mode_dynamic_smooth(), which is wled00/FX.cpp:386
// mode_dynamic() with SEGMENT.check1 (its smooth-blend path) forced on -
// only that branch is ported, since our board always takes it.
void mode_dynamic_smooth(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  struct DynamicState {
    uint8_t hue[width];
  };
  static_assert(sizeof(DynamicState) <= State::kDataSize, "DynamicState too big");
  auto &s = *reinterpret_cast<DynamicState *>(state.data);

  if (state.call == 0)
    for (int i = 0; i < width; i++) s.hue[i] = random8();

  uint32_t cycleTime = 50 + (255 - p.speed) * 15;
  uint32_t it = now_ms / cycleTime;
  if (it != state.step && p.speed != 0) {
    for (int i = 0; i < width; i++)
      if (random8() <= p.intensity) s.hue[i] = random8();
    state.step = it;
  }

  for (int i = 0; i < width; i++) {
    Rgb target = hsv2rgb(s.hue[i], 255, 255);
    fill_column(frame, i, blend(frame[0][i], target, 16));
  }
}
EFFECTS_REGISTER(Id::kDynamicSmooth, mode_dynamic_smooth)

// wled00/FX.cpp:6086 mode_2Dspaceships().
void mode_2dspaceships(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  constexpr int cols = GuDisplay::WIDTH;
  constexpr int rows = GuDisplay::HEIGHT;

  uint32_t tb = now_ms >> 12;
  if (tb > state.step) {
    int dir = static_cast<int>(state.aux0) + 1;
    dir += static_cast<int>(random8(3)) - 1;
    if (dir > 7) state.aux0 = 0;
    else if (dir < 0) state.aux0 = 7;
    else state.aux0 = static_cast<uint16_t>(dir);
    state.step = tb + random8(4);
  }

  fade_to_black_by(frame, static_cast<uint8_t>(map_range(p.speed, 0, 255, 248, 16)));
  move_frame(frame, state.aux0, 1);

  for (int i = 0; i < 8; i++) {
    int x = beatsin8(now_ms, static_cast<uint8_t>(12 + i), 2, static_cast<uint8_t>(cols - 3));
    int y = beatsin8(now_ms, static_cast<uint8_t>(15 + i), 2, static_cast<uint8_t>(rows - 3));
    Rgb color = color_from_palette(p.palette_id, beatsin8(now_ms, static_cast<uint8_t>(12 + i), 0, 255), p.primary,
                                    p.secondary, p.tertiary);
    frame[y][x] = add_rgb_ratio(frame[y][x], color);
    if (cols > 24 || rows > 24) {
      if (x + 1 < cols) frame[y][x + 1] = add_rgb_ratio(frame[y][x + 1], color);
      if (x - 1 >= 0) frame[y][x - 1] = add_rgb_ratio(frame[y][x - 1], color);
      if (y + 1 < rows) frame[y + 1][x] = add_rgb_ratio(frame[y + 1][x], color);
      if (y - 1 >= 0) frame[y - 1][x] = add_rgb_ratio(frame[y - 1][x], color);
    }
  }
  blur2d(frame, static_cast<uint8_t>(p.intensity >> 3), static_cast<uint8_t>(p.intensity >> 3), p.option1);
}
EFFECTS_REGISTER(Id::k2dspaceships, mode_2dspaceships)

// wled00/FX.cpp:6127 mode_2Dcrazybees(). WLED keys the bee AI off its own
// seedable `prng`; this codebase's random8/16 (RP2040 hardware RNG,
// unseedable) stands in - the bees only need decorrelated random draws,
// not reproducibility.
void mode_2dcrazybees(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  constexpr int cols = GuDisplay::WIDTH;
  constexpr int rows = GuDisplay::HEIGHT;
  constexpr int kMaxBees = 5;

  struct Bee {
    uint8_t posX, posY, aimX, aimY, hue;
    int8_t deltaX, deltaY, signX, signY, error;
  };
  struct CrazyBeesState {
    Bee bees[kMaxBees];
  };
  static_assert(sizeof(CrazyBeesState) <= State::kDataSize, "CrazyBeesState too big");
  auto &s = *reinterpret_cast<CrazyBeesState *>(state.data);

  auto aim = [&](Bee &b) {
    b.aimX = random8(0, cols);
    b.aimY = random8(0, rows);
    b.hue = random8();
    b.deltaX = static_cast<int8_t>(iabs(b.aimX - b.posX));
    b.deltaY = static_cast<int8_t>(iabs(b.aimY - b.posY));
    b.signX = static_cast<int8_t>(b.posX < b.aimX ? 1 : -1);
    b.signY = static_cast<int8_t>(b.posY < b.aimY ? 1 : -1);
    b.error = static_cast<int8_t>(b.deltaX - b.deltaY);
  };

  int n = std::min(kMaxBees, (rows * cols) / 256 + 1);

  if (state.call == 0) {
    for (int i = 0; i < n; i++) {
      s.bees[i].posX = random8(0, cols);
      s.bees[i].posY = random8(0, rows);
      aim(s.bees[i]);
    }
  }

  if (now_ms > state.step) {
    state.step = now_ms + (16u * 16u) / ((p.speed >> 4) + 1);  // FRAMETIME approximated as 16ms/frame
    fade_to_black_by(frame, static_cast<uint8_t>(32 + ((p.option1 * p.intensity) / 25)));
    uint8_t blurAmt = static_cast<uint8_t>(p.intensity / (2 + p.option1 * 9));
    blur2d(frame, blurAmt, blurAmt, p.option1);
    for (int i = 0; i < n; i++) {
      Bee &b = s.bees[i];
      Rgb flowerColor = color_from_palette(p.palette_id, b.hue, p.primary, p.secondary, p.tertiary);
      if (b.aimX + 1 < cols) frame[b.aimY][b.aimX + 1] = add_rgb_ratio(frame[b.aimY][b.aimX + 1], flowerColor);
      if (b.aimY + 1 < rows) frame[b.aimY + 1][b.aimX] = add_rgb_ratio(frame[b.aimY + 1][b.aimX], flowerColor);
      if (b.aimX >= 1) frame[b.aimY][b.aimX - 1] = add_rgb_ratio(frame[b.aimY][b.aimX - 1], flowerColor);
      if (b.aimY >= 1) frame[b.aimY - 1][b.aimX] = add_rgb_ratio(frame[b.aimY - 1][b.aimX], flowerColor);
      if (b.posX != b.aimX || b.posY != b.aimY) {
        frame[b.posY][b.posX] = hsv2rgb(b.hue, 60, 255);
        int error2 = b.error * 2;
        if (error2 > -b.deltaY) {
          b.error = static_cast<int8_t>(b.error - b.deltaY);
          b.posX = static_cast<uint8_t>(b.posX + b.signX);
        }
        if (error2 < b.deltaX) {
          b.error = static_cast<int8_t>(b.error + b.deltaX);
          b.posY = static_cast<uint8_t>(b.posY + b.signY);
        }
      } else {
        aim(b);
      }
    }
  }
}
EFFECTS_REGISTER(Id::k2dcrazybees, mode_2dcrazybees)

// wled00/FX.cpp:6199 mode_2Dghostrider(). Real WLED only builds this
// (rather than its particle-system replacement, mode_particleghostrider())
// when WLED_PS_DONT_REPLACE_2D_FX is defined; the legacy math it ports
// doesn't touch audio or the particle system, so it stands on its own.
void mode_2dghostrider(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  constexpr int cols = GuDisplay::WIDTH;
  constexpr int rows = GuDisplay::HEIGHT;
  constexpr int kMaxLighters = 64;  // "adequate for 32x32 matrix" per real WLED - this board exactly

  struct Lighter {
    int16_t gPosX, gPosY;
    uint16_t gAngle;
    int8_t angleSpeed;
    uint16_t lightersPosX[kMaxLighters];
    uint16_t lightersPosY[kMaxLighters];
    uint16_t angle[kMaxLighters];
    uint16_t time[kMaxLighters];
    uint8_t reg[kMaxLighters];
    int8_t vSpeed;
  };
  static_assert(sizeof(Lighter) <= State::kDataSize, "Lighter too big");
  auto &l = *reinterpret_cast<Lighter *>(state.data);

  int maxLighters = std::min(cols + rows, kMaxLighters);
  constexpr float kDegToRad = 3.14159265f / 180.0f;

  if (state.aux0 != cols || state.aux1 != rows) {
    state.aux0 = cols;
    state.aux1 = rows;
    l.angleSpeed = static_cast<int8_t>(static_cast<int>(random8(0, 20)) - 10);
    l.gAngle = random16();
    l.vSpeed = 5;
    l.gPosX = static_cast<int16_t>((cols / 2) * 10);
    l.gPosY = static_cast<int16_t>((rows / 2) * 10);
    for (int i = 0; i < maxLighters; i++) {
      l.lightersPosX[i] = static_cast<uint16_t>(l.gPosX);
      l.lightersPosY[i] = static_cast<uint16_t>(l.gPosY + i);
      l.time[i] = static_cast<uint16_t>(i * 2);
      l.reg[i] = 0;
    }
  }

  if (now_ms > state.step) {
    state.step = now_ms + 1024 / (cols + rows);
    fade_to_black_by(frame, static_cast<uint8_t>((p.speed >> 2) + 64));

    wu_pixel(frame, static_cast<uint32_t>(l.gPosX) * 256 / 10, static_cast<uint32_t>(l.gPosY) * 256 / 10,
             Rgb{255, 255, 255});

    l.gPosX = static_cast<int16_t>(l.gPosX + l.vSpeed * sinf(l.gAngle * kDegToRad));
    l.gPosY = static_cast<int16_t>(l.gPosY + l.vSpeed * cosf(l.gAngle * kDegToRad));
    l.gAngle = static_cast<uint16_t>(l.gAngle + l.angleSpeed);
    if (l.gPosX < 0) l.gPosX = static_cast<int16_t>((cols - 1) * 10);
    if (l.gPosX > (cols - 1) * 10) l.gPosX = 0;
    if (l.gPosY < 0) l.gPosY = static_cast<int16_t>((rows - 1) * 10);
    if (l.gPosY > (rows - 1) * 10) l.gPosY = 0;

    for (int i = 0; i < maxLighters; i++) {
      l.time[i] = static_cast<uint16_t>(l.time[i] + random8(5, 20));
      if (l.time[i] >= 255 || l.lightersPosX[i] <= 0 || l.lightersPosX[i] >= static_cast<uint16_t>((cols - 1) * 10) ||
          l.lightersPosY[i] <= 0 || l.lightersPosY[i] >= static_cast<uint16_t>((rows - 1) * 10)) {
        l.reg[i] = 1;
      }
      if (l.reg[i]) {
        l.lightersPosY[i] = static_cast<uint16_t>(l.gPosY);
        l.lightersPosX[i] = static_cast<uint16_t>(l.gPosX);
        l.angle[i] = static_cast<uint16_t>(l.gAngle + (static_cast<int>(random8(20)) - 10));
        l.time[i] = 0;
        l.reg[i] = 0;
      } else {
        l.lightersPosX[i] = static_cast<uint16_t>(l.lightersPosX[i] + static_cast<int>(-7.0f * sinf(l.angle[i] * kDegToRad)));
        l.lightersPosY[i] = static_cast<uint16_t>(l.lightersPosY[i] + static_cast<int>(-7.0f * cosf(l.angle[i] * kDegToRad)));
      }
      Rgb trail = color_from_palette(p.palette_id, static_cast<uint8_t>(256 - l.time[i]), p.primary, p.secondary, p.tertiary);
      wu_pixel(frame, static_cast<uint32_t>(l.lightersPosX[i]) * 256 / 10, static_cast<uint32_t>(l.lightersPosY[i]) * 256 / 10,
               trail);
    }
    blur2d(frame, static_cast<uint8_t>(p.intensity >> 3), static_cast<uint8_t>(p.intensity >> 3), false);
  }
}
EFFECTS_REGISTER(Id::k2dghostrider, mode_2dghostrider)

// wled00/FX.cpp:6286 mode_2Dfloatingblobs().
void mode_2dblobs(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  constexpr int cols = GuDisplay::WIDTH;
  constexpr int rows = GuDisplay::HEIGHT;
  constexpr int kMaxBlobs = 8;

  struct BlobsState {
    float x[kMaxBlobs], y[kMaxBlobs], sX[kMaxBlobs], sY[kMaxBlobs], r[kMaxBlobs];
    uint8_t grow[kMaxBlobs];
    uint8_t color[kMaxBlobs];
  };
  static_assert(sizeof(BlobsState) <= State::kDataSize, "BlobsState too big");
  auto &s = *reinterpret_cast<BlobsState *>(state.data);

  int amount = (p.intensity >> 5) + 1;

  if (state.aux0 != cols || state.aux1 != rows) {
    state.aux0 = cols;
    state.aux1 = rows;
    for (int i = 0; i < kMaxBlobs; i++) {
      s.r[i] = static_cast<float>(random8(1, static_cast<uint8_t>(cols > 8 ? cols / 4 : 2)));
      s.sX[i] = static_cast<float>(random8(3, static_cast<uint8_t>(cols))) / static_cast<float>(256 - p.speed);
      s.sY[i] = static_cast<float>(random8(3, static_cast<uint8_t>(rows))) / static_cast<float>(256 - p.speed);
      s.x[i] = static_cast<float>(random8(0, static_cast<uint8_t>(cols - 1)));
      s.y[i] = static_cast<float>(random8(0, static_cast<uint8_t>(rows - 1)));
      s.color[i] = random8();
      s.grow[i] = s.r[i] < 1.0f ? 1 : 0;
      if (s.sX[i] == 0) s.sX[i] = 1;
      if (s.sY[i] == 0) s.sY[i] = 1;
    }
  }

  fade_to_black_by(frame, static_cast<uint8_t>((p.custom2 >> 3) + 1));

  for (int i = 0; i < amount; i++) {
    if (state.step < now_ms) s.color[i] = static_cast<uint8_t>(s.color[i] + 4);
    float speed = std::max(fabsf(s.sX[i]), fabsf(s.sY[i]));
    if (s.grow[i]) {
      s.r[i] += speed * 0.05f;
      if (s.r[i] >= std::min(cols / 4.0f, 2.0f)) s.grow[i] = 0;
    } else {
      s.r[i] -= speed * 0.05f;
      if (s.r[i] < 1.0f) s.grow[i] = 1;
    }
    Rgb c = color_from_palette(p.palette_id, s.color[i], p.primary, p.secondary, p.tertiary);
    int px = static_cast<int>(roundf(s.x[i]));
    int py = static_cast<int>(roundf(s.y[i]));
    if (s.r[i] > 1.0f) fill_circle(frame, px, py, static_cast<int>(roundf(s.r[i])), c);
    else if (px >= 0 && py >= 0 && px < cols && py < rows) frame[py][px] = c;

    if (s.x[i] + s.r[i] >= cols - 1) s.x[i] += s.sX[i] * ((cols - 1 - s.x[i]) / s.r[i] + 0.005f);
    else if (s.x[i] - s.r[i] <= 0) s.x[i] += s.sX[i] * (s.x[i] / s.r[i] + 0.005f);
    else s.x[i] += s.sX[i];

    if (s.y[i] + s.r[i] >= rows - 1) s.y[i] += s.sY[i] * ((rows - 1 - s.y[i]) / s.r[i] + 0.005f);
    else if (s.y[i] - s.r[i] <= 0) s.y[i] += s.sY[i] * (s.y[i] / s.r[i] + 0.005f);
    else s.y[i] += s.sY[i];

    if (s.x[i] < 0.01f) {
      s.sX[i] = static_cast<float>(random8(3, static_cast<uint8_t>(cols))) / (256 - p.speed);
      s.x[i] = 0.01f;
    } else if (s.x[i] > cols - 1.01f) {
      s.sX[i] = -(static_cast<float>(random8(3, static_cast<uint8_t>(cols))) / (256 - p.speed));
      s.x[i] = cols - 1.01f;
    }
    if (s.y[i] < 0.01f) {
      s.sY[i] = static_cast<float>(random8(3, static_cast<uint8_t>(rows))) / (256 - p.speed);
      s.y[i] = 0.01f;
    } else if (s.y[i] > rows - 1.01f) {
      s.sY[i] = -(static_cast<float>(random8(3, static_cast<uint8_t>(rows))) / (256 - p.speed));
      s.y[i] = rows - 1.01f;
    }
  }
  blur2d(frame, static_cast<uint8_t>(p.custom1 >> 2), static_cast<uint8_t>(p.custom1 >> 2), false);

  if (state.step < now_ms) state.step = now_ms + 2000;
}
EFFECTS_REGISTER(Id::k2dblobs, mode_2dblobs)

// wled00/FX.cpp:6574 mode_2Ddriftrose().
void mode_2ddriftrose(uint32_t now_ms, const Params &p, State &, Frame frame) {
  constexpr int cols = GuDisplay::WIDTH;
  constexpr int rows = GuDisplay::HEIGHT;
  const float cx = (cols - cols % 2) / 2.0f - 0.5f;
  const float cy = (rows - rows % 2) / 2.0f - 0.5f;
  const float l = std::min(cols, rows) / 2.0f;

  fade_to_black_by(frame, static_cast<uint8_t>(32 + (p.speed >> 3)));
  for (int i = 1; i < 37; i++) {
    float angle = i * 10 * (3.14159265f / 180.0f);
    uint32_t x = static_cast<uint32_t>(
        (cx + (sinf(angle) * (beatsin8(now_ms, static_cast<uint8_t>(i), 0, static_cast<uint8_t>(l * 2)) - l))) * 255.0f);
    uint32_t y = static_cast<uint32_t>(
        (cy + (cosf(angle) * (beatsin8(now_ms, static_cast<uint8_t>(i), 0, static_cast<uint8_t>(l * 2)) - l))) * 255.0f);
    Rgb c = (p.palette_id == 0) ? hsv2rgb(static_cast<uint8_t>(i * 10), 255, 255)
                                 : color_from_palette(p.palette_id, static_cast<uint8_t>(i * 10), p.primary, p.secondary,
                                                       p.tertiary);
    wu_pixel(frame, x, y, c);
  }
  blur2d(frame, static_cast<uint8_t>(p.intensity >> 4), static_cast<uint8_t>(p.intensity >> 4), p.option1);
}
EFFECTS_REGISTER(Id::k2ddriftrose, mode_2ddriftrose)

// wled00/FX.cpp:7754 mode_2Ddistortionwaves().
void mode_2ddistortionwaves(uint32_t now_ms, const Params &p, State &, Frame frame) {
  constexpr int cols = GuDisplay::WIDTH;
  constexpr int rows = GuDisplay::HEIGHT;

  uint8_t speed = p.speed / 32;
  uint8_t scale = p.intensity / 32;
  if (p.option2) scale = static_cast<uint8_t>(scale + 192 / (cols + rows));

  unsigned a = now_ms / 32;
  unsigned a2 = a / 2;
  unsigned a3 = a / 3;
  unsigned colsScaled = cols * scale;
  unsigned rowsScaled = rows * scale;

  unsigned cx = beatsin16(now_ms, static_cast<uint8_t>(10 - speed), 0, static_cast<uint16_t>(colsScaled));
  unsigned cy = beatsin16(now_ms, static_cast<uint8_t>(12 - speed), 0, static_cast<uint16_t>(rowsScaled));
  unsigned cx1 = beatsin16(now_ms, static_cast<uint8_t>(13 - speed), 0, static_cast<uint16_t>(colsScaled));
  unsigned cy1 = beatsin16(now_ms, static_cast<uint8_t>(15 - speed), 0, static_cast<uint16_t>(rowsScaled));
  unsigned cx2 = beatsin16(now_ms, static_cast<uint8_t>(17 - speed), 0, static_cast<uint16_t>(colsScaled));
  unsigned cy2 = beatsin16(now_ms, static_cast<uint8_t>(14 - speed), 0, static_cast<uint16_t>(rowsScaled));

  unsigned xoffs = 0;
  for (int x = 0; x < cols; x++) {
    xoffs += scale;
    unsigned yoffs = 0;
    for (int y = 0; y < rows; y++) {
      yoffs += scale;

      uint8_t rdistort, gdistort, bdistort;
      if (p.option3) {
        rdistort = static_cast<uint8_t>(cos8(static_cast<uint8_t>((x + y) * 8 + a2)) >> 1);
        gdistort = static_cast<uint8_t>(cos8(static_cast<uint8_t>((x + y) * 8 + a3 + 32)) >> 1);
        bdistort = static_cast<uint8_t>(cos8(static_cast<uint8_t>((x + y) * 8 + a + 64)) >> 1);
      } else {
        rdistort = static_cast<uint8_t>(
            cos8(static_cast<uint8_t>(cos8(static_cast<uint8_t>((x << 3) + a)) + cos8(static_cast<uint8_t>((y << 3) - a2)) + a3)) >> 1);
        gdistort = static_cast<uint8_t>(
            cos8(static_cast<uint8_t>(cos8(static_cast<uint8_t>((x << 3) - a2)) + cos8(static_cast<uint8_t>((y << 3) + a3)) + a + 32)) >> 1);
        bdistort = static_cast<uint8_t>(
            cos8(static_cast<uint8_t>(cos8(static_cast<uint8_t>((x << 3) + a3)) + cos8(static_cast<uint8_t>((y << 3) - a)) + a2 + 64)) >> 1);
      }

      int valueR = rdistort + (static_cast<int>(a - (((xoffs - cx) * (xoffs - cx) + (yoffs - cy) * (yoffs - cy)) >> 7)) << 1);
      int valueG = gdistort + (static_cast<int>(a2 - (((xoffs - cx1) * (xoffs - cx1) + (yoffs - cy1) * (yoffs - cy1)) >> 7)) << 1);
      int valueB = bdistort + (static_cast<int>(a3 - (((xoffs - cx2) * (xoffs - cx2) + (yoffs - cy2) * (yoffs - cy2)) >> 7)) << 1);

      uint8_t vr = cos8(static_cast<uint8_t>(valueR));
      uint8_t vg = cos8(static_cast<uint8_t>(valueG));
      uint8_t vb = cos8(static_cast<uint8_t>(valueB));

      if (p.palette_id == 0) {
        frame[y][x] = Rgb{vr, vg, vb};
      } else {
        uint8_t brightness = static_cast<uint8_t>((vr + vg + vb) / 3);
        if (p.option1) {
          frame[y][x] = color_from_palette(p.palette_id, brightness, p.primary, p.secondary, p.tertiary);
        } else {
          uint8_t hue = rgb_to_hue(static_cast<uint8_t>(vr >> 2), static_cast<uint8_t>(vg >> 2), static_cast<uint8_t>(vb >> 2));
          frame[y][x] = scale_rgb(color_from_palette(p.palette_id, hue, p.primary, p.secondary, p.tertiary), brightness);
        }
      }
    }
  }

  if (!p.option1 && p.palette_id) blur2d(frame, 200, 200, true);
}
EFFECTS_REGISTER(Id::k2ddistortionwaves, mode_2ddistortionwaves)

// wled00/FX.cpp:7834 soapPixels(), the shared helper wled00/FX.cpp:7886
// mode_2Dsoap() calls twice (once per axis). Real WLED keeps a *second*
// full-frame CRGB copy in SEGENV.data purely so this can read "the frame
// before this pass touched it"; combined with the per-pixel noise map
// that's ~4KB here, four times State::kDataSize. This port reads/writes
// the shared persistent `frame` buffer directly instead (it already
// persists across calls exactly like WLED's own segment buffer - see
// effects.cpp's render()), keeping only the noise map itself in
// state.data.
void soap_pixels(bool is_row, const uint8_t *noise3d, const Params &p, Frame frame) {
  constexpr int cols = GuDisplay::WIDTH;
  constexpr int rows = GuDisplay::HEIGHT;
  const auto XY = [&](int x, int y) { return x + y * cols; };
  const int tRC = is_row ? rows : cols;
  const int tCR = is_row ? cols : rows;
  const int amplitude = std::max(1, (tCR - 8) >> 3) * (1 + (p.custom1 >> 5));

  Rgb ledsbuff[GuDisplay::WIDTH];  // tCR is WIDTH or HEIGHT, equal on this board

  for (int i = 0; i < tRC; i++) {
    int amount = (static_cast<int>(noise3d[is_row ? i * cols : i]) - 128) * amplitude;
    int delta = iabs(amount) >> 8;
    int fraction = iabs(amount) & 255;
    for (int j = 0; j < tCR; j++) {
      int zD = amount < 0 ? j - delta : j + delta;
      int zF = amount < 0 ? zD - 1 : zD + 1;
      int yA = iabs(zD) % tCR;
      int yB = iabs(zF) % tCR;
      int xA = i, xB = i;
      if (is_row) {
        std::swap(xA, yA);
        std::swap(xB, yB);
      }
      Rgb pixelA = (zD >= 0 && zD < tCR)
                       ? frame[yA][xA]
                       : color_from_palette(p.palette_id, static_cast<uint8_t>(~noise3d[XY(xA, yA)] * 3), p.primary,
                                             p.secondary, p.tertiary);
      Rgb pixelB = (zF >= 0 && zF < tCR)
                       ? frame[yB][xB]
                       : color_from_palette(p.palette_id, static_cast<uint8_t>(~noise3d[XY(xB, yB)] * 3), p.primary,
                                             p.secondary, p.tertiary);
      ledsbuff[j] = add_rgb_sat(scale_rgb(pixelA, ease8InOutCubic(static_cast<uint8_t>(255 - fraction))),
                                 scale_rgb(pixelB, ease8InOutCubic(static_cast<uint8_t>(fraction))));
    }
    for (int j = 0; j < tCR; j++) {
      if (is_row) frame[i][j] = ledsbuff[j];
      else frame[j][i] = ledsbuff[j];
    }
  }
}

// wled00/FX.cpp:7886 mode_2Dsoap(). The three signed 32-bit noise
// coordinates real WLED walks independently are derived from a single
// accumulator in state.step instead (all three there just add the same
// per-frame `mov` to their own random starting offset anyway - fixed
// constant offsets stand in for the random ones with no visible
// difference), freeing all of state.data for the noise map (cols*rows
// bytes, exactly filling the 1024-byte budget - see soap_pixels() above
// for where the pixel buffer went).
void mode_2dsoap(uint32_t, const Params &p, State &state, Frame frame) {
  constexpr int cols = GuDisplay::WIDTH;
  constexpr int rows = GuDisplay::HEIGHT;
  struct SoapState {
    uint8_t noise3d[cols * rows];
  };
  static_assert(sizeof(SoapState) <= State::kDataSize, "SoapState too big");
  auto &s = *reinterpret_cast<SoapState *>(state.data);
  const auto XY = [&](int x, int y) { return x + y * cols; };

  constexpr uint32_t scale32_x = 160000u / cols;
  constexpr uint32_t scale32_y = 160000u / rows;
  const uint32_t mov = static_cast<uint32_t>(std::min(cols, rows)) * (p.speed + 2) / 2;
  const uint8_t smoothness = static_cast<uint8_t>(std::min<int>(250, p.intensity));

  uint32_t base = state.step;

  for (int i = 0; i < cols; i++) {
    int32_t ioffset = static_cast<int32_t>(scale32_x) * (i - cols / 2);
    for (int j = 0; j < rows; j++) {
      int32_t joffset = static_cast<int32_t>(scale32_y) * (j - rows / 2);
      uint8_t data = static_cast<uint8_t>(value_noise16(static_cast<int32_t>(base) + ioffset,
                                                          static_cast<int32_t>(base + 12345u) + joffset,
                                                          static_cast<int32_t>(base + 54321u)) >>
                                           8);
      s.noise3d[XY(i, j)] =
          static_cast<uint8_t>(scale8(s.noise3d[XY(i, j)], smoothness) + scale8(data, static_cast<uint8_t>(255 - smoothness)));
    }
  }

  if (state.call == 0) {
    for (int i = 0; i < cols; i++)
      for (int j = 0; j < rows; j++)
        frame[j][i] = color_from_palette(p.palette_id, static_cast<uint8_t>(~s.noise3d[XY(i, j)] * 3), p.primary,
                                          p.secondary, p.tertiary);
  }

  state.step = base + mov;

  soap_pixels(true, s.noise3d, p, frame);
  soap_pixels(false, s.noise3d, p, frame);
}
EFFECTS_REGISTER(Id::k2dsoap, mode_2dsoap)

// wled00/FX.cpp:7937 mode_2Doctopus(). Real WLED caches a per-pixel
// angle/radius map (2 bytes * 32 * 32 = 2048 bytes) in SEGENV.data to
// avoid recomputing atan2/sqrt every frame; that's twice this board's
// whole State::kDataSize budget, so this port recomputes angle/radius
// per pixel every frame instead - same output, more CPU per frame, no
// cross-frame cache.
void mode_2doctopus(uint32_t, const Params &p, State &state, Frame frame) {
  constexpr int cols = GuDisplay::WIDTH;
  constexpr int rows = GuDisplay::HEIGHT;
  const uint8_t mapp = static_cast<uint8_t>(180 / std::max(cols, rows));
  const int cX = cols / 2 + ((static_cast<int>(p.custom1) - 128) * cols) / 255;
  const int cY = rows / 2 + ((static_cast<int>(p.custom2) - 128) * rows) / 255;

  state.step += p.speed / 32 + 1;

  for (int x = 0; x < cols; x++) {
    for (int y = 0; y < rows; y++) {
      int dx = x - cX;
      int dy = y - cY;
      uint8_t angle = static_cast<uint8_t>(static_cast<int>(40.7436f * atan2f(static_cast<float>(dy), static_cast<float>(dx))));
      uint8_t radius = static_cast<uint8_t>(sqrtf(static_cast<float>(dx * dx + dy * dy)) * mapp);
      uint8_t bright = sin8(static_cast<uint8_t>(
          sin8(static_cast<uint8_t>((angle * 4 - radius) / 4 + state.step / 2)) + radius - state.step + angle * (p.custom3 / 4 + 1)));
      Rgb c = color_from_palette(p.palette_id, static_cast<uint8_t>(state.step / 2 - radius), p.primary, p.secondary, p.tertiary);
      frame[y][x] = scale_rgb(c, bright);
    }
  }
}
EFFECTS_REGISTER(Id::k2doctopus, mode_2doctopus)

// wled00/FX.cpp:7994 mode_2Dwavingcell().
void mode_2dwavingcell(uint32_t now_ms, const Params &p, State &, Frame frame) {
  constexpr int cols = GuDisplay::WIDTH;
  constexpr int rows = GuDisplay::HEIGHT;
  uint32_t t = (now_ms * (p.speed + 1)) >> 3;
  uint32_t aX = p.custom1 / 16 + 9;
  uint32_t aY = p.custom2 / 16 + 1;
  uint32_t aZ = p.custom3 + 1;
  for (int x = 0; x < cols; x++) {
    for (int y = 0; y < rows; y++) {
      uint32_t wave = sin8(static_cast<uint8_t>(x * aX + sin8(static_cast<uint8_t>((((y << 8) + t) * aY) >> 8)))) +
                      cos8(static_cast<uint8_t>(y * aZ));
      uint8_t colorIndex = static_cast<uint8_t>(wave + (t >> (8 - (p.option2 * 3))));
      frame[y][x] = color_from_palette(p.palette_id, colorIndex, p.primary, p.secondary, p.tertiary);
    }
  }
  blur2d(frame, p.intensity, p.intensity, false);
}
EFFECTS_REGISTER(Id::k2dwavingcell, mode_2dwavingcell)

}  // namespace
}  // namespace effects
