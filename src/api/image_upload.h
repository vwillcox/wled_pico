#pragma once

class AsyncWebServer;

// POST /image - a raw, fixed-size RGB888 buffer (effects::kImageBytes,
// see src/effects/image_data.h) becomes the "Image" effect's picture.
// Not multipart/form-data like Ota's /update - the control page uploads
// the plain bytes it read back off a <canvas>, so a raw request body is
// all this needs.
class ImageUpload {
 public:
  void begin(AsyncWebServer &server);
};
