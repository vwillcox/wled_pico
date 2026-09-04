#include "effects.h"

#include <math.h>

#include "display/gu_display.h"

namespace effects {

void State::reset() {
  step = 0;
  call = 0;
  aux0 = 0;
  aux1 = 0;
  for (auto &b : data) b = 0;
}

namespace {
ModeFn g_table[static_cast<size_t>(Id::kCount)] = {nullptr};
}  // namespace

void register_mode(Id id, ModeFn fn) { g_table[static_cast<size_t>(id)] = fn; }

// Display names, real WLED's exactly, indexed by Id (see effects.h's top
// comment for why the numbering matches real WLED and why unimplemented
// IDs say "RSVD" specifically). Source: wled/WLED's wled00/FX.cpp's
// per-effect `_data_FX_MODE_*` strings (the part before the first '@').
constexpr const char *kNames[static_cast<size_t>(Id::kCount)] = {
    "Solid", "Blink", "Breathe", "Wipe", "Wipe Random", "Random Colors", "Sweep", "Dynamic",
    "Colorloop", "Rainbow", "Scan", "Scan Dual", "Fade", "Theater", "Theater Rainbow", "Running",
    "Saw", "Twinkle", "Dissolve", "Dissolve Rnd", "Sparkle", "Sparkle Dark", "Sparkle+", "Strobe",
    "Strobe Rainbow", "Strobe Mega", "Blink Rainbow", "Android", "Chase", "Chase Random",
    "Chase Rainbow", "Chase Flash", "Chase Flash Rnd", "Rainbow Runner", "Colorful",
    "Traffic Light", "Sweep Random", "Chase 2", "Aurora", "Stream", "Scanner", "Lighthouse",
    "Fireworks", "Rain", "Tetrix", "Fire Flicker", "Gradient", "Loading", "Rolling Balls",
    "Fairy", "Two Dots", "Fairytwinkle", "Running Dual", "Image", "Chase 3", "Tri Wipe",
    "Tri Fade", "Lightning", "ICU", "Multi Comet", "Scanner Dual", "Stream 2", "Oscillate",
    "Pride 2015", "Juggle", "Palette", "Fire 2012", "Colorwaves", "Bpm", "Fill Noise", "Noise 1",
    "Noise 2", "Noise 3", "Noise 4", "Colortwinkles", "Lake", "Meteor", "Copy Segment", "Railway",
    "Ripple", "Twinklefox", "Twinklecat", "Halloween Eyes", "Solid Pattern",
    "Solid Pattern Tri", "Spots", "Spots Fade", "Glitter", "Candle", "Fireworks Starburst",
    "Fireworks 1D", "Bouncing Balls", "Sinelon", "Sinelon Dual", "Sinelon Rainbow", "Popcorn",
    "Drip", "Plasma", "Percent", "Ripple Rainbow", "Heartbeat", "Pacifica", "Candle Multi",
    "Solid Glitter", "Sunrise", "Phased", "Twinkleup", "Noise Pal", "Sine", "Phased Noise",
    "Flow", "Chunchun", "Dancing Shadows", "Washing Machine", "Rotozoomer", "Blends",
    "TV Simulator", "Dynamic Smooth", "Spaceships", "Crazy Bees", "Ghost Rider", "Blobs",
    "Scrolling Text", "Drift Rose", "Distortion Waves", "Soap", "Octopus", "Waving Cell",
    "Pixels", "Pixelwave", "Juggles", "Matripix", "Gravimeter", "Plasmoid", "Puddles",
    "Midnoise", "Noisemeter", "Freqwave", "Freqmatrix", "GEQ", "Waterfall", "Freqpixels",
    "RSVD", "Noisefire", "Puddlepeak", "Noisemove", "Noise2D", "Perlin Move", "Ripple Peak",
    "Firenoise", "Squared Swirl", "PacMan", "DNA", "Matrix", "Metaballs", "Freqmap",
    "Gravcenter", "Gravcentric", "Gravfreq", "DJ Light", "Funky Plank", "Shimmer", "Pulser",
    "Blurz", "Drift", "Waverly", "Sun Radiation", "Colored Bursts", "Julia", "RSVD", "RSVD",
    "RSVD", "Game Of Life", "Tartan", "Polar Lights", "Swirl", "Lissajous", "Frizzles",
    "Plasma Ball", "Flow Stripe", "Hiphotic", "Sindots", "DNA Spiral", "Black Hole", "Wavesins",
    "Rocktaves", "Akemi", "PS Volcano", "PS Fire", "PS Fireworks", "PS Vortex", "PS Fuzzy Noise",
    "PS Ballpit", "PS Box", "PS Attractor", "PS Impact", "PS Waterfall", "PS Spray",
    "PS GEQ 2D", "PS GEQ Nova", "PS Ghost Rider", "PS Blobs", "PS DripDrop", "PS Pinball",
    "PS Dancing Shadows", "PS Fireworks 1D", "PS Sparkler", "PS Hourglass", "PS Spray 1D",
    "PS 1D Balance", "PS Chase", "PS Starburst", "PS GEQ 1D", "PS Fire 1D", "PS Sonic Stream",
    "PS Sonic Boom", "PS Springy", "PS Galaxy", "Color Clouds", "Slow Transition",
};
static_assert(sizeof(kNames) / sizeof(kNames[0]) == static_cast<size_t>(Id::kCount),
              "kNames must have one entry per Id");

const char *name(Id id) { return kNames[static_cast<size_t>(id)]; }

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

void fill_column(Frame frame, int x, const Rgb &c) {
  for (int y = 0; y < GuDisplay::HEIGHT; y++) frame[y][x] = c;
}

// wled00/FX.cpp:136 mode_static() - SEGMENT.fill(SEGCOLOR(0))
void mode_static(uint32_t, const Params &p, State &, Frame frame) {
  for (int x = 0; x < GuDisplay::WIDTH; x++) fill_column(frame, x, p.primary);
}
EFFECTS_REGISTER(Id::kStatic, mode_static)

// wled00/FX.cpp:262 color_wipe(rev=false, useRandomColors=false), as used
// by mode_color_wipe(). The useRandomColors branch (SEGENV.step/aux0/aux1)
// is dropped since our default wipe never takes it - everything else,
// including the ledIndex/rem timing math, is unchanged.
void mode_color_wipe(uint32_t now_ms, const Params &p, State &, Frame frame) {
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
    fill_column(frame, i, px);
  }
}
EFFECTS_REGISTER(Id::kColorWipe, mode_color_wipe)

