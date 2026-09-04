#pragma once

#include <WiFiUdp.h>
#include <cstdint>

class GuDisplay;

// Real WLED's UDP realtime-pixel protocols (WARLS/DRGB/DRGBW/DNRGB/DNRGBW,
// port 21324) - the wire format screen-capture "sync to WLED" tools,
// Hyperion-style pixel pushers, and other WLED instances' "Send realtime
// notifications" feature all speak, so this device works with them without
// requiring the JSON API. Ported directly from wled/WLED's
// wled00/udp.cpp's handleNotifications() (the `udpIn[0] > 0 && udpIn[0] < 6`
// branch) - same header bytes, same per-mode payload layout. Two things
// that file also handles are deliberately out of scope: the plain "wled
// notifier" protocol (byte0==0, multi-controller state sync - this board is
// always the only unit) and TPM2.NET/Hyperion's own raw-RGB port/Art-Net/
// E1.31 (separate protocols, not the WLED-native one this targets).
//
// Pixel id is row-major (id = y*WIDTH + x), matching WLED's own default 2D
// mapping for a matrix with no custom ledmap.json - the same assumption a
// sender configured with this device's /json/info leds.matrix {w,h} would
// make.
class RealtimeUdp {
 public:
  static constexpr uint16_t kPort = 21324;

  void begin();

  // Parses one pending packet (if any), updating the pixel buffer and the
  // realtime timeout. Call every loop() iteration, same as WLED's own
  // handleNotifications().
  void loop_tick();

  // True while realtime data is live (a packet arrived within the timeout
  // it declared) - main.cpp checks this to bypass the effects engine and
  // push the received frame straight to the display, matching real WLED's
  // realtime-override behavior.
  bool active() const;

  // Pushes the last-received frame to `display`. Only meaningful when
  // active() - main.cpp is expected to check that first.
  void render(GuDisplay &display) const;

 private:
  static constexpr unsigned kPixelCount = 32 * 32;  // GuDisplay::WIDTH * HEIGHT
  static constexpr size_t kMaxPacket = 1472;   // UDP_IN_MAXSIZE in real WLED

  WiFiUDP udp_;
  uint8_t pixels_[kPixelCount][3] = {{0, 0, 0}};
  uint32_t timeout_at_ms_ = 0;  // 0 = no realtime data received yet / cancelled

  void handle_packet(const uint8_t *buf, size_t len);
  void set_pixel_id(unsigned id, uint8_t r, uint8_t g, uint8_t b);
};
