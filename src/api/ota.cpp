#include "ota.h"

#include <ESPAsyncWebServer.h>
#include <Updater.h>

void Ota::begin(AsyncWebServer &server) {
  server.on(
      "/update", HTTP_POST,
      [](AsyncWebServerRequest *request) {
        bool ok = !Update.hasError();
        AsyncWebServerResponse *response =
            request->beginResponse(200, "text/plain", ok ? "OK, rebooting" : "update failed");
        response->addHeader("Connection", "close");
        request->send(response);
        if (ok) request->onDisconnect([]() { rp2040.reboot(); });
      },
      [](AsyncWebServerRequest *request, const String &filename, size_t index, uint8_t *data,
         size_t len, bool final) {
        if (index == 0) {
          Serial.printf("OTA: starting update, %s (%u bytes)\n", filename.c_str(),
                         request->contentLength());
          if (!Update.begin(request->contentLength(), U_FLASH)) {
            Update.printError(Serial);
          }
        }
        if (len && Update.write(data, len) != len) {
          Update.printError(Serial);
        }
        if (final) {
          if (Update.end(true)) {
            Serial.println("OTA: update complete, rebooting");
          } else {
            Update.printError(Serial);
          }
        }
      });
}
