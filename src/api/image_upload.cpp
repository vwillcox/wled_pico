#include "image_upload.h"

#include <ESPAsyncWebServer.h>

#include "effects/image_data.h"

void ImageUpload::begin(AsyncWebServer &server) {
  server.on(
      "/image", HTTP_POST,
      [](AsyncWebServerRequest *request) { request->send(200, "text/plain", "ok"); },
      nullptr,
      [](AsyncWebServerRequest *, uint8_t *data, size_t len, size_t index, size_t total) {
        effects::receive_image_chunk(data, len, index, total);
      });
}
