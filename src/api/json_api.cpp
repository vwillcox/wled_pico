#include "json_api.h"

#include <AsyncJson.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <WiFi.h>

#include <cstring>

#include "display/gu_display.h"
#include "effects/palettes.h"
#include "net/device_id.h"

namespace {
constexpr size_t kEffectCount = static_cast<size_t>(effects::Id::kCount);
constexpr int kPixelCount = GuDisplay::WIDTH * GuDisplay::HEIGHT;
}  // namespace

// wled00/json.cpp's serializeState()/serializeSegment(), trimmed to the
// single-segment, no-playlist/nightlight subset this firmware actually has
// state for. Field names (on, bri, ps, seg[].{id,start,stop,len,fx,sx,ix,
// col}) match real WLED exactly.
void JsonApi::serialize_state(JsonObject root) const {
  root["on"] = on_;
  root["bri"] = bri_;
  root["transition"] = 0;
  root["ps"] = current_preset_;
  root["pl"] = -1;
  root["mainseg"] = 0;
  root["ledmap"] = 0;

  // "nl", "udpn" and "lor" are required keys in real WLED's /json/state
  // (see wled00/json.cpp's serializeState()) - the nightlight, UDP-sync and
  // live-data-override features they describe aren't implemented here, but
  // WLED client libraries (e.g. Home Assistant's) fail to parse a state
  // object missing them, so the objects are still sent with WLED's own
  // "off/none" defaults.
  JsonObject nl = root["nl"].to<JsonObject>();
  nl["on"] = false;
  nl["dur"] = 1;
  nl["mode"] = 0;
  nl["tbri"] = 0;
  nl["rem"] = -1;

  JsonObject udpn = root["udpn"].to<JsonObject>();
  udpn["send"] = false;
  udpn["recv"] = false;
  udpn["sgrp"] = 1;
  udpn["rgrp"] = 1;

  root["lor"] = 0;

  JsonArray segs = root["seg"].to<JsonArray>();
  JsonObject seg = segs.add<JsonObject>();
  seg["id"] = 0;
  seg["start"] = 0;
  seg["stop"] = kPixelCount;
  seg["len"] = kPixelCount;
  seg["on"] = on_;
  seg["bri"] = 255;
  seg["fx"] = static_cast<int>(effect_id_);
  seg["sx"] = params_.speed;
  seg["ix"] = params_.intensity;
  seg["c1"] = params_.custom1;
  seg["c2"] = params_.custom2;
  seg["c3"] = params_.custom3;
  seg["o1"] = params_.option1;
  seg["o2"] = params_.option2;
  seg["o3"] = params_.option3;
  seg["pal"] = params_.palette_id;
  seg["n"] = params_.name;
  seg["sel"] = true;

  JsonArray col = seg["col"].to<JsonArray>();
  JsonArray c0 = col.add<JsonArray>();
  c0.add(params_.primary.r); c0.add(params_.primary.g); c0.add(params_.primary.b);
  JsonArray c1 = col.add<JsonArray>();
  c1.add(params_.secondary.r); c1.add(params_.secondary.g); c1.add(params_.secondary.b);
  JsonArray c2 = col.add<JsonArray>();
  c2.add(params_.tertiary.r); c2.add(params_.tertiary.g); c2.add(params_.tertiary.b);
}

