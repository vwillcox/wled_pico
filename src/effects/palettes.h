#pragma once
#include <cstdint>
#include "effects.h"  // for effects::Rgb {uint8_t r,g,b;}

namespace effects {

// Palette IDs 0-71, matching real WLED's numbering exactly (see
// wled00/FX_fcn.cpp's Segment::loadPalette() and wled00/palettes.cpp):
//   0    "Default" - alias for FastLED's PartyColors (id 6 below). Real WLED
//        varies this per-effect via _default_palette; that nuance is out of
//        scope here, this firmware always resolves 0 the same way WLED's
//        own case-0 fallback does (PartyColors).
//   1    "Random Cycle" - real WLED slowly blends toward a new random
//        palette every ~5s (Segment::handleRandomPalette()). Out of scope:
//        alias this to the Rainbow fastled palette (id 11) instead.
//   2    primary color only (flat, all 16 stops = primary)
//   3    primary + secondary (half/half)
//   4    tertiary/secondary/primary triple wipe
//   5    primary+secondary(+tertiary if not black), more distinct bands
//   6-12 the 7 built-in FastLED palettes (Party, Cloud, Lava, Ocean, Forest,
//        Rainbow, Rainbow Bands)
//   13-71 the 59 built-in WLED gradient palettes (wled00/palettes.cpp's
//        gGradientPalettes[] array, in that exact order)
constexpr int kPaletteCount = 72;

// Display name for palette `id` - used for /json/pal. Matches real WLED's
// names (the comments next to each entry in fastledPalettes[]/
// gGradientPalettes[] in wled00/palettes.cpp - e.g. "13-00 Sunset" means
// id 13 is named "Sunset").
const char *palette_name(uint8_t id);

// WLED's ColorFromPalette(), ported. `index` sweeps 0-255 around the
// palette (wraps). IDs 2-5 are computed from `primary`/`secondary`/
// `tertiary` (segment colors) rather than a fixed table - see
// Segment::loadPalette() in wled00/FX_fcn.cpp:226 for the exact per-ID
// color math to replicate. IDs 6+ ignore primary/secondary/tertiary
// entirely (they're fixed tables).
Rgb color_from_palette(uint8_t palette_id, uint8_t index, Rgb primary, Rgb secondary, Rgb tertiary);

}  // namespace effects
