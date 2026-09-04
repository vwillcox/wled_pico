#pragma once

class AsyncWebServer;

// Milestone 5: web-based firmware update over the existing WiFi AP - POST
// a firmware.bin (the .bin PlatformIO already builds alongside the .uf2)
// to /update as multipart/form-data, the same mechanism ESP8266/ESP32
// Arduino sketches commonly use via the Update class. arduino-pico ships
// a compatible Updater (extern UpdaterClass Update;), so no BOOTSEL button
// is needed for updates once this firmware is running.
class Ota {
 public:
  void begin(AsyncWebServer &server);
};
