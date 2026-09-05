#include "image_data.h"

#include <LittleFS.h>

#include <cstring>

namespace effects {
namespace {

constexpr const char *kPath = "/image.bin";

uint8_t g_pixels[kImageBytes] = {0};
uint8_t g_staging[kImageBytes];
bool g_has_image = false;

}  // namespace

void load_image() {
  File f = LittleFS.open(kPath, "r");
  if (!f) return;
  if (f.size() == kImageBytes && f.read(g_pixels, kImageBytes) == kImageBytes) {
    g_has_image = true;
  }
  f.close();
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

bool receive_image_chunk(const uint8_t *data, size_t len, size_t index, size_t total) {
  if (total != kImageBytes) return false;
  if (index + len > kImageBytes) return false;
  memcpy(g_staging + index, data, len);
  if (index + len != total) return true;  // more chunks still coming

  File f = LittleFS.open(kPath, "w");
  if (f) {
    f.write(g_staging, kImageBytes);
    f.close();
  }
  memcpy(g_pixels, g_staging, kImageBytes);
  g_has_image = true;
  return true;
}

}  // namespace effects
