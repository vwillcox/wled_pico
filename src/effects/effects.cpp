#include "effects.h"

#include <math.h>

#include "display/gu_display.h"

namespace effects {
namespace {

uint8_t clamp_u8(int v) {
  if (v < 0) return 0;
  if (v > 255) return 255;
  return static_cast<uint8_t>(v);
}

// WLED's color_blend(c1, c2, amount): blend from a to b by amount/255.
// wled00/FX_fcn.cpp
Rgb blend(const Rgb &a, const Rgb &b, uint8_t amount) {
  return Rgb{
      clamp_u8(a.r + (static_cast<int>(b.r) - a.r) * amount / 255),
      clamp_u8(a.g + (static_cast<int>(b.g) - a.g) * amount / 255),
      clamp_u8(a.b + (static_cast<int>(b.b) - a.b) * amount / 255),
  };
}

// WLED's Segment::color_wheel(): CHSV32(pos << 8, 255, 255) - a plain
// full-saturation hue wheel, no palette involved. wled00/FX_fcn.cpp:1141
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

// wled00/FX.cpp:136 mode_static() - SEGMENT.fill(SEGCOLOR(0))
void solid(uint32_t, const Params &p, Rgb *out, int width) {
  for (int i = 0; i < width; i++) out[i] = p.primary;
}

// wled00/FX.cpp:262 color_wipe(rev=false, useRandomColors=false), as used
// by mode_color_wipe(). The useRandomColors branch (SEGENV.step/aux0/aux1)
// is dropped since our default wipe never takes it - everything else,
// including the ledIndex/rem timing math, is unchanged.
void wipe(uint32_t now_ms, const Params &p, Rgb *out, int width) {
  uint32_t cycle_time = 750 + (255 - p.speed) * 150;
  uint32_t perc = now_ms % cycle_time;
  uint32_t prog = (perc * 65535) / cycle_time;
  bool back = prog > 32767;
  if (back) prog -= 32767;

  int led_index = static_cast<int>((prog * static_cast<uint32_t>(width)) >> 15);
  uint32_t rem = (prog * static_cast<uint32_t>(width) * 2) / (p.intensity + 1);
  if (rem > 255) rem = 255;

  for (int i = 0; i < width; i++) {
    Rgb px;
    if (i < led_index) {
      px = back ? p.secondary : p.primary;
    } else {
      px = back ? p.primary : p.secondary;
      if (i == led_index) {
        px = blend(back ? p.primary : p.secondary,
                    back ? p.secondary : p.primary,
                    static_cast<uint8_t>(rem));
      }
    }
    out[i] = px;
  }
}

// wled00/FX.cpp:530 mode_rainbow_cycle()
void rainbow(uint32_t now_ms, const Params &p, Rgb *out, int width) {
  uint32_t counter = (now_ms * ((p.speed >> 2) + 2)) & 0xFFFFu;
  counter >>= 8;
  for (int i = 0; i < width; i++) {
    uint8_t index = static_cast<uint8_t>(
        (static_cast<uint32_t>(i) * (16u << (p.intensity / 29)) / width) + counter);
    out[i] = color_wheel(index);
  }
}

// wled00/FX.cpp:432 mode_breath(). sin16_t (WLED/FastLED's Q15 fixed-point
// sine, input 0-65535 -> output roughly -32767..32767) is replaced with
// sinf() over the same domain - matches the shape, not bit-exact.
void breathe(uint32_t now_ms, const Params &p, Rgb *out, int width) {
  uint32_t counter = (now_ms * ((p.speed >> 3) + 10)) & 0xFFFFu;
  counter = (counter >> 2) + (counter >> 4);  // 0-16384 + 0-2048

  int var = 0;
  if (counter < 16384) {
    if (counter > 8192) counter = 8192 - (counter - 8192);
    float rad = counter * (2.0f * static_cast<float>(M_PI) / 65536.0f);
    var = static_cast<int>((sinf(rad) * 32767.0f) / 103.0f);
  }
  uint8_t lum = clamp_u8(30 + var);

  Rgb color = blend(p.secondary, p.primary, lum);
  for (int i = 0; i < width; i++) out[i] = color;
}

}  // namespace

void render(Id id, GuDisplay &display, uint32_t now_ms, const Params &params) {
  Rgb columns[GuDisplay::WIDTH];

  switch (id) {
    case Id::kSolid:   solid(now_ms, params, columns, GuDisplay::WIDTH); break;
    case Id::kWipe:    wipe(now_ms, params, columns, GuDisplay::WIDTH); break;
    case Id::kRainbow: rainbow(now_ms, params, columns, GuDisplay::WIDTH); break;
    case Id::kBreathe: breathe(now_ms, params, columns, GuDisplay::WIDTH); break;
    default:           solid(now_ms, params, columns, GuDisplay::WIDTH); break;
  }

  for (int x = 0; x < GuDisplay::WIDTH; x++) {
    const Rgb &c = columns[x];
    for (int y = 0; y < GuDisplay::HEIGHT; y++) {
      display.set_pixel(x, y, c.r, c.g, c.b);
    }
  }
}

const char *name(Id id) {
  switch (id) {
    case Id::kSolid:   return "Solid";
    case Id::kWipe:    return "Wipe";
    case Id::kRainbow: return "Rainbow";
    case Id::kBreathe: return "Breathe";
    default:           return "?";
  }
}

}  // namespace effects
