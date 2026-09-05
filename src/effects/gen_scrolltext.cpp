#include "effects.h"

#include <cstring>

#include "display/gu_display.h"
#include "font_tom_thumb.h"
#include "palettes.h"
#include "wled_compat.h"

// wled00/FX.cpp:6382 mode_2Dscrollingtext(). Real WLED's version supports 5
// selectable fonts via a `FontManager` that caches glyphs and can load
// custom `.wbf` font files from the filesystem, date/time token
// substitution (#TIME, #DATE, ... - this firmware has no RTC/NTP time
// source to feed those), text rotation, gradient coloring, and a fading
// trail. This port is deliberately narrower - the actual "draw scrolling
// text" core, not the font-management/clock/rotation machinery around it:
// one embedded font (Tom Thumb, font_tom_thumb.h - real WLED's own
// default), no rotation, no trail/gradient, no time tokens. `params.name`
// (real WLED dual-purposes SEGMENT.name as the scroll text the same way,
// rather than a dedicated field) is what scrolls, at a rate driven by
// `speed`; `intensity` nudges it vertically (real WLED's "Y Offset",
// custom1 there - kept on intensity here since this port doesn't use
// custom1 for anything else in this effect); `option3` reverses direction
// (real WLED's "Reverse", its own option3 too).
namespace effects {
namespace {

void mode_2dscrollingtext(uint32_t now_ms, const Params &p, State &, Frame frame) {
  constexpr int width = GuDisplay::WIDTH;
  constexpr int height = GuDisplay::HEIGHT;
  constexpr int glyph_w = kFontGlyphWidth;
  constexpr int glyph_h = kFontGlyphHeight;
  constexpr int spacing = 1;
  constexpr int advance = glyph_w + spacing;

  for (int y = 0; y < height; y++)
    for (int x = 0; x < width; x++) frame[y][x] = p.secondary;

  size_t len = strnlen(p.name, Params::kNameSize - 1);
  if (len == 0) return;

  int total_width = static_cast<int>(len) * advance - spacing;  // no trailing spacing after the last glyph
  int scroll_span = total_width + width;  // text travels from fully off-screen-right to fully off-screen-left

  uint32_t speed_px_per_sec = 4 + p.speed / 4;  // ~4-67 px/sec
  int32_t offset = static_cast<int32_t>((static_cast<uint64_t>(now_ms) * speed_px_per_sec / 1000) %
                                          static_cast<uint32_t>(scroll_span));
  if (p.option3) offset = scroll_span - 1 - offset;

  int text_start_x = width - offset;  // on-screen column of the text's leftmost pixel

  int y_offset = (height - glyph_h) / 2 + (static_cast<int>(p.intensity) - 128) / 16;
  if (y_offset < 0) y_offset = 0;
  if (y_offset > height - glyph_h) y_offset = height - glyph_h;

  for (int sx = 0; sx < width; sx++) {
    int text_col = sx - text_start_x;
    if (text_col < 0 || text_col >= total_width) continue;
    size_t char_idx = static_cast<size_t>(text_col / advance);
    int col_in_char = text_col % advance;
    if (col_in_char >= glyph_w || char_idx >= len) continue;

    char c = p.name[char_idx];
    for (int row = 0; row < glyph_h; row++) {
      if (!font_pixel(c, col_in_char, row)) continue;
      Rgb color = color_from_palette(p.palette_id, static_cast<uint8_t>((char_idx * 37 + row * 11) & 0xFF), p.primary,
                                      p.secondary, p.tertiary);
      frame[y_offset + row][sx] = color;
    }
  }
}
EFFECTS_REGISTER(Id::k2dscrolltext, mode_2dscrollingtext)

}  // namespace
}  // namespace effects
