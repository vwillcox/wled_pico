#pragma once

#include <cstdint>

#include "hardware/pio.h"

// Cosmic Unicorn matrix driver: owns the PIO/DMA bring-up and exposes a
// flat set_pixel(x, y, r, g, b), the same shape WLED's effects engine
// expects from its LED backend. This is a trimmed-down reimplementation of
// Pimoroni's CosmicUnicorn class (cosmic_unicorn.cpp/.hpp) — the pin map,
// bitstream layout, and PIO program are theirs (vendored, see gu_pins.h /
// cosmic_unicorn.pio); audio/synth support and the button/light sensor
// helpers are deliberately left out of this milestone.
//
// This board is a 1-in-2 row-multiplexed 32x32 matrix: there are only 16
// physical row-select addresses, and each one drives two rows of the
// display simultaneously (see set_pixel()'s y < 16 remap in the .cpp).
// That's different from - and was originally confused with - the Galactic
// Unicorn, a 53x11 board with one row-select address per row; if you're
// diffing this against that board's driver, the row-doubling is the
// actual delta, not a mistake.
class GuDisplay {
 public:
  static constexpr int WIDTH = 32;
  static constexpr int HEIGHT = 32;

  // Bring up GPIOs, driver-chip current config, and the PIO/DMA chain that
  // continuously scans the bitstream buffer out to the matrix. Safe to call
  // once from setup().
  void begin();

  // x in [0, WIDTH), y in [0, HEIGHT). Out-of-range calls are ignored.
  // Writes take effect on the next PIO scan pass (no explicit flush/update
  // call needed — DMA is already looping over the buffer in the background).
  void set_pixel(int x, int y, uint8_t r, uint8_t g, uint8_t b);

  // 0.0 - 1.0. Matches Pimoroni's fixed-point (value * 256) internal scale.
  void set_brightness(float value);

 private:
  static constexpr uint32_t ROW_COUNT = 16;        // physical row-select addresses, not HEIGHT
  static constexpr uint32_t BCD_FRAME_COUNT = 14;  // 14-bit brightness resolution
  static constexpr uint32_t BCD_FRAME_BYTES = 72;  // 2 header + 64 (2*WIDTH) pixel + 2 dummy + 4 bcd-tick
  static constexpr uint32_t ROW_BYTES = BCD_FRAME_COUNT * BCD_FRAME_BYTES;
  static constexpr uint32_t BITSTREAM_LENGTH = ROW_COUNT * ROW_BYTES;

  // Must be 32-bit aligned: the DMA channel below reads it 4 bytes at a time.
  alignas(4) uint8_t bitstream_[BITSTREAM_LENGTH] = {0};
  const uint32_t bitstream_addr_ = reinterpret_cast<uint32_t>(bitstream_);

  uint16_t gamma_lut_[256] = {0};
  uint16_t brightness_ = 256;  // 0..256 fixed point, see set_brightness()

  PIO pio_ = pio0;
  uint sm_ = 0;
  uint sm_offset_ = 0;
  uint dma_chan_ = 0;
  uint dma_ctrl_chan_ = 0;

  void init_gamma_lut();
  void init_bitstream_headers();
  void init_gpio_and_driver_current();
  void init_pio_and_dma();
};