// wled00/FX.cpp:530 mode_rainbow_cycle()
void mode_rainbow_cycle(uint32_t now_ms, const Params &p, State &, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  uint32_t counter = (now_ms * ((p.speed >> 2) + 2)) & 0xFFFFu;
  counter >>= 8;
  for (int i = 0; i < width; i++) {
    uint8_t index = static_cast<uint8_t>(
        (static_cast<uint32_t>(i) * (16u << (p.intensity / 29)) / width) + counter);
    fill_column(frame, i, color_wheel(index));
  }
}
EFFECTS_REGISTER(Id::kRainbowCycle, mode_rainbow_cycle)

// wled00/FX.cpp:432 mode_breath(). sin16_t (WLED/FastLED's Q15 fixed-point
// sine, input 0-65535 -> output roughly -32767..32767) is replaced with
// sinf() over the same domain - matches the shape, not bit-exact.
void mode_breath(uint32_t now_ms, const Params &p, State &, Frame frame) {
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
  for (int x = 0; x < GuDisplay::WIDTH; x++) fill_column(frame, x, color);
}
EFFECTS_REGISTER(Id::kBreath, mode_breath)

}  // namespace

void render(Id id, GuDisplay &display, uint32_t now_ms, const Params &params, State &state) {
  static Rgb frame[GuDisplay::HEIGHT][GuDisplay::WIDTH];

  // `frame` is one shared buffer reused across every effect (state.reset()
  // only zeroes State::data, not this) - effects that read-then-fade their
  // own previous frame (trails, sparkle decay) need a genuinely blank
  // canvas the moment they're selected, not whatever the last effect left
  // behind. Matches real WLED's segment pixel buffer starting blank.
  if (state.call == 0) {
    for (auto &row : frame)
      for (auto &px : row) px = Rgb{0, 0, 0};
  }

  ModeFn fn = g_table[static_cast<size_t>(id)];
  if (fn) {
    fn(now_ms, params, state, frame);
  } else {
    // No ported effect for this ID yet (or it's a real WLED "RSVD" slot) -
    // fall back to Solid rather than showing whatever the previous effect
    // left in the frame buffer.
    for (int x = 0; x < GuDisplay::WIDTH; x++) fill_column(frame, x, params.primary);
  }

  for (int y = 0; y < GuDisplay::HEIGHT; y++) {
    for (int x = 0; x < GuDisplay::WIDTH; x++) {
      const Rgb &c = frame[y][x];
      display.set_pixel(x, y, c.r, c.g, c.b);
    }
  }

  state.call++;
}

}  // namespace effects
