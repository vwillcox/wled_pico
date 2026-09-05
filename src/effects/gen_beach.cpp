#include "effects.h"

#include "display/gu_display.h"
#include "wled_compat.h"

// An original effect with no real WLED counterpart - waves rolling in and
// receding across a sandy beach. Colors are a fixed scene (sand, sea,
// foam) rather than routed through the palette/primary-color system
// gen_liquidfill.cpp's Liquid Fill uses, so it reads as "a beach"
// immediately rather than depending on the user having picked matching
// colors first - there's no one obvious "pick your own" axis here the way
// Liquid Fill's actual liquid color is.
namespace effects {
namespace {

constexpr int kW = GuDisplay::WIDTH;
constexpr int kH = GuDisplay::HEIGHT;

struct BeachState {
  // Per-column "high-water mark": how many logical rows past sand_top the
  // wave has recently reached (Q4 fixed-point, so it can shrink smoothly
  // rather than in whole-row jumps). Rows within this mark are wet, rows
  // beyond it stay dry - a single per-column opacity value here instead
  // would wet the *entire* column uniformly the instant any wave touched
  // it at all, including sand far past where the water actually reached.
  uint16_t wet_depth_q4[kW];
};
static_assert(sizeof(BeachState) <= State::kDataSize, "BeachState too big");

void mode_beach_waves(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  auto &s = *reinterpret_cast<BeachState *>(state.data);

  // Custom1: how much of the screen is dry beach vs. open sea.
  int sand_height = 8 + (p.custom1 >> 5);  // 8..15 rows
  int sand_top = kH - sand_height;

  // One wave per cycle: a smooth surge up onto the sand (positive half of
  // a sine), then a calm trough before the next - not a wave sitting at
  // its peak the whole time. Speed sets the cycle length. Intensity is the
  // direct wave-height control (how far up the beach waves reach), with a
  // modest per-cycle jitter layered on top (state.aux0, redrawn once per
  // cycle) so consecutive waves still form loose "sets" - some a little
  // bigger, some a little smaller - rather than every wave being identical
  // or the slider itself feeling random.
  uint32_t cycle_ms = 900u + (255u - p.speed) * 12u;
  auto cycle_idx = static_cast<uint16_t>(now_ms / cycle_ms);
  if (cycle_idx != state.aux1) {
    state.aux1 = cycle_idx;
    int base_height = 2 + (p.intensity * 20) / 255;  // 2..22 rows
    int jitter = 1 + (base_height >> 2);
    int height = base_height + random8(static_cast<uint8_t>(jitter * 2 + 1)) - jitter;
    if (height < 1) height = 1;
    state.aux0 = static_cast<uint16_t>(height);
  }
  uint32_t t = now_ms % cycle_ms;
  uint8_t phase255 = static_cast<uint8_t>((t * 255) / cycle_ms);
  uint8_t raw = sin8(phase255);
  int surge = (raw > 128) ? ((raw - 128) * state.aux0) / 127 : 0;

  uint16_t decay_q4 = static_cast<uint16_t>(1 + ((255 - p.custom2) >> 3));

  // Custom3: how thick the foam band at the wave's leading edge is - a
  // thin crisp line at 0, a wide frothy band at 255.
  int foam_thickness = 1 + (p.custom3 >> 6);  // 1..4 rows

  constexpr Rgb kDeepSea{6, 45, 95};
  constexpr Rgb kShallowSea{50, 150, 175};
  constexpr Rgb kFoam{230, 248, 250};
  constexpr Rgb kDrySand{206, 178, 108};
  constexpr Rgb kWetSand{104, 84, 48};

  for (int x = 0; x < kW; x++) {
    int col_wave = (static_cast<int>(sin8(static_cast<uint8_t>(x * 18 + (now_ms >> 6)))) - 128) / 90;  // +-1px
    // Surge must push the water/sand boundary DOWN the screen (larger row
    // number = further onto the beach) as it grows - this was subtracted
    // instead of added, so a big wave shrank the water area and exposed
    // *more* sand instead of covering more of it, and the receded/calm
    // state showed the most water. That inversion is what made washed
    // sand look dry and dry sand look freshly washed.
    int front = sand_top + surge + col_wave;
    if (front < 0) front = 0;
    if (front > kH) front = kH;

    // How far past sand_top this wave currently reaches, in logical rows -
    // extends the high-water mark immediately when a wave washes further
    // than it, otherwise lets it shrink back a little each frame. Measured
    // to the far edge of the foam band, not just `front` itself, or the
    // sand immediately next to currently-visible foam would read as dry.
    int reach = front + foam_thickness - sand_top;
    if (reach < 0) reach = 0;
    auto reach_q4 = static_cast<uint16_t>(reach << 4);
    if (p.option1) {
      if (reach_q4 > s.wet_depth_q4[x]) {
        s.wet_depth_q4[x] = reach_q4;
      } else {
        s.wet_depth_q4[x] = (s.wet_depth_q4[x] > decay_q4) ? static_cast<uint16_t>(s.wet_depth_q4[x] - decay_q4) : 0;
      }
    } else {
      s.wet_depth_q4[x] = 0;
    }

    // Rendered in logical rows - 0 is always "deep sea", kH-1 is always
    // "furthest onto dry sand" - then mapped onto the real screen row last,
    // flipped when Option3 is set so the sea ends up at the bottom of the
    // matrix and the sand at the top instead of the default sea-top/
    // sand-bottom layout.
    for (int logical_y = 0; logical_y < kH; logical_y++) {
      int y = p.option3 ? (kH - 1 - logical_y) : logical_y;
      if (logical_y < front) {
        uint8_t frac = front > 0 ? static_cast<uint8_t>((logical_y * 255) / front) : 0;
        frame[y][x] = blend(kDeepSea, kShallowSea, frac);
      } else if (logical_y < front + foam_thickness) {
        frame[y][x] = kFoam;  // the wave's leading edge
      } else {
        // This row is wet only if the high-water mark actually reaches
        // this far past sand_top - not just because *some* row in this
        // column got washed - with one row of soft fade at the edge
        // rather than a hard cutoff.
        int depth_into_sand = logical_y - sand_top;
        uint16_t depth_q4 = static_cast<uint16_t>(depth_into_sand << 4);
        uint8_t wet_amount;
        if (depth_q4 >= s.wet_depth_q4[x]) {
          wet_amount = 0;
        } else if (s.wet_depth_q4[x] - depth_q4 >= 16) {
          wet_amount = 255;
        } else {
          wet_amount = static_cast<uint8_t>((s.wet_depth_q4[x] - depth_q4) << 4);
        }

        // Cheap fixed noise texture (a hash of the coordinates, not
        // randomized per frame) so the sand isn't a flat block of color.
        uint8_t speckle = static_cast<uint8_t>((x * 37 + logical_y * 17) & 0x0F);
        Rgb sand = blend(kDrySand, kWetSand, wet_amount);
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
