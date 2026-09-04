#include "preset_store.h"

#include <ESPAsyncWebServer.h>
#include <LittleFS.h>

void PresetStore::begin() {
  LittleFS.begin();  // formats on first boot if no filesystem is present yet
}

bool PresetStore::load_all(JsonDocument &doc) {
  File f = LittleFS.open(kPath, "r");
  if (!f) return false;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  return !err;
}

bool PresetStore::save_all(JsonDocument &doc) {
  File f = LittleFS.open(kPath, "w");
  if (!f) return false;
  bool ok = serializeJson(doc, f) > 0;
  f.close();
  return ok;
}

bool PresetStore::save(int id, JsonObjectConst state) {
  if (id < kMinId || id > kMaxId) return false;

  JsonDocument doc;
  load_all(doc);  // fine if this fails (no file yet) - doc stays empty

  JsonObject root = doc.as<JsonObject>();
  if (root.isNull()) root = doc.to<JsonObject>();

  char key[4];
  snprintf(key, sizeof(key), "%d", id);
  root[key] = state;

  return save_all(doc);
}

bool PresetStore::load(int id, JsonObject out) {
  if (id < kMinId || id > kMaxId) return false;

  JsonDocument doc;
  if (!load_all(doc)) return false;

  char key[4];
  snprintf(key, sizeof(key), "%d", id);
  JsonObjectConst preset = doc[key];
  if (preset.isNull()) return false;

  for (JsonPairConst kv : preset) out[kv.key()] = kv.value();
  return true;
}

bool PresetStore::remove(int id) {
  if (id < kMinId || id > kMaxId) return false;

  JsonDocument doc;
  if (!load_all(doc)) return false;

  char key[4];
  snprintf(key, sizeof(key), "%d", id);
  doc.remove(key);

  return save_all(doc);
}

void PresetStore::serve(AsyncWebServerRequest *request) {
  if (LittleFS.exists(kPath)) {
    request->send(LittleFS, kPath, "application/json");
  } else {
    request->send(200, "application/json", "{}");
  }
}