// wled00/json.cpp's serializeInfo(), trimmed to fields worth having for a
// board with no multi-segment support.
void JsonApi::serialize_info(JsonObject root) const {
  // "ver" is deliberately a real WLED release string, not this firmware's
  // own version - WLED client libraries (Home Assistant's in particular)
  // parse this as a semver and refuse to set up the device below a minimum
  // supported version (0.14.0 as of this writing). This firmware's JSON
  // schema is compatible with that release line, so it reports as one
  // rather than getting rejected outright over an unparseable string.
  root["ver"] = "0.15.0";
  root["vid"] = 0;
  root["brand"] = "wled_pico";
  root["product"] = "Cosmic Unicorn";
  root["name"] = "wled_pico";
  root["arch"] = "rp2040";
  root["core"] = "arduino-pico";
  root["mac"] = device_mac();

  JsonObject leds = root["leds"].to<JsonObject>();
  leds["count"] = kPixelCount;
  leds["fps"] = 60;
  leds["maxseg"] = 1;
  leds["rgbw"] = false;
  leds["wv"] = false;
  JsonObject matrix = leds["matrix"].to<JsonObject>();
  matrix["w"] = GuDisplay::WIDTH;
  matrix["h"] = GuDisplay::HEIGHT;

  root["str"] = false;
  root["live"] = live_;
  root["liveseg"] = -1;
  root["lm"] = live_ ? "UDP" : "";
  root["lip"] = "";
  root["ws"] = static_cast<int>(ws_.count());
  root["fxcount"] = kEffectCount;
  root["palcount"] = effects::kPaletteCount;
  root["freeheap"] = rp2040.getFreeHeap();
  root["uptime"] = millis() / 1000;

  // "fs" is a required key for WLED client libraries (info.filesystem has
  // no default) - only its presence matters to them, but real usage numbers
  // cost nothing to report.
  JsonObject fs = root["fs"].to<JsonObject>();
  FSInfo fs_info{};
  LittleFS.info(fs_info);
  fs["u"] = fs_info.usedBytes / 1000;
  fs["t"] = fs_info.totalBytes / 1000;
  fs["pmt"] = 0;

  JsonObject wifi = root["wifi"].to<JsonObject>();
  wifi["bssid"] = WiFi.softAPmacAddress();
  wifi["signal"] = 100;
  wifi["channel"] = WiFi.channel();

  root["ip"] = WiFi.softAPIP().toString();
}

void JsonApi::serialize_effects(JsonArray arr) const {
  for (size_t i = 0; i < kEffectCount; i++) arr.add(effects::name(static_cast<effects::Id>(i)));
}

void JsonApi::serialize_palettes(JsonArray arr) const {
  for (int i = 0; i < effects::kPaletteCount; i++) arr.add(effects::palette_name(static_cast<uint8_t>(i)));
}

// wled00/json.cpp's deserializeState()/deserializeSegment(), trimmed the
// same way, plus the psave/ps/pdel preset fields handleSet()/json.cpp
// handle in real WLED. Only seg[0] is honored (no multi-segment); "col"
// entries follow WLED's [r,g,b] array form only (hex-string/object/Kelvin
// forms from real WLED are out of scope here).
void JsonApi::apply_basic_state(JsonObjectConst root) {
  if (root["on"].is<bool>()) on_ = root["on"].as<bool>();
  if (root["bri"].is<int>()) {
    int v = root["bri"].as<int>();
    bri_ = static_cast<uint8_t>(v < 1 ? 1 : (v > 255 ? 255 : v));
  }

  JsonArrayConst segs = root["seg"];
  if (!segs.isNull() && segs.size() > 0) {
    JsonObjectConst seg = segs[0];

    if (seg["fx"].is<int>()) {
      int fx = seg["fx"].as<int>();
      if (fx >= 0 && fx < static_cast<int>(kEffectCount) && static_cast<effects::Id>(fx) != effect_id_) {
        effect_id_ = static_cast<effects::Id>(fx);
        state_.reset();  // matches WLED zeroing SEGENV on a mode switch
      }
    }
    if (seg["sx"].is<int>()) params_.speed = static_cast<uint8_t>(seg["sx"].as<int>());
    if (seg["ix"].is<int>()) params_.intensity = static_cast<uint8_t>(seg["ix"].as<int>());
    if (seg["c1"].is<int>()) params_.custom1 = static_cast<uint8_t>(seg["c1"].as<int>());
    if (seg["c2"].is<int>()) params_.custom2 = static_cast<uint8_t>(seg["c2"].as<int>());
    if (seg["c3"].is<int>()) params_.custom3 = static_cast<uint8_t>(seg["c3"].as<int>());
    if (seg["o1"].is<bool>()) params_.option1 = seg["o1"].as<bool>();
    if (seg["o2"].is<bool>()) params_.option2 = seg["o2"].as<bool>();
    if (seg["o3"].is<bool>()) params_.option3 = seg["o3"].as<bool>();
    if (seg["pal"].is<int>()) params_.palette_id = static_cast<uint8_t>(seg["pal"].as<int>());
    if (seg["n"].is<const char *>()) {
      strncpy(params_.name, seg["n"].as<const char *>(), effects::Params::kNameSize - 1);
      params_.name[effects::Params::kNameSize - 1] = '\0';
    }

    JsonArrayConst col = seg["col"];
    if (!col.isNull()) {
      if (col.size() > 0 && !col[0].isNull()) {
        JsonArrayConst c = col[0];
        if (c.size() >= 3) {
          params_.primary = {static_cast<uint8_t>(c[0].as<int>()),
                              static_cast<uint8_t>(c[1].as<int>()),
                              static_cast<uint8_t>(c[2].as<int>())};
        }
      }
      if (col.size() > 1 && !col[1].isNull()) {
        JsonArrayConst c = col[1];
        if (c.size() >= 3) {
          params_.secondary = {static_cast<uint8_t>(c[0].as<int>()),
                                static_cast<uint8_t>(c[1].as<int>()),
                                static_cast<uint8_t>(c[2].as<int>())};
        }
      }
      if (col.size() > 2 && !col[2].isNull()) {
        JsonArrayConst c = col[2];
        if (c.size() >= 3) {
          params_.tertiary = {static_cast<uint8_t>(c[0].as<int>()),
                               static_cast<uint8_t>(c[1].as<int>()),
                               static_cast<uint8_t>(c[2].as<int>())};
        }
      }
    }
  }
}

