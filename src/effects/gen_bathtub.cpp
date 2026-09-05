#include "effects.h"

#include "display/gu_display.h"
#include "wled_compat.h"

// An original effect with no real WLED counterpart - a bathtub filling
// with water. Water color is a fixed blue gradient rather than routed
// through the palette/primary-color system every ported effect here uses:
// this is a small fixed "scene" (water, foam, tub wall) in the same spirit
// as WLED's own Akemi or Halloween Eyes hardcoding their character colors,
// so it looks like water immediately rather than depending on the user
// having already picked a blue palette. Speed/Intensity/Custom1/Option1
// still shape the animation - see each's use below.
namespace effects {
namespace {

constexpr int kW = GuDisplay::WIDTH;
constexpr int kH = GuDisplay::HEIGHT;
constexpr int kMaxBubbles = 20;

struct Bubble {
  uint8_t x;
  float y;      // row units, kH-1 (bottom) rising toward 0 (top)
  bool active;
};
struct BathtubState {
  Bubble bubbles[kMaxBubbles];
};
static_assert(sizeof(BathtubState) <= State::kDataSize, "BathtubState too big");

enum Phase : uint16_t { kFilling = 0, kPausedFull = 1, kDraining = 2, kPausedEmpty = 3 };

void mode_bathtub_fill(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  auto &s = *reinterpret_cast<BathtubState *>(state.data);

  if (state.call == 0) {
    state.aux0 = kFilling;
    state.step = now_ms;
  }

  // Speed sets how fast the tub fills; drain and both pauses scale off the
  // same number so a fast setting reads as one snappy cycle, not a fast
  // fill glued to a slow drain. Option1 turns the drain-and-refill loop on
  // at all - off, it just fills once and stays full (a bath run once,
  // rather than looping for an ambient display).
  uint32_t fill_ms = 1500u + (255u - p.speed) * 40u;  // ~1.5s (fast) .. ~11.7s (slow)
  uint32_t drain_ms = fill_ms / 2;
  uint32_t pause_full_ms = 500u + p.custom2 * 4u;  // ~0.5s..1.5s dwell at full
  uint32_t pause_empty_ms = 400;

  uint32_t elapsed = now_ms - state.step;
  float level;  // 0 (empty) .. 1 (full)

  switch (state.aux0) {
    case kFilling:
      level = static_cast<float>(elapsed) / static_cast<float>(fill_ms);
      if (level >= 1.0f) {
        level = 1.0f;
        if (p.option1) {
          state.aux0 = kPausedFull;
          state.step = now_ms;
        }
      }
      break;
    case kPausedFull:
      level = 1.0f;
      if (elapsed >= pause_full_ms) {
        state.aux0 = kDraining;
        state.step = now_ms;
      }
      break;
    case kDraining:
      level = 1.0f - static_cast<float>(elapsed) / static_cast<float>(drain_ms);
      if (level <= 0.0f) {
        level = 0.0f;
        state.aux0 = kPausedEmpty;
        state.step = now_ms;
      }
      break;
    default:  // kPausedEmpty
      level = 0.0f;
      if (elapsed >= pause_empty_ms) {
        state.aux0 = kFilling;
        state.step = now_ms;
      }
      break;
  }

  int filled_rows = static_cast<int>(level * kH + 0.5f);
  int base_surface_row = kH - filled_rows;  // flat waterline row (0=top, kH=fully drained/off-bottom)

  // Choppier surface at higher Intensity. Amplitude in whole pixels stays
  // small (1-3) - this is a 32-row matrix, a big wave would eat the scene.
  int amplitude = 1 + (p.intensity >> 6);
  uint8_t time_phase = static_cast<uint8_t>(now_ms >> 5);

  constexpr Rgb kWallColor{10, 11, 15};       // dim, faintly cool - porcelain in low light
  constexpr Rgb kDeepWater{8, 40, 92};        // near the tub floor
  constexpr Rgb kShallowWater{60, 170, 205};  // right at the surface
  constexpr Rgb kFoam{225, 245, 250};

  int surface_row[kW];
  for (int x = 0; x < kW; x++) {
    int wave = (static_cast<int>(sin8(static_cast<uint8_t>(x * 24 + time_phase))) - 128) * amplitude / 128;
    int row = base_surface_row + wave;
    if (row < 0) row = 0;
    if (row > kH) row = kH;
    surface_row[x] = row;
  }

  for (int x = 0; x < kW; x++) {
    int surf = surface_row[x];
    for (int y = 0; y < kH; y++) {
      if (y < surf) {
        frame[y][x] = kWallColor;
      } else if (y == surf && filled_rows > 0) {
        frame[y][x] = kFoam;
      } else {
        uint8_t depth = static_cast<uint8_t>(((y - surf) * 255) / kH);
        frame[y][x] = blend(kShallowWater, kDeepWater, depth);
      }
    }
  }

  // Bubbles fizz up from the tub floor while there's enough water to
  // plausibly hold them. Custom1 caps how many are active at once (1..20)
  // rather than just nudging the spawn chance within a small fixed pool -
  // that made the slider barely visible, since a handful of slots fill up
  // almost immediately regardless of the chance per frame. Capping the
  // count directly instead makes low settings read as "the occasional
  // bubble" and high settings as "fizzing", a real difference across the
  // slider's range.
  int bubble_limit = 1 + (static_cast<int>(p.custom1) * (kMaxBubbles - 1)) / 255;
  if (filled_rows > 4) {
    int active_count = 0;
    for (const auto &b : s.bubbles)
      if (b.active) active_count++;

    for (auto &b : s.bubbles) {
      if (!b.active) {
        if (active_count < bubble_limit && random8() < 50) {
          b.active = true;
          b.x = random8(static_cast<uint8_t>(kW));
          b.y = static_cast<float>(kH - 1);
          active_count++;
        }
        continue;
      }
      b.y -= 0.4f;
      int row = static_cast<int>(b.y);
      if (row <= surface_row[b.x]) {
        b.active = false;
        continue;
      }
      if (row >= 0 && row < kH) frame[row][b.x] = blend(frame[row][b.x], kFoam, 200);
    }
  } else {
    for (auto &b : s.bubbles) b.active = false;
  }
}
EFFECTS_REGISTER(Id::kBathtubFill, mode_bathtub_fill)

}  // namespace
}  // namespace effects
