#pragma once

#include <cstdint>

class GuDisplay;

// Milestone 3: WLED's effect math, ported off its Segment/SEGENV machinery
// onto a flat per-column buffer. Each effect here is genuinely WLED's own
// algorithm (see effects.cpp for the exact source lines it's ported from,
// in wled/WLED's wled00/FX.cpp) — not reinvented, just re-hosted.
//
// WLED's Segment can be 1D or 2D; these four effects all run as 1D against
// SEGLEN in the original code, so here they compute one color per matrix
// column and broadcast it across every row - the same "1D effect on a 2D
// board" shape WLED itself uses when a segment spans a matrix's width.
namespace effects {

struct Rgb {
  uint8_t r, g, b;
};

enum class Id : uint8_t {
  kSolid,
  kWipe,
  kRainbow,
  kBreathe,
  kCount,
};

struct Params {
  uint8_t speed = 128;      // matches SEGMENT.speed, 0-255
  uint8_t intensity = 128;  // matches SEGMENT.intensity, 0-255
  Rgb primary{255, 0, 0};   // matches SEGCOLOR(0)
  Rgb secondary{0, 0, 0};   // matches SEGCOLOR(1)
};

// Renders one frame of `id` into `display`. `now_ms` should be a
// free-running millis()-style timestamp (this is what WLED's strip.now is).
void render(Id id, GuDisplay &display, uint32_t now_ms, const Params &params);

const char *name(Id id);

}  // namespace effects
