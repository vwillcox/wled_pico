#include "effects.h"

#include "display/gu_display.h"
#include "wled_compat.h"

// An original effect with no real WLED counterpart - waves rolling in and
// receding across a sandy beach. Like gen_bathtub.cpp's Bathtub Fill,
// colors are a fixed scene (sand, sea, foam) rather than routed through
// the palette/primary-color system, so it reads as "a beach" immediately
// rather than depending on the user having picked matching colors first.
namespace effects {
namespace {

constexpr int kW = GuDisplay::WIDTH;
constexpr int kH = GuDisplay::HEIGHT;

struct BeachState {
  uint8_t wet[kW];  // per-column wetness, 255=freshly washed, decays toward 0
};
static_assert(sizeof(BeachState) <= State::kDataSize, "BeachState too big");

void mode_beach_waves(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  auto &s = *reinterpret_cast<BeachState *>(state.data);

  // Custom1: how much of the screen is dry beach vs. open sea.
  int sand_height = 8 + (p.custom1 >> 5);  // 8..15 rows
  int sand_top = kH - sand_height;

  // One wave per cycle: a smooth surge up onto the sand (positive half of
  // a sine), then a calm trough before the next - not a wave sitting at
  // its peak the whole time. Speed sets the cycle length; a fresh random
  // peak height is drawn every cycle (state.aux0) so waves form loose
  // "sets" (some bigger, some smaller) instead of all being identical.
  uint32_t cycle_ms = 900u + (255u - p.speed) * 12u;
  auto cycle_idx = static_cast<uint16_t>(now_ms / cycle_ms);
  if (cycle_idx != state.aux1) {
    state.aux1 = cycle_idx;
    state.aux0 = static_cast<uint16_t>(5 + random8(static_cast<uint8_t>(3 + (p.intensity >> 4))));
  }
  uint32_t t = now_ms % cycle_ms;
  uint8_t phase255 = static_cast<uint8_t>((t * 255) / cycle_ms);
  uint8_t raw = sin8(phase255);
  int surge = (raw > 128) ? ((raw - 128) * state.aux0) / 127 : 0;

  uint8_t decay = static_cast<uint8_t>(1 + ((255 - p.custom2) >> 5));

  constexpr Rgb kDeepSea{6, 45, 95};
  constexpr Rgb kShallowSea{50, 150, 175};
  constexpr Rgb kFoam{230, 248, 250};
  constexpr Rgb kDrySand{206, 178, 108};
  constexpr Rgb kWetSand{104, 84, 48};

  for (int x = 0; x < kW; x++) {
    int col_wave = (static_cast<int>(sin8(static_cast<uint8_t>(x * 18 + (now_ms >> 6)))) - 128) / 90;  // +-1px
    int front = sand_top - surge + col_wave;
    if (front < 0) front = 0;
    if (front > kH) front = kH;

    // front <= sand_top is true almost every frame just from the resting
    // waterline plus col_wave's cosmetic jitter, even with no real surge -
    // gating on surge itself (not just the jittered front position) is
    // what keeps sand able to fully dry between waves instead of reading
    // as permanently wet.
    bool washed = surge > 1 && front <= sand_top;
    if (p.option1) {
      s.wet[x] = washed ? 255 : qsub8(s.wet[x], decay);
    } else {
      s.wet[x] = 0;
    }

    for (int y = 0; y < kH; y++) {
      if (y < front) {
        uint8_t frac = front > 0 ? static_cast<uint8_t>((y * 255) / front) : 0;
        frame[y][x] = blend(kDeepSea, kShallowSea, frac);
      } else if (y < front + 1) {
        frame[y][x] = kFoam;  // the wave's leading edge
      } else {
        // Cheap fixed noise texture (a hash of the coordinates, not
        // randomized per frame) so the sand isn't a flat block of color.
        uint8_t speckle = static_cast<uint8_t>((x * 37 + y * 17) & 0x0F);
        Rgb sand = blend(kDrySand, kWetSand, s.wet[x]);
        sand.r = qsub8(sand.r, speckle >> 2);
        sand.g = qsub8(sand.g, speckle >> 3);
        frame[y][x] = sand;
      }
    }
  }
}
EFFECTS_REGISTER(Id::kBeachWaves, mode_beach_waves)

}  // namespace
}  // namespace effects