void JsonApi::apply_state(JsonObjectConst root) {
  apply_basic_state(root);

  // Preset fields, applied after the above so "psave" snapshots whatever
  // else this same request just changed - matches real WLED applying
  // state before handling presets in handleSet().
  if (root["ps"].is<int>()) {
    int id = root["ps"].as<int>();
    JsonDocument doc;
    JsonObject loaded = doc.to<JsonObject>();
    if (presets_.load(id, loaded)) {
      apply_basic_state(loaded);  // never apply_state(): see this function's header comment
      current_preset_ = id;
    }
  }
  if (root["psave"].is<int>()) {
    int id = root["psave"].as<int>();
    JsonDocument doc;
    JsonObject obj = doc.to<JsonObject>();
    serialize_state(obj);
    obj["ps"] = -1;  // never save a live "ps" self-reference - see apply_basic_state()'s comment
    if (presets_.save(id, obj)) current_preset_ = id;
  }
  if (root["pdel"].is<int>()) {
    presets_.remove(root["pdel"].as<int>());
    if (current_preset_ == root["pdel"].as<int>()) current_preset_ = -1;
  }
}

void JsonApi::broadcast_state() {
  if (ws_.count() == 0) return;
  JsonDocument doc;
  serialize_state(doc.to<JsonObject>());
  String out;
  serializeJson(doc, out);
  ws_.textAll(out);
}

void JsonApi::handle_ws_event(AsyncWebSocket *server, AsyncWebSocketClient *client,
                               AwsEventType type, void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    JsonDocument doc;
    serialize_state(doc.to<JsonObject>());
    String out;
    serializeJson(doc, out);
    client->text(out);
    return;
  }
  if (type != WS_EVT_DATA) return;

  AwsFrameInfo *info = static_cast<AwsFrameInfo *>(arg);
  if (!info->final || info->index != 0 || info->len != len || info->opcode != WS_TEXT) return;

  JsonDocument doc;
  if (deserializeJson(doc, data, len) != DeserializationError::Ok) return;
  apply_state(doc.as<JsonObjectConst>());
  broadcast_state();
}

