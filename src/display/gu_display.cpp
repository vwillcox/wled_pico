#include "gu_display.h"

#include <math.h>

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "pico/time.h"

#include "cosmic_unicorn.pio.h"  // generated at build time from cosmic_unicorn.pio
#include "gu_pins.h"

using namespace gu_pins;

// --- gamma -------------------------------------------------------------
//
// The BCD frame count (14) fixes the driver's brightness resolution at
// 14 bits (0-16383). Pimoroni ships a precomputed GAMMA_14BIT lookup table
// tuned against the matrix's actual LEDs; we don't have those exact values,
// so this is a standard gamma-2.8 approximation instead. Good enough to
// confirm the driver works — revisit if colors look off against reference
// photos/videos of the real firmware.
void GuDisplay::init_gamma_lut() {
  for (int i = 0; i < 256; i++) {
    float normalized = i / 255.0f;
    gamma_lut_[i] = static_cast<uint16_t>(powf(normalized, 2.8f) * 16383.0f + 0.5f);
  }
}

// --- bitstream header fields --------------------------------------------
//
// Layout per BCD frame (72 bytes), matching cosmic_unicorn.cpp's init()/
// set_pixel() and the .pio program's consumption order:
//
//   byte  0      : row pixel count - 1 (2*WIDTH - 1 = 63), constant
//   byte  1      : row select (low 4 bits -> ROW_BIT_0..3, one of 16), constant
//   bytes 2..65  : per-pixel xxxxxbgr byte, 64 of them (2 rows' worth -
//                  see set_pixel()), written by set_pixel()
//   bytes 66..67 : dummy/alignment, discarded by the PIO program
//   bytes 68..71 : bcd tick count (1 << frame), little-endian uint32
void GuDisplay::init_bitstream_headers() {
  for (uint8_t row = 0; row < ROW_COUNT; row++) {
    for (uint8_t frame = 0; frame < BCD_FRAME_COUNT; frame++) {
      uint8_t *p = &bitstream_[row * ROW_BYTES + BCD_FRAME_BYTES * frame];

      p[0] = 2 * WIDTH - 1;
      p[1] = row;

      uint32_t bcd_ticks = 1UL << frame;
      p[68] = (bcd_ticks >> 0) & 0xff;
      p[69] = (bcd_ticks >> 8) & 0xff;
      p[70] = (bcd_ticks >> 16) & 0xff;
      p[71] = (bcd_ticks >> 24) & 0xff;
    }
  }
}

// --- LED driver chip bring-up -------------------------------------------
//
// The column shift registers are LED driver chips with a configurable
// output-current register. This bit-bangs "full current" into all 12
// cascaded chips (32-wide, doubled to 64 columns of shift-register bits
// per BCD frame, needs more chips than the Galactic Unicorn's 53-wide
// panel) before the PIO takes over the same clock/data lines. Ported
// as-is from Pimoroni's init() — plain GPIO bit-banging, no PIO involved.
void GuDisplay::init_gpio_and_driver_current() {
  gpio_init(COLUMN_CLOCK); gpio_set_dir(COLUMN_CLOCK, GPIO_OUT); gpio_put(COLUMN_CLOCK, false);
  gpio_init(COLUMN_DATA);  gpio_set_dir(COLUMN_DATA, GPIO_OUT);  gpio_put(COLUMN_DATA, false);
  gpio_init(COLUMN_LATCH); gpio_set_dir(COLUMN_LATCH, GPIO_OUT); gpio_put(COLUMN_LATCH, false);
  gpio_init(COLUMN_BLANK); gpio_set_dir(COLUMN_BLANK, GPIO_OUT); gpio_put(COLUMN_BLANK, true);

  // Non-visible row while we bring things up, to avoid a startup flash.
  gpio_init(ROW_BIT_0); gpio_set_dir(ROW_BIT_0, GPIO_OUT); gpio_put(ROW_BIT_0, true);
  gpio_init(ROW_BIT_1); gpio_set_dir(ROW_BIT_1, GPIO_OUT); gpio_put(ROW_BIT_1, true);
  gpio_init(ROW_BIT_2); gpio_set_dir(ROW_BIT_2, GPIO_OUT); gpio_put(ROW_BIT_2, true);
  gpio_init(ROW_BIT_3); gpio_set_dir(ROW_BIT_3, GPIO_OUT); gpio_put(ROW_BIT_3, true);

  sleep_ms(100);

  const uint16_t reg1 = 0b1111111111001110;  // full output current, register 2

  // Clock the value into the first 11 of 12 cascaded driver chips.
  for (int chip = 0; chip < 11; chip++) {
    for (int bit = 0; bit < 16; bit++) {
      gpio_put(COLUMN_DATA, (reg1 & (1U << (15 - bit))) != 0);
      sleep_us(10);
      gpio_put(COLUMN_CLOCK, true);
      sleep_us(10);
      gpio_put(COLUMN_CLOCK, false);
    }
  }

  // Clock the last chip and latch the whole cascade.
  for (int bit = 0; bit < 16; bit++) {
    gpio_put(COLUMN_DATA, (reg1 & (1U << (15 - bit))) != 0);
    sleep_us(10);
    gpio_put(COLUMN_CLOCK, true);
    sleep_us(10);
    gpio_put(COLUMN_CLOCK, false);
    if (bit == 4) gpio_put(COLUMN_LATCH, true);
  }
  gpio_put(COLUMN_LATCH, false);

  // Blank pulse clears a slight glow the latch above tends to leave behind.
  gpio_put(COLUMN_BLANK, false);
  sleep_us(10);
  gpio_put(COLUMN_BLANK, true);
}

