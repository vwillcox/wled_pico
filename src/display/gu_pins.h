#pragma once

#include <cstdint>

// Cosmic Unicorn (Pico W Aboard) GPIO map.
//
// Verbatim from Pimoroni's own driver (MIT licensed):
// https://github.com/pimoroni/pimoroni-pico/blob/main/libraries/cosmic_unicorn/cosmic_unicorn.hpp
//
// Identical pinout to Pimoroni's other Pico W matrix boards (Galactic
// Unicorn included) - only the matrix geometry and row-multiplexing in
// gu_display.cpp differ between boards. This is board-specific wiring,
// not something to rederive — if you're porting this to a different
// RP2040 matrix board, check both this file and gu_display.cpp.
namespace gu_pins {

// LED matrix shift-register / row-select control
constexpr uint8_t COLUMN_CLOCK = 13;
constexpr uint8_t COLUMN_DATA  = 14;
constexpr uint8_t COLUMN_LATCH = 15;
constexpr uint8_t COLUMN_BLANK = 16;

constexpr uint8_t ROW_BIT_0 = 17;
constexpr uint8_t ROW_BIT_1 = 18;
constexpr uint8_t ROW_BIT_2 = 19;
constexpr uint8_t ROW_BIT_3 = 20;

// Not used yet (future milestones)
constexpr uint8_t LIGHT_SENSOR = 28;
constexpr uint8_t MUTE         = 22;

constexpr uint8_t SWITCH_A = 0;
constexpr uint8_t SWITCH_B = 1;
constexpr uint8_t SWITCH_C = 3;
constexpr uint8_t SWITCH_D = 6;

constexpr uint8_t SWITCH_SLEEP           = 27;
constexpr uint8_t SWITCH_VOLUME_UP       = 7;
constexpr uint8_t SWITCH_VOLUME_DOWN     = 8;
constexpr uint8_t SWITCH_BRIGHTNESS_UP   = 21;
constexpr uint8_t SWITCH_BRIGHTNESS_DOWN = 26;

}  // namespace gu_pins