void JsonApi::begin(AsyncWebServer &server) {
  presets_.begin();

  ws_.onEvent([this](AsyncWebSocket *s, AsyncWebSocketClient *c, AwsEventType type,
                      void *arg, uint8_t *data, size_t len) {
    handle_ws_event(s, c, type, arg, data, len);
  });
  server.addHandler(&ws_);

  server.on("/json/state", HTTP_GET, [this](AsyncWebServerRequest *request) {
    JsonDocument doc;
    serialize_state(doc.to<JsonObject>());
    AsyncResponseStream *response = request->beginResponseStream("application/json");
    serializeJson(doc, *response);
    request->send(response);
  });

  server.on("/json/info", HTTP_GET, [this](AsyncWebServerRequest *request) {
    JsonDocument doc;
    serialize_info(doc.to<JsonObject>());
    AsyncResponseStream *response = request->beginResponseStream("application/json");
    serializeJson(doc, *response);
    request->send(response);
  });

  // "/json/eff" and "/json/pal" are the actual paths real WLED clients (the
  // app, Home Assistant's integration) request - "/json/effects" and
  // "/json/palettes" are kept alongside for anyone hitting the more
  // readable names directly (this firmware's own control page doesn't use
  // either).
  auto serve_effects = [this](AsyncWebServerRequest *request) {
    JsonDocument doc;
    serialize_effects(doc.to<JsonArray>());
    AsyncResponseStream *response = request->beginResponseStream("application/json");
    serializeJson(doc, *response);
    request->send(response);
  };
  server.on("/json/effects", HTTP_GET, serve_effects);
  server.on("/json/eff", HTTP_GET, serve_effects);

  // Not a real WLED endpoint - /json/eff has to stay real WLED's dense,
  // index-aligned name array for compatibility (see effects.h), so it
  // lists plenty of effects (audio-reactive, particle-system, 2D
  // scrolling text) this firmware can't actually render. This is how the
  // built-in control page (src/net/captive_portal.cpp) tells those apart
  // from ones that'll do something, without touching that array.
  server.on("/json/implemented", HTTP_GET, [this](AsyncWebServerRequest *request) {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (size_t i = 0; i < kEffectCount; i++) arr.add(effects::is_implemented(static_cast<effects::Id>(i)));
    AsyncResponseStream *response = request->beginResponseStream("application/json");
    serializeJson(doc, *response);
    request->send(response);
  });

  auto serve_palettes = [this](AsyncWebServerRequest *request) {
    JsonDocument doc;
    serialize_palettes(doc.to<JsonArray>());
    AsyncResponseStream *response = request->beginResponseStream("application/json");
    serializeJson(doc, *response);
    request->send(response);
  };
  server.on("/json/palettes", HTTP_GET, serve_palettes);
  server.on("/json/pal", HTTP_GET, serve_palettes);

  server.on("/presets.json", HTTP_GET, [this](AsyncWebServerRequest *request) {
    presets_.serve(request);
  });

  // "/json/si" (state+info, no effects/palettes arrays) is what real WLED
  // clients poll routinely - "/json" (the full object, effects/palettes
  // included) is what they fetch once on first connect.
  server.on("/json/si", HTTP_GET, [this](AsyncWebServerRequest *request) {
    JsonDocument doc;
    JsonObject root = doc.to<JsonObject>();
    serialize_state(root["state"].to<JsonObject>());
    serialize_info(root["info"].to<JsonObject>());
    AsyncResponseStream *response = request->beginResponseStream("application/json");
    serializeJson(doc, *response);
    request->send(response);
  });

  server.on("/json", HTTP_GET, [this](AsyncWebServerRequest *request) {
    JsonDocument doc;
    JsonObject root = doc.to<JsonObject>();
    serialize_state(root["state"].to<JsonObject>());
    serialize_info(root["info"].to<JsonObject>());
    serialize_effects(root["effects"].to<JsonArray>());
    serialize_palettes(root["palettes"].to<JsonArray>());
    AsyncResponseStream *response = request->beginResponseStream("application/json");
    serializeJson(doc, *response);
    request->send(response);
  });

  auto *state_post = new AsyncCallbackJsonWebHandler(
      "/json/state", [this](AsyncWebServerRequest *request, JsonVariant &json) {
        apply_state(json.as<JsonObjectConst>());
        broadcast_state();

        JsonDocument doc;
        serialize_state(doc.to<JsonObject>());
        AsyncResponseStream *response = request->beginResponseStream("application/json");
        serializeJson(doc, *response);
        request->send(response);
      });
  state_post->setMethod(HTTP_POST);
  server.addHandler(state_post);
}
