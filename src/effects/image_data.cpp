#include "image_data.h"

#include <LittleFS.h>

#include <cstring>

namespace effects {
namespace {

constexpr const char *kPath = "/image.bin";

uint8_t g_pixels[kImageBytes] = {0};
bool g_has_image = false;
uint16_t g_frame_count = 0;
uint16_t g_current_frame = 0;
uint16_t g_current_delay_ms = 100;
uint32_t g_frame_start_ms = 0;
File g_upload_file;  // only open while an upload is in progress

uint32_t frame_data_offset(uint16_t frame_count, uint16_t frame_index) {
  return 2u + static_cast<uint32_t>(frame_count) * 2u + static_cast<uint32_t>(frame_index) * kImageBytes;
}

uint16_t read_u16le(File &f) {
  uint8_t lo = 0, hi = 0;
  f.read(&lo, 1);
  f.read(&hi, 1);
  return static_cast<uint16_t>(lo | (static_cast<uint16_t>(hi) << 8));
}

// Loads frame `frame_index`'s pixels into g_pixels and its delay into
// g_current_delay_ms. False (leaving both untouched) if the file's gone,
// too short, or the index is out of range.
bool load_frame(uint16_t frame_index) {
  if (frame_index >= g_frame_count) return false;
  File f = LittleFS.open(kPath, "r");
  if (!f) return false;
  f.seek(2 + static_cast<uint32_t>(frame_index) * 2u);
  uint16_t delay = read_u16le(f);
  f.seek(frame_data_offset(g_frame_count, frame_index));
  bool ok = f.read(g_pixels, kImageBytes) == kImageBytes;
  f.close();
  if (ok) g_current_delay_ms = delay ? delay : 100;  // 0 = "unspecified" (a 1-frame/static upload)
  return ok;
}

}  // namespace

void load_image() {
  g_has_image = false;
  g_frame_count = 0;

  File f = LittleFS.open(kPath, "r");
  if (!f) return;
  if (f.size() < 2) { f.close(); return; }
  uint16_t frame_count = read_u16le(f);
  uint64_t expected_size = frame_data_offset(frame_count, frame_count);  // == offset just past the last frame
  bool size_ok = frame_count > 0 && frame_count <= kMaxFrames && f.size() == static_cast<size_t>(expected_size);
  f.close();
  if (!size_ok) return;

  g_frame_count = frame_count;
  g_current_frame = 0;
  g_frame_start_ms = 0;
  g_has_image = load_frame(0);
  if (!g_has_image) g_frame_count = 0;
}

bool has_image() { return g_has_image; }

Rgb image_pixel(int x, int y) {
  if (!g_has_image) return Rgb{0, 0, 0};
  if (x < 0) x = 0;
  if (x >= kImageWidth) x = kImageWidth - 1;
  if (y < 0) y = 0;
  if (y >= kImageHeight) y = kImageHeight - 1;
  const uint8_t *p = &g_pixels[(static_cast<size_t>(y) * kImageWidth + x) * 3];
  return Rgb{p[0], p[1], p[2]};
}

void update_animation(uint32_t now_ms, bool restart) {
  if (!g_has_image) return;
  if (restart) {
    g_current_frame = 0;
    g_frame_start_ms = now_ms;
    load_frame(0);
    return;
  }
  if (g_frame_count <= 1) return;
  if (now_ms - g_frame_start_ms < g_current_delay_ms) return;
  uint16_t next = static_cast<uint16_t>((g_current_frame + 1) % g_frame_count);
  if (load_frame(next)) {
    g_current_frame = next;
    g_frame_start_ms = now_ms;
  }
}

bool receive_image_chunk(const uint8_t *data, size_t len, size_t index, size_t total) {
  if (total == 0 || total > kMaxUploadBytes) return false;
  if (index == 0) {
    g_upload_file = LittleFS.open(kPath, "w");
    if (!g_upload_file) return false;
  }
  if (!g_upload_file) return false;  // open at index 0 failed - ignore the rest of this upload
  g_upload_file.write(data, len);
  if (index + len >= total) {
    g_upload_file.close();
    load_image();
  }
  return true;
}

}  // namespace effects
