#pragma once

#include <cstddef>
#include <cstdint>

#include "effects.h"

// Backing store for the "Image" effect (Id::kImage - see gen_batch2.cpp's
// mode_image()), including animation. Real WLED's mode_image() displays an
// uploaded GIF via its WLED_ENABLE_GIF feature (filesystem-backed storage
// + a GIF/LZW decoder); this firmware has neither. Instead of porting a
// GIF decoder onto an RP2040, the decode work is pushed onto the browser:
// the control page uses the WebCodecs `ImageDecoder` API to decode every
// frame of whatever file the user picks (GIF's LZW decoding included, for
// free) plus each frame's real duration, draws each onto a 32x32 <canvas>,
// and uploads the whole sequence - so this side only ever handles a small,
// simple container of already-decoded RGB frames, not any image format's
// actual encoding.
//
// On-disk format (/image.bin), written verbatim as the upload streams in
// (no RAM staging buffer - see receive_image_chunk()):
//   uint16_t frame_count (little-endian)
//   uint16_t delay_ms[frame_count] (little-endian each; 0 means "not
//     specified", treated as 100ms - only possible for a 1-frame /
//     non-animated upload, see the control page's fallback path)
//   uint8_t pixels[frame_count][kImageBytes] (RGB888, row-major, back to
//     back with no padding)
namespace effects {

constexpr int kImageWidth = GuDisplay::WIDTH;
constexpr int kImageHeight = GuDisplay::HEIGHT;
constexpr size_t kImageBytes = static_cast<size_t>(kImageWidth) * kImageHeight * 3;  // RGB888, row-major

// Cap on stored frames - generous for a typical small pixel-art GIF
// without letting a huge upload fill the 1MB LittleFS partition or take
// forever over the AP's WiFi. The control page caps its own decode to the
// same number (kept in sync by comment, not code - see captive_portal.cpp).
constexpr int kMaxFrames = 40;
constexpr size_t kMaxUploadBytes = 2 + static_cast<size_t>(kMaxFrames) * 2 + static_cast<size_t>(kMaxFrames) * kImageBytes;

// Loads /image.bin's metadata and its first frame into the in-RAM cache,
// if one was uploaded on a previous boot. Safe to call once from setup().
void load_image();

// True once a valid image has been uploaded (this boot or a previous one).
bool has_image();

// {0,0,0} if !has_image(). x/y outside [0,kImageWidth)/[0,kImageHeight)
// are clamped rather than out-of-bounds. Reads the currently-cached frame
// only - see update_animation() for advancing it.
Rgb image_pixel(int x, int y);

// Advances playback: if `restart` (mode_image passes state.call==0, i.e.
// the effect was just (re)selected), resets to frame 0; otherwise checks
// whether the current frame's stored delay has elapsed and, if so, loads
// the next one (wrapping around). A no-op if there's no image or it has
// only one frame. Call once per rendered frame.
void update_animation(uint32_t now_ms, bool restart);

// Feeds one chunk of an image upload - `index`/`len`/`total` straight from
// ESPAsyncWebServer's body-handler callback shape. Streams directly to
// /image.bin on LittleFS as chunks arrive (no full-upload RAM buffer -
// kMaxUploadBytes would be ~120KB, too large to stage in RAM alongside
// everything else this firmware keeps resident). Once `index + len ==
// total`, re-reads the file's metadata and first frame via load_image().
// Rejects (returns false, writes nothing) if `total` is 0 or exceeds
// kMaxUploadBytes.
bool receive_image_chunk(const uint8_t *data, size_t len, size_t index, size_t total);

}  // namespace effects
