#pragma once

#include <cstddef>
#include <cstdint>

#include "display/gu_display.h"

// Milestone 3 gave this file WLED's effect math, ported off its Segment/
// SEGENV machinery onto a flat per-column buffer - the four effects there
// were all genuinely stateless functions of time, so that's all they
// needed. The "port every hardware-feasible WLED effect" push after that
// needs more: persistent per-effect state (State - WLED's SEGENV), a real
// per-pixel framebuffer instead of a column broadcast (Frame - a chunk of
// WLED's effect list is 2D-native, addressing (x,y) individually), and,
// since the target list runs to ~160 effects across many separately-landed
// batches, a way to add one without editing a shared dispatch file every
// time (register_mode() + EFFECTS_REGISTER, below).
//
// Effect IDs match real WLED's FX_MODE_* numbering exactly (see
// wled/WLED's wled00/FX.h) rather than being assigned in porting order -
// so this firmware's /json "fx" field means the same thing a real WLED
// device's does, and anything with real WLED's IDs memorized (docs,
// scripts, saved presets from an actual WLED device) is correct against
// this firmware too. IDs with no ported effect report as "RSVD" (see
// name()) - the same thing real WLED itself does for retired/reserved
// IDs, which is also why WLED client libraries (Home Assistant's
// python-wled included) already know to filter that name out.
namespace effects {

struct Rgb {
  uint8_t r, g, b;
};

enum class Id : uint8_t {
  kStatic = 0,
  kBlink = 1,
  kBreath = 2,
  kColorWipe = 3,
  kColorWipeRandom = 4,
  kRandomColor = 5,
  kColorSweep = 6,
  kDynamic = 7,
  kRainbow = 8,
  kRainbowCycle = 9,
  kScan = 10,
  kDualScan = 11,
  kFade = 12,
  kTheaterChase = 13,
  kTheaterChaseRainbow = 14,
  kRunningLights = 15,
  kSaw = 16,
  kTwinkle = 17,
  kDissolve = 18,
  kDissolveRandom = 19,
  kSparkle = 20,
  kFlashSparkle = 21,
  kHyperSparkle = 22,
  kStrobe = 23,
  kStrobeRainbow = 24,
  kMultiStrobe = 25,
  kBlinkRainbow = 26,
  kAndroid = 27,
  kChaseColor = 28,
  kChaseRandom = 29,
  kChaseRainbow = 30,
  kChaseFlash = 31,
  kChaseFlashRandom = 32,
  kChaseRainbowWhite = 33,
  kColorful = 34,
  kTrafficLight = 35,
  kColorSweepRandom = 36,
  kRunningColor = 37,
  kAurora = 38,
  kRunningRandom = 39,
  kLarsonScanner = 40,
  kComet = 41,
  kFireworks = 42,
  kRain = 43,
  kTetrix = 44,
  kFireFlicker = 45,
  kGradient = 46,
  kLoading = 47,
  kRollingballs = 48,
  kFairy = 49,
  kTwoDots = 50,
  kFairytwinkle = 51,
  kRunningDual = 52,
  kImage = 53,
  kTricolorChase = 54,
  kTricolorWipe = 55,
  kTricolorFade = 56,
  kLightning = 57,
  kIcu = 58,
  kMultiComet = 59,
  kDualLarsonScanner = 60,
  kRandomChase = 61,
  kOscillate = 62,
  kPride2015 = 63,
  kJuggle = 64,
  kPalette = 65,
  kFire2012 = 66,
  kColorwaves = 67,
  kBpm = 68,
  kFillnoise8 = 69,
  kNoise161 = 70,
  kNoise162 = 71,
  kNoise163 = 72,
  kNoise164 = 73,
  kColortwinkle = 74,
  kLake = 75,
  kMeteor = 76,
  kCopy = 77,
  kRailway = 78,
  kRipple = 79,
  kTwinklefox = 80,
  kTwinklecat = 81,
  kHalloweenEyes = 82,
  kStaticPattern = 83,
  kTriStaticPattern = 84,
  kSpots = 85,
  kSpotsFade = 86,
  kGlitter = 87,
  kCandle = 88,
  kStarburst = 89,
  kExplodingFireworks = 90,
  kBouncingballs = 91,
  kSinelon = 92,
  kSinelonDual = 93,
  kSinelonRainbow = 94,
  kPopcorn = 95,
  kDrip = 96,
  kPlasma = 97,
  kPercent = 98,
  kRippleRainbow = 99,
  kHeartbeat = 100,
  kPacifica = 101,
  kCandleMulti = 102,
  kSolidGlitter = 103,
  kSunrise = 104,
  kPhased = 105,
  kTwinkleup = 106,
  kNoisepal = 107,
  kSinewave = 108,
  kPhasednoise = 109,
  kFlow = 110,
  kChunchun = 111,
  kDancingShadows = 112,
  kWashingMachine = 113,
  k2dplasmarotozoom = 114,
  kBlends = 115,
  kTvSimulator = 116,
  kDynamicSmooth = 117,
  k2dspaceships = 118,
  k2dcrazybees = 119,
  k2dghostrider = 120,
  k2dblobs = 121,
  k2dscrolltext = 122,
  k2ddriftrose = 123,
  k2ddistortionwaves = 124,
  k2dsoap = 125,
  k2doctopus = 126,
  k2dwavingcell = 127,
  kPixels = 128,
  kPixelwave = 129,
  kJuggles = 130,
  kMatripix = 131,
  kGravimeter = 132,
  kPlasmoid = 133,
  kPuddles = 134,
  kMidnoise = 135,
  kNoisemeter = 136,
  kFreqwave = 137,
  kFreqmatrix = 138,
  k2dgeq = 139,
  kWaterfall = 140,
  kFreqpixels = 141,
  kBinmap = 142,
  kNoisefire = 143,
  kPuddlepeak = 144,
  kNoisemove = 145,
  k2dnoise = 146,
  kPerlinmove = 147,
  kRipplepeak = 148,
  k2dfirenoise = 149,
  k2dsquaredswirl = 150,
  kPacman = 151,
  k2ddna = 152,
  k2dmatrix = 153,
  k2dmetaballs = 154,
  kFreqmap = 155,
  kGravcenter = 156,
  kGravcentric = 157,
  kGravfreq = 158,
  kDjlight = 159,
  k2dfunkyplank = 160,
  kShimmer = 161,
  k2dpulser = 162,
  kBlurz = 163,
  k2ddrift = 164,
  k2dwaverly = 165,
  k2dsunradiation = 166,
  k2dcoloredbursts = 167,
  k2djulia = 168,
  kRsvd169 = 169,
  kRsvd170 = 170,
  kRsvd171 = 171,
  k2dgameoflife = 172,
  k2dtartan = 173,
  k2dpolarlights = 174,
  k2dswirl = 175,
  k2dlissajous = 176,
  k2dfrizzles = 177,
  k2dplasmaball = 178,
  kFlowstripe = 179,
  k2dhiphotic = 180,
  k2dsindots = 181,
  k2ddnaspiral = 182,
  k2dblackhole = 183,
  kWavesins = 184,
  kRocktaves = 185,
  k2dakemi = 186,
  kParticlevolcano = 187,
  kParticlefire = 188,
  kParticlefireworks = 189,
  kParticlevortex = 190,
  kParticleperlin = 191,
  kParticlepit = 192,
  kParticlebox = 193,
  kParticleattractor = 194,
  kParticleimpact = 195,
  kParticlewaterfall = 196,
  kParticlespray = 197,
  kParticlesgeq = 198,
  kParticlecentergeq = 199,
  kParticleghostrider = 200,
  kParticleblobs = 201,
  kPsdrip = 202,
  kPspinball = 203,
  kPsdancingshadows = 204,
  kPsfireworks1d = 205,
  kPssparkler = 206,
  kPshourglass = 207,
  kPs1dspray = 208,
  kPsbalance = 209,
  kPschase = 210,
  kPsstarburst = 211,
  kPs1dgeq = 212,
  kPsfire1d = 213,
  kPs1dsonicstream = 214,
  kPs1dsonicboom = 215,
  kPs1dspringy = 216,
  kParticlegalaxy = 217,
  kColorclouds = 218,
  kSlowTransition = 219,
  kCount = 220,
};

struct Params {
  uint8_t speed = 128;      // matches SEGMENT.speed, 0-255
  uint8_t intensity = 128;  // matches SEGMENT.intensity, 0-255
  uint8_t custom1 = 128;    // matches SEGMENT.custom1, 0-255
  uint8_t custom2 = 128;    // matches SEGMENT.custom2, 0-255
  uint8_t custom3 = 16;     // matches SEGMENT.custom3, 0-31 in real WLED (kept 0-255 range here)
  bool option1 = false;     // matches SEGMENT.check1
  bool option2 = false;     // matches SEGMENT.check2
  bool option3 = false;     // matches SEGMENT.check3
  Rgb primary{255, 0, 0};   // matches SEGCOLOR(0)
  Rgb secondary{0, 0, 0};   // matches SEGCOLOR(1)
  Rgb tertiary{0, 0, 0};    // matches SEGCOLOR(2)
  uint8_t palette_id = 0;   // matches SEGMENT.palette, see palettes.h

