// Milestone 1: display driver bring-up. Milestone 2: WiFi AP + captive
// portal. Milestone 3: WLED's effect math ported onto the pixel buffer.
// Milestone 4: WLED's real /json/state + /json/info API. Milestone 5:
// presets (LittleFS, src/api/preset_store.cpp), a /ws live-state channel
// (src/api/json_api.cpp), and web-based OTA updates (src/api/ota.cpp) -
// no more BOOTSEL button once this is running. See README.md for the
// full plan.

#include <Arduino.h>
#include <ESP8266mDNS.h>
#include <WiFi.h>

#include "api/json_api.h"
#include "api/ota.h"
#include "display/gu_display.h"
#include "effects/effects.h"
#include "net/captive_portal.h"
#include "net/device_id.h"
#include "net/realtime_udp.h"

namespace {

GuDisplay display;
CaptivePortal portal;
JsonApi api;
Ota ota;
RealtimeUdp realtime;

// mDNS advertisement (_wled._tcp + _http._tcp, TXT "mac") - what the WLED
// app and Home Assistant's WLED integration use to auto-discover the
// device, instead of the user typing in an IP. See wled/WLED's wled.cpp
// (search "MDNS.addService") for the shape this mirrors.
void begin_mdns() {
  if (!MDNS.begin("wled-pico")) return;
  MDNS.addService("http", "tcp", 80);
  MDNS.addService("wled", "tcp", 80);
  MDNS.addServiceTxt("wled", "tcp", "mac", device_mac().c_str());
}

}  // namespace

void setup() {
  Serial.begin(115200);
  display.begin();

  portal.begin("WLED-Pico-Setup");
  api.begin(portal.server());
  ota.begin(portal.server());
  portal.server().begin();

  begin_mdns();
  realtime.begin();

  Serial.print("AP up, SSID=WLED-Pico-Setup IP=");
  Serial.println(WiFi.softAPIP());

  // Hardware watchdog: fed every loop() iteration below. If anything ever
  // blocks the firmware for more than ~8s - the WiFi join hang that
  // prompted this being the concrete case - the RP2040 force-resets
  // itself instead of staying stuck until someone notices and does a
  // physical BOOTSEL recovery. 8000ms is close to this chip's ~8.3s
  // hardware ceiling (a 24-bit counter), so it's the longest single
  // timeout available, not an arbitrary choice.
  rp2040.wdt_begin(8000);
}

void loop() {
  rp2040.wdt_reset();
  portal.loop_tick();

  // Re-arm mDNS on every AP<->STA transition (join succeeding, a join
  // timing out and falling back to the AP, "Forget saved WiFi") - it's
  // bound to whichever interface was up at MDNS.begin() time, so a mode
  // switch without this leaves it advertising on an interface that's gone.
  // Real WLED re-does MDNS.end()+begin() for the same reason on its own
  // network-(re)init path.
  static bool wifi_was_connected = false;
  bool wifi_connected = WiFi.status() == WL_CONNECTED;
  if (wifi_connected != wifi_was_connected) {
    MDNS.end();
    begin_mdns();
    wifi_was_connected = wifi_connected;
  }
  MDNS.update();

  realtime.loop_tick();
  api.set_live(realtime.active());

  display.set_brightness(api.on() ? api.brightness() / 255.0f : 0.0f);

  // Realtime UDP data (real WLED's "Sync"/pixel-pusher tools) overrides the
  // effects engine while it's live, same as real WLED's realtimeMode does -
  // see RealtimeUdp's own comment for the protocol this mirrors.
  if (realtime.active()) {
    realtime.render(display);
  } else {
    effects::render(api.effect_id(), display, millis(), api.effect_params());
  }

  delay(16);  // ~60 updates/sec; PIO/DMA scan itself runs at ~300fps independent of this
}
