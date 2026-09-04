#pragma once

#include <DNSServer.h>
#include <ESPAsyncWebServer.h>

// WiFi AP + DNS captive portal + the served control page. The page itself
// talks to the real WLED-shaped JSON API (see src/api/json_api.h) rather
// than one-off endpoints.
//
// The "WLED-Pico-Setup" AP runs by default. The page's "Join WiFi" form
// (POST /wifi {ssid,pass}) drops the AP and switches to WIFI_STA to join
// an existing network - the Pico W's CYW43 WiFi doesn't reliably run
// AP+STA concurrently (station association just never completes), and
// this is also how real WLED normally operates: AP as onboarding/fallback
// only, not alongside a joined network. If the join doesn't complete
// within a few seconds, loop_tick() automatically restores the AP so
// there's no way to get locked out.
//
// Credentials ARE persisted (LittleFS, /wifi.json) and auto-rejoined on
// boot - safe to do because of two things working together: the
// AP-restoring fallback above (a boot-time join that never completes just
// leaves you on the AP, same as never having joined at all), and the
// hardware watchdog armed in main.cpp, which recovers a genuinely hung
// join attempt (this chip's WiFi occasionally does hang, not just fail)
// without needing a physical BOOTSEL reset.
class CaptivePortal {
 public:
  // Brings up the AP, starts the wildcard DNS responder, registers this
  // class's own routes ("/", "/wifi", and the captive-portal catch-all),
  // and - if /wifi.json has saved credentials - kicks off a join attempt
  // to them immediately. Does NOT call server().begin() - callers that
  // need to register additional routes (see JsonApi) must do so before
  // calling server().begin() themselves, once everything is registered.
  void begin(const char *ap_ssid);

  // Call every loop() iteration. Services DNSServer, and drives the
  // join-WiFi timeout/fallback state machine.
  void loop_tick();

  AsyncWebServer &server() { return server_; }

  // Same logic the "/wifi" POST handler runs (and persists the
  // credentials to LittleFS for next boot) - exposed so it can also be
  // triggered directly, e.g. from begin() on a saved-credentials boot.
  void join(const char *ssid, const char *pass);

 private:
  static constexpr uint32_t kStaConnectTimeoutMs = 25000;
  static constexpr const char *kCredsPath = "/wifi.json";

  DNSServer dns_;
  AsyncWebServer server_{80};
  const char *ap_ssid_ = nullptr;

  bool sta_attempted_ = false;
  bool sta_failed_ = false;
  uint32_t sta_attempt_start_ms_ = 0;

  void save_credentials(const char *ssid, const char *pass);
  bool load_and_join_saved_credentials();
};