  // Matches SEGMENT.name (real WLED's per-segment display name field,
  // JSON "n") - dual-purposed by real WLED's Scrolling Text effect as the
  // text to display (see gen_scrolltext.cpp), the same repurposing real
  // WLED itself does rather than a dedicated text field.
  static constexpr size_t kNameSize = 33;
  char name[kNameSize] = "WLED PICO";
};

// Per-effect persistent state, real WLED's SEGENV trimmed to the
// single-segment case this firmware has. `call` is a frame counter that
// starts at 0 each time an effect is (re)selected - effects that need
// one-time setup check `call == 0`, matching SEGENV.call's same role in
// WLED. `data` is generic scratch space an effect reinterpret_casts into
// its own struct, matching SEGENV.data (WLED's SEGMENT.allocateData<T>()) -
// sized to comfortably fit the largest per-effect struct expected to land
// here (a handful of uint16_t/uint32_t arrays sized to the 32x32 board,
// not WLED's own much larger max strip length).
struct State {
  uint32_t step = 0;
  uint32_t call = 0;
  uint16_t aux0 = 0;
  uint16_t aux1 = 0;
  static constexpr size_t kDataSize = 1024;
  uint8_t data[kDataSize] = {0};

  // Called whenever the selected effect ID changes - matches WLED zeroing
  // SEGENV.data and resetting SEGENV.call on a mode switch.
  void reset();
};

// One full frame buffer, addressed frame[y][x] - every mode_ function
// writes into one of these; render() blits it to the display in one pass
// afterward. A de facto 1D effect (the majority) just writes the same
// color down an entire column; a 2D-native one addresses (x,y) directly.
using Frame = Rgb[GuDisplay::HEIGHT][GuDisplay::WIDTH];

// Writes `c` down every row of column `x` - the broadcast every de facto
// 1D effect (the majority of what's ported here) needs, now that each one
// addresses the full 2D `frame` instead of a 32-wide columns[] array.
inline void fill_column(Frame frame, int x, Rgb c) {
  for (int y = 0; y < GuDisplay::HEIGHT; y++) frame[y][x] = c;
}

using ModeFn = void (*)(uint32_t now_ms, const Params &params, State &state, Frame frame);

// Registers `fn` as the renderer for effect `id` - real WLED's addEffect(),
// same idea, minus the ability to also carry a UI metadata string (this
// firmware doesn't have real WLED's effect-parameter UI). Effects call
// this via the EFFECTS_REGISTER macro below rather than calling it
// directly - see that macro's comment for why.
void register_mode(Id id, ModeFn fn);

// Effect display name. "RSVD" for an ID with no registered effect - see
// this file's top comment for why that specific string.
const char *name(Id id);

// True if `id` has an actual registered renderer. Real WLED's own
// /json/eff has to stay a dense, index-aligned array matching its ID
// space (that's the whole point of matching its numbering - see this
// file's top comment), so it lists every real WLED effect name whether or
// not this firmware happens to implement it (audio-reactive/particle-
// system effects and 2D scrolling text don't - see README.md's "Effects
// library" section for why). This is how a UI can tell those apart from
// one that'll actually render something, without changing that array.
bool is_implemented(Id id);

void render(Id id, GuDisplay &display, uint32_t now_ms, const Params &params, State &state);

// Copies the RGB frame buffer render() last produced into `out` (32x32
// Rgb, row-major, matching Frame's own [y][x] layout) - a read path onto
// data that would otherwise only ever flow one way, into GuDisplay's
// gamma/PWM-encoded bitstream (see gu_display.h; there's no un-gamma-
// correcting that back into plain RGB). Exists for /debug/frame - a way to
// verify what an effect is actually doing, since "the code looks right"
// has been wrong before (see git history: DNA, Pinball, PS Volcano, and
// Fireworks 1D, which fell back to a solid fill without that being
// obvious from the source).
void get_frame(Rgb out[GuDisplay::HEIGHT][GuDisplay::WIDTH]);

}  // namespace effects

// Drop this at file scope right after defining a `mode_` function to wire
// it up - e.g. `EFFECTS_REGISTER(effects::Id::kTwinkle, mode_twinkle);`.
// Registration runs from a file-scope static object's constructor (which
// the C++ runtime always runs before setup(), same mechanism this
// firmware's other global objects like `GuDisplay display` already rely
// on - verified this also holds for a brand-new translation unit under
// this project's actual toolchain/linker, not just in general: its
// constructor symbol survives to the final link even though nothing else
// references it). That means a new effect file needs no edit anywhere
// else - effects.cpp's dispatch table just picks up whatever has
// registered itself by the time render() first runs - which is what lets
// this scale to ~160 effects landing across many independently-authored
// files without every one of them needing to touch a shared table.
#define EFFECTS_REGISTER(id, fn)                                     \
  namespace {                                                        \
  struct Reg_##fn {                                                  \
    Reg_##fn() { effects::register_mode(id, fn); }                   \
  } reg_##fn;                                                        \
  }
