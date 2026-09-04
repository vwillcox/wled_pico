#include "realtime_udp.h"

#include "display/gu_display.h"

void RealtimeUdp::begin() { udp_.begin(kPort); }

void RealtimeUdp::set_pixel_id(unsigned id, uint8_t r, uint8_t g, uint8_t b) {
  if (id >= kPixelCount) return;
  pixels_[id][0] = r;
  pixels_[id][1] = g;
  pixels_[id][2] = b;
}

// Wire format matches wled00/udp.cpp's handleNotifications() exactly: byte0
// selects the mode (1=WARLS 2=DRGB 3=DRGBW 4=DNRGB 5=DNRGBW), byte1 is the
// realtime timeout in seconds (0 cancels immediately). White channel bytes
// (DRGBW/DNRGBW) are read past but dropped - this display has no white LEDs.
void RealtimeUdp::handle_packet(const uint8_t *udpIn, size_t packetSize) {
  if (packetSize < 2 || udpIn[0] == 0 || udpIn[0] > 5) return;

  if (udpIn[1] == 0) {
    timeout_at_ms_ = 0;  // cancel realtime mode immediately
    return;
  }
  timeout_at_ms_ = millis() + static_cast<uint32_t>(udpIn[1]) * 1000 + 1;

  // Bounds below are copied verbatim from wled00/udp.cpp, not re-derived -
  // WLED's own DNRGB/DNRGBW loops both bound on `packetSize - 2` despite
  // the different stride, which looks asymmetric but is what real senders
  // are built against.
  if (udpIn[0] == 1 && packetSize > 5) {  // WARLS: [id, r, g, b] repeated
    for (size_t i = 2; i < packetSize - 3; i += 4) {
      set_pixel_id(udpIn[i], udpIn[i + 1], udpIn[i + 2], udpIn[i + 3]);
    }
  } else if (udpIn[0] == 2 && packetSize > 4) {  // DRGB: sequential [r,g,b] from id 0
    unsigned id = 0;
    for (size_t i = 2; i < packetSize - 2 && id < kPixelCount; i += 3, id++) {
      set_pixel_id(id, udpIn[i], udpIn[i + 1], udpIn[i + 2]);
    }
  } else if (udpIn[0] == 3 && packetSize > 6) {  // DRGBW: sequential [r,g,b,w] from id 0
    unsigned id = 0;
    for (size_t i = 2; i < packetSize - 3 && id < kPixelCount; i += 4, id++) {
      set_pixel_id(id, udpIn[i], udpIn[i + 1], udpIn[i + 2]);
    }
  } else if (udpIn[0] == 4 && packetSize > 7) {  // DNRGB: 16-bit start id, then [r,g,b]
    unsigned id = (static_cast<unsigned>(udpIn[2]) << 8) | udpIn[3];
    for (size_t i = 4; i < packetSize - 2 && id < kPixelCount; i += 3, id++) {
      set_pixel_id(id, udpIn[i], udpIn[i + 1], udpIn[i + 2]);
    }
  } else if (udpIn[0] == 5 && packetSize > 8) {  // DNRGBW: 16-bit start id, then [r,g,b,w]
    unsigned id = (static_cast<unsigned>(udpIn[2]) << 8) | udpIn[3];
    for (size_t i = 4; i < packetSize - 2 && id < kPixelCount; i += 4, id++) {
      set_pixel_id(id, udpIn[i], udpIn[i + 1], udpIn[i + 2]);
    }
  }
}

void RealtimeUdp::loop_tick() {
  size_t packet_size = udp_.parsePacket();
  if (!packet_size || packet_size > kMaxPacket) return;

  uint8_t buf[kMaxPacket];
  size_t len = udp_.read(buf, packet_size);
  handle_packet(buf, len);
}

bool RealtimeUdp::active() const { return timeout_at_ms_ != 0 && millis() < timeout_at_ms_; }

void RealtimeUdp::render(GuDisplay &display) const {
  for (int y = 0; y < GuDisplay::HEIGHT; y++) {
    for (int x = 0; x < GuDisplay::WIDTH; x++) {
      const uint8_t *px = pixels_[y * GuDisplay::WIDTH + x];
      display.set_pixel(x, y, px[0], px[1], px[2]);
    }
  }
}
