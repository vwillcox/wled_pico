#pragma once

#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>

#include "effects/effects.h"
#include "preset_store.h"

// Milestone 4 gave this class WLED's actual /json/state and /json/info
// schema (see wled/WLED's wled00/json.cpp). Milestone 5 adds the parts of
// that same file's preset handling (psave/ps/pdel fields on POST
// /json/state) and a /ws WebSocket that pushes state on every change and
// accepts the same JSON this class's HTTP POST handler does - matching
// real WLED's live-preview behavior, where any connected client (this
// firmware's own page, a second browser tab, a real WLED app) sees changes
// made anywhere else immediately instead of having to poll.
class JsonApi {
 public:
  // Registers /json, /json/state, /json/info, /json/effects,
  // /json/palettes, /presets.json, and /ws on `server`. Does not call
  // server.begin() - the caller does that once, after every module has
  // registered its routes.
  void begin(AsyncWebServer &server);

  // main.cpp reads these every loop() iteration to drive the display -
  // this class owns the "current state" so both HTTP/WS handlers and the
  // render loop see the same numbers.
  bool on() const { return on_; }
  uint8_t brightness() const { return bri_; }
  effects::Id effect_id() const { return effect_id_; }
  const effects::Params &effect_params() const { return params_; }

  // main.cpp calls this every loop() iteration with RealtimeUdp::active() -
  // JsonApi reports it back out as info.live so real WLED clients (the app,
  // Home Assistant) show the same "receiving realtime data" state real WLED
  // would.
  void set_live(bool live) { live_ = live; }

 private:
  bool on_ = true;
  uint8_t bri_ = 128;
  effects::Id effect_id_ = effects::Id::kRainbow;
  effects::Params params_;
  int current_preset_ = -1;
  bool live_ = false;

  PresetStore presets_;
  AsyncWebSocket ws_{"/ws"};

  void serialize_state(JsonObject root) const;
  void serialize_info(JsonObject root) const;
  void serialize_effects(JsonArray arr) const;

  // Applies on/bri/seg[0].{fx,sx,ix,col} from `root`, then handles
  // WLED's psave/ps/pdel preset fields (which need the *result* of that
  // application to snapshot, in psave's case) - so this does both steps
  // real WLED splits across deserializeState() and handleSet().
  void apply_state(JsonObjectConst root);

  void broadcast_state();
  void handle_ws_event(AsyncWebSocket *server, AsyncWebSocketClient *client,
                        AwsEventType type, void *arg, uint8_t *data, size_t len);
};