// --- PIO + DMA bring-up ---------------------------------------------------
//
// Two chained DMA channels loop forever: dma_ctrl_chan_ reloads
// dma_chan_'s read address back to the start of bitstream_ every time
// dma_chan_ finishes, so the whole buffer streams out to the PIO's TX FIFO
// on repeat with zero CPU involvement once this returns.
void GuDisplay::init_pio_and_dma() {
  pio_ = pio0;
  sm_ = pio_claim_unused_sm(pio_, true);
  sm_offset_ = pio_add_program(pio_, &cosmic_unicorn_program);

  pio_gpio_init(pio_, COLUMN_CLOCK);
  pio_gpio_init(pio_, COLUMN_DATA);
  pio_gpio_init(pio_, COLUMN_LATCH);
  pio_gpio_init(pio_, COLUMN_BLANK);
  pio_gpio_init(pio_, ROW_BIT_0);
  pio_gpio_init(pio_, ROW_BIT_1);
  pio_gpio_init(pio_, ROW_BIT_2);
  pio_gpio_init(pio_, ROW_BIT_3);

  const uint pins_to_set = (1u << COLUMN_BLANK) | (0b1111u << ROW_BIT_0);
  pio_sm_set_pins_with_mask(pio_, sm_, pins_to_set, pins_to_set);
  pio_sm_set_consecutive_pindirs(pio_, sm_, COLUMN_CLOCK, 8, true);

  pio_sm_config c = cosmic_unicorn_program_get_default_config(sm_offset_);
  sm_config_set_out_shift(&c, true, true, 32);
  sm_config_set_out_pins(&c, ROW_BIT_0, 4);
  sm_config_set_set_pins(&c, COLUMN_DATA, 3);
  sm_config_set_sideset_pins(&c, COLUMN_CLOCK);
  sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);

  dma_chan_ = dma_claim_unused_channel(true);
  dma_ctrl_chan_ = dma_claim_unused_channel(true);

  dma_channel_config ctrl_config = dma_channel_get_default_config(dma_ctrl_chan_);
  channel_config_set_transfer_data_size(&ctrl_config, DMA_SIZE_32);
  channel_config_set_read_increment(&ctrl_config, false);
  channel_config_set_write_increment(&ctrl_config, false);
  channel_config_set_chain_to(&ctrl_config, dma_chan_);

  dma_channel_configure(
      dma_ctrl_chan_, &ctrl_config,
      &dma_hw->ch[dma_chan_].read_addr,
      &bitstream_addr_,
      1, false);

  dma_channel_config config = dma_channel_get_default_config(dma_chan_);
  channel_config_set_transfer_data_size(&config, DMA_SIZE_32);
  channel_config_set_bswap(&config, false);
  channel_config_set_dreq(&config, pio_get_dreq(pio_, sm_, true));
  channel_config_set_chain_to(&config, dma_ctrl_chan_);

  dma_channel_configure(
      dma_chan_, &config,
      &pio_->txf[sm_],
      nullptr,
      BITSTREAM_LENGTH / 4, false);

  pio_sm_init(pio_, sm_, sm_offset_, &c);
  pio_sm_set_enabled(pio_, sm_, true);

  dma_start_channel_mask(1u << dma_ctrl_chan_);
}

void GuDisplay::begin() {
  init_gamma_lut();
  init_bitstream_headers();
  init_gpio_and_driver_current();
  init_pio_and_dma();
}

void GuDisplay::set_pixel(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
  if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) return;

  // Physical wiring runs the matrix in reverse on both axes.
  x = (WIDTH - 1) - x;
  y = (HEIGHT - 1) - y;

  // This is a 1-in-2 row-multiplexed panel: only 16 physical row-select
  // addresses exist for 32 rows. Each address's 64-byte pixel-data block
  // is really two side-by-side 32-byte halves - one for that row, one for
  // the row 16 below/above it - selected by which half of the block you
  // write into. Ported from cosmic_unicorn.cpp's set_pixel().
  if (y < 16) {
    x += 32;
  } else {
    y -= 16;
  }

  r = (r * brightness_) >> 8;
  g = (g * brightness_) >> 8;
  b = (b * brightness_) >> 8;

  uint16_t gamma_r = gamma_lut_[r];
  uint16_t gamma_g = gamma_lut_[g];
  uint16_t gamma_b = gamma_lut_[b];

  for (uint8_t frame = 0; frame < BCD_FRAME_COUNT; frame++) {
    uint8_t *p = &bitstream_[y * ROW_BYTES + BCD_FRAME_BYTES * frame + 2 + x];

    uint8_t red_bit = gamma_r & 1;
    uint8_t green_bit = gamma_g & 1;
    uint8_t blue_bit = gamma_b & 1;

    *p = (blue_bit << 0) | (green_bit << 1) | (red_bit << 2);

    gamma_r >>= 1;
    gamma_g >>= 1;
    gamma_b >>= 1;
  }
}

void GuDisplay::set_brightness(float value) {
  if (value < 0.0f) value = 0.0f;
  if (value > 1.0f) value = 1.0f;
  brightness_ = static_cast<uint16_t>(floorf(value * 256.0f));
}
