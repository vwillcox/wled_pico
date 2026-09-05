#pragma once

#include <cstddef>
#include <cstdint>

#include "effects.h"

// Backing store for the "Image" effect (Id::kImage - see gen_batch2.cpp's
// mode_image()). Real WLED's mode_image() displays an uploaded GIF via its
// WLED_ENABLE_GIF feature (filesystem-backed storage + a GIF decoder);
// this firmware has neither. Instead of porting a GIF decoder onto an
// RP2040, the decode work is pushed onto the browser: the control page
// draws whatever image file the user picks onto a 32x32 <canvas> (the
// browser handles JPEG/PNG/GIF/WEBP decoding and the resize for free) and
// uploads the raw resulting pixels here - so this side only ever handles
// one simple, fixed-size raw RGB buffer, no decoding of its own.
namespace effects {

constexpr int kImageWidth = GuDisplay::WIDTH;
constexpr int kImageHeight = GuDisplay::HEIGHT;
constexpr size_t kImageBytes = static_cast<size_t>(kImageWidth) * kImageHeight * 3;  // RGB888, row-major

// Mounts nothing new (LittleFS is already mounted elsewhere) - just loads
// /image.bin into the in-RAM cache if one was uploaded on a previous boot.
// Safe to call once from setup().
void load_image();

// True once an image has been uploaded (this boot or a previous one).
bool has_image();

// {0,0,0} if !has_image(). x/y outside [0,kImageWidth)/[0,kImageHeight)
// are clamped rather than out-of-bounds.
Rgb image_pixel(int x, int y);

// Feeds one chunk of a raw image upload - `index`/`len`/`total` straight
// from ESPAsyncWebServer's body-handler callback shape. Accumulates into a
// staging buffer; once `index + len == total`, commits to LittleFS and
// swaps it into the live in-RAM cache. Returns false (and accepts no
// partial write) if `total` isn't exactly kImageBytes - the upload is
// expected to already be the right size (the control page's canvas step
// guarantees that), not something to renegotiate here.
bool receive_image_chunk(const uint8_t *data, size_t len, size_t index, size_t total);

}  // namespace effects
