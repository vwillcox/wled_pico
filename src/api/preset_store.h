#pragma once

#include <ArduinoJson.h>

// LittleFS-backed preset storage, matching real WLED's presets.json shape
// closely enough to be recognizable: a JSON object keyed by preset id
// (as a string, ArduinoJson/WLED convention) mapping to a saved state
// object. WLED supports up to 250 presets with names, quick-load labels,
// and playlists; this firmware only needs a handful of numbered slots, so
// names/playlists are out of scope.
class PresetStore {
 public:
  static constexpr int kMinId = 1;
  static constexpr int kMaxId = 8;

  // Mounts LittleFS. Safe to call once from setup(); formats on first boot
  // if the filesystem isn't already there (LittleFS.begin()'s normal
  // behavior via arduino-pico).
  void begin();

  // Writes `state` (typically the same object JsonApi::serialize_state()
  // produces) under preset `id`, merging into whatever's already on disk.
  bool save(int id, JsonObjectConst state);

  // Reads preset `id` into `out`. Returns false if it doesn't exist.
  bool load(int id, JsonObject out);

  bool remove(int id);

  // Serves the raw presets.json file, matching real WLED's GET
  // /presets.json path and shape exactly (it's just the file on disk).
  void serve(class AsyncWebServerRequest *request);

 private:
  static constexpr const char *kPath = "/presets.json";

  bool load_all(JsonDocument &doc);
  bool save_all(JsonDocument &doc);
};
