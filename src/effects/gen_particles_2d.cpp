#include "effects.h"
#include "wled_compat.h"
#include "palettes.h"
#include "particle_system_2d.h"
#include "display/gu_display.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

// Port of the 11 real-WLED 2D-particle-system effects assigned to this
// batch (wled00/FX.cpp's mode_particle*() functions, all built on top of
// ParticleSystem2D - see particle_system_2d.h for the engine itself,
// already fully ported). Every effect below is close to a 1:1
// transliteration of its real WLED source, using the same engine calls
// under this port's naming (ps2d::* instead of PartSys->*, see
// particle_system_2d.h's own comments for the exact mapping).
//
// Two deviations needed by every effect here, not requested by any single
// one of them, called out once instead of at each site:
//  - Real WLED's ESP8266-specific branches (`#ifdef ESP8266`: shorter
//    particle lifetimes, an "every other frame" emit skip, a static-
//    particle anti-flicker hack in mode_particlevortex()) are dropped -
//    this board is resource-comparable to WLED's non-ESP8266 tier, and
//    every other batch in this codebase already targets that tier only.
//  - `Params::custom3` spans 0-255 in this port (effects.h) where real
//    WLED's SEGMENT.custom3 is a native 5-bit UI slider, 0-31 (it's
//    packed into the same byte as three 1-bit check flags upstream).
//    Effects below that use custom3 through an explicit
//    `map(custom3, 0, 31, lo, hi)` call use `map_range(custom3, 0, 255,
//    lo, hi)` instead - same idea, more resolution, matching
//    gen_batch7.cpp's own precedent for this exact situation. Effects
//    that use custom3 directly in 0-31-domain bit-twiddling or sentinel
//    comparisons (`>>1`, `==31`, `<31`) instead rescale it back down to
//    WLED's native 0-31 range first with custom3_native() below, so
//    those exact constants/sentinels keep meaning what they meant
//    upstream.
namespace effects {
namespace {

// ---------------------------------------------------------------------
// Shared helpers for this batch (local to this file - see gen_batch6.cpp's
// own precedent for duplicating small helpers per translation unit rather
// than growing wled_compat.h).
// ---------------------------------------------------------------------

// Arduino's map(), integer, not clamped - same as effects.cpp's private
// helper of the same name, duplicated here per this file's own top
// comment on small per-TU helpers.
long map_range(long x, long in_min, long in_max, long out_min, long out_max) {
  return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

uint8_t custom3_native(uint8_t custom3) {
  return static_cast<uint8_t>((static_cast<uint32_t>(custom3) * 31) / 255);
}

// Stand-in for real WLED's sqrt32_bw() (wled00/util.cpp's integer binary-
// search sqrt) - this codebase has no integer sqrt helper; sqrtf() over
// the same non-negative domain matches closely enough for the distance-
// based motion mode_particlegalaxy() needs it for (same "matches the
// shape, not bit-exact" tolerance wled_compat.h documents for its own
// math helpers).
int32_t isqrt(int32_t v) { return v > 0 ? static_cast<int32_t>(sqrtf(static_cast<float>(v))) : 0; }

// wled00/FX.cpp:102 tristate_square8() (file-scope helper there too, not
// exposed via wled_compat.h - only mode_particlebox() below needs it).
int8_t tristate_square8(uint8_t x, uint8_t pulsewidth, uint8_t attdec) {
  int8_t a = 127;
  if (x > 127) {
    a = -127;
    x = static_cast<uint8_t>(x - 127);
  }
  if (x < attdec) return static_cast<int8_t>(static_cast<int16_t>(x) * a / attdec);
  if (x < pulsewidth - attdec) return a;
  if (x < pulsewidth) return static_cast<int8_t>(static_cast<int16_t>(pulsewidth - x) * a / attdec);
  return 0;
}

// Stand-in for real WLED's perlin8() (wled00/util.cpp's lattice-gradient
// Perlin noise with permutation tables this codebase doesn't have):
// hash-based trilinear value noise instead, the same substitution
// effects.cpp's mode_2dplasmarotozoom()/mode_2dsoap() already make for
// FastLED's inoise8() (see that file's value_noise8() - this is the same
// algorithm, duplicated per this file's own top comment on small per-TU
// helpers). Not bit-exact, matches the shape. Inputs are left-shifted
// before hashing so unit steps (as every caller below uses: an
// incrementing aux0 counter, adjacent pixel coordinates, ...) interpolate
// smoothly instead of jumping lattice-cell to lattice-cell.
uint32_t noise_lattice_hash(int32_t x, int32_t y, int32_t z) {
  uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u +
               static_cast<uint32_t>(z) * 2147483647u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return h ^ (h >> 16);
}
uint16_t noise_lerp16(uint16_t a, uint16_t b, uint32_t f) {
  return static_cast<uint16_t>(a + (((static_cast<int32_t>(b) - static_cast<int32_t>(a)) * static_cast<int32_t>(f)) >> 8));
}
uint8_t noise8(int32_t x, int32_t y, int32_t z) {
  x <<= 4;
  y <<= 4;
  z <<= 4;
  int32_t xi = x >> 8, yi = y >> 8, zi = z >> 8;
  uint32_t xf = static_cast<uint32_t>(x) & 0xFF;
  uint32_t yf = static_cast<uint32_t>(y) & 0xFF;
  uint32_t zf = static_cast<uint32_t>(z) & 0xFF;
  uint16_t c000 = static_cast<uint16_t>(noise_lattice_hash(xi, yi, zi));
  uint16_t c100 = static_cast<uint16_t>(noise_lattice_hash(xi + 1, yi, zi));
  uint16_t c010 = static_cast<uint16_t>(noise_lattice_hash(xi, yi + 1, zi));
  uint16_t c110 = static_cast<uint16_t>(noise_lattice_hash(xi + 1, yi + 1, zi));
  uint16_t c001 = static_cast<uint16_t>(noise_lattice_hash(xi, yi, zi + 1));
  uint16_t c101 = static_cast<uint16_t>(noise_lattice_hash(xi + 1, yi, zi + 1));
  uint16_t c011 = static_cast<uint16_t>(noise_lattice_hash(xi, yi + 1, zi + 1));
  uint16_t c111 = static_cast<uint16_t>(noise_lattice_hash(xi + 1, yi + 1, zi + 1));
  uint16_t x00 = noise_lerp16(c000, c100, xf), x10 = noise_lerp16(c010, c110, xf);
  uint16_t x01 = noise_lerp16(c001, c101, xf), x11 = noise_lerp16(c011, c111, xf);
  uint16_t y0 = noise_lerp16(x00, x10, yf), y1 = noise_lerp16(x01, x11, yf);
  return static_cast<uint8_t>(noise_lerp16(y0, y1, zf) >> 8);
}
uint8_t noise8(int32_t x, int32_t y) { return noise8(x, y, 0); }
uint8_t noise8(int32_t x) { return noise8(x, 0, 0); }

// ---------------------------------------------------------------------
// Effects
// ---------------------------------------------------------------------

// wled00/FX.cpp:8281 mode_particlevolcano().
void mode_particlevolcano(uint32_t, const Params &p, State &state, Frame frame) {
  constexpr uint32_t kNumSprayRequest = 1;

  if (state.call == 0) {
    ps2d::begin(false, false);
    ps2d::set_bounce_y(true);
    ps2d::set_gravity();
    ps2d::set_kill_out_of_bounds(true);
    ps2d::set_motion_blur(230);
    uint32_t num_sprays = std::min(ps2d::num_sources, kNumSprayRequest);
    for (uint32_t i = 0; i < num_sprays; i++) {
      ps2d::sources[i].source.hue = random16();
      ps2d::sources[i].source.x = static_cast<int16_t>(ps2d::max_x / static_cast<int32_t>(num_sprays + 1) * static_cast<int32_t>(i + 1));
      ps2d::sources[i].max_life = 300;
      ps2d::sources[i].min_life = 250;
      ps2d::sources[i].source_flags.collide = true;
      ps2d::sources[i].source_flags.perpetual = true;
    }
  }

  uint32_t num_sprays = std::min(ps2d::num_sources, kNumSprayRequest);
  if (state.call % (11 - (p.intensity / 25)) == 0) {
    ps2d::Settings2D volcano_settings;
    volcano_settings.bounce_x = true;
    for (uint32_t i = 0; i < num_sprays; i++) {
      ps2d::sources[i].source.y = static_cast<int16_t>(ps2d::kPRadius + 5);
      ps2d::sources[i].source.vy = 0;
      ps2d::sources[i].source.hue++;
      ps2d::sources[i].source.vx =
          ps2d::sources[i].source.vx > 0 ? static_cast<int8_t>(p.custom1 >> 2) : static_cast<int8_t>(-(p.custom1 >> 2));
      ps2d::sources[i].vy = static_cast<int8_t>(p.speed >> 2);
      ps2d::sources[i].vx = 0;
      ps2d::sources[i].var = static_cast<int8_t>(custom3_native(p.custom3) >> 1);
      ps2d::spray_emit(ps2d::sources[i]);
      ps2d::set_wall_hardness(255);
      ps2d::particle_move_update(ps2d::sources[i].source, ps2d::sources[i].source_flags, &volcano_settings);
    }
  }

  ps2d::update_system();
  ps2d::set_color_by_age(p.option1);
  ps2d::set_bounce_x(p.option2);
  ps2d::set_wall_hardness(p.custom2);
  if (p.option3) ps2d::enable_particle_collisions(true, p.custom2);
  else ps2d::enable_particle_collisions(false);

  ps2d::set_colors(p.palette_id, p.primary, p.secondary, p.tertiary);
  ps2d::update();
  ps2d::render(frame);
}
EFFECTS_REGISTER(Id::kParticlevolcano, mode_particlevolcano)

struct FireState {
  uint32_t last_call_ms = 0;
};
static_assert(sizeof(FireState) <= State::kDataSize, "FireState too big");

// wled00/FX.cpp:8352 mode_particlefire(). The 4 extra bytes real WLED's
// initParticleSystem2D() carves out of its own PS memory block (for
// frame-rate throttling below) become FireState in state.data instead -
// the particle pool itself has no equivalent per-effect scratch space on
// this port (see particle_system_2d.h's top comment on fixed-size static
// storage).
void mode_particlefire(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  auto &s = *reinterpret_cast<FireState *>(state.data);

  if (state.call == 0) {
    ps2d::begin(false, false);
    state.aux0 = random16();
  }

  ps2d::update_system();
  ps2d::set_wrap_x(p.option2);
  ps2d::set_motion_blur(static_cast<uint8_t>(p.option1 * 170));
  ps2d::set_smear_blur(static_cast<uint8_t>(!p.option1 * 60));

  uint32_t firespeed = std::max<uint32_t>(100, p.speed);
  if (p.speed < 100) {
    uint32_t period = now_ms - s.last_call_ms;
    if (period < static_cast<uint32_t>(map_range(p.speed, 0, 99, 50, 10))) {
      state.call--;
      return;
    }
    s.last_call_ms = now_ms;
  }

  uint32_t spread = static_cast<uint32_t>(ps2d::max_x >> 5) * (custom3_native(p.custom3) + 1);
  uint32_t num_flames = std::min(ps2d::num_sources, static_cast<uint32_t>(4 + ((spread / static_cast<uint32_t>(ps2d::kPRadius)) << 1)));
  uint32_t per_cycle = (num_flames * 2) / 3;

  for (uint32_t i = 0; i < num_flames; i++) {
    if ((state.call & 1) && ps2d::sources[i].source.ttl > 0) {
      ps2d::sources[i].source.ttl--;
    } else {
      ps2d::sources[i].source.x =
          static_cast<int16_t>((ps2d::max_x >> 1) - static_cast<int32_t>(spread >> 1) + random16(static_cast<uint16_t>(spread)));
      ps2d::sources[i].source.y = static_cast<int16_t>(-(ps2d::kPRadius << 2));
      ps2d::sources[i].source.ttl = static_cast<uint16_t>(
          20 + random16(static_cast<uint16_t>((static_cast<uint32_t>(p.custom1) * p.custom1) >> 8)) / (1 + (firespeed >> 5)));
      ps2d::sources[i].max_life = static_cast<uint16_t>(random16(GuDisplay::HEIGHT >> 1) + 16);
      ps2d::sources[i].min_life = static_cast<uint16_t>(ps2d::sources[i].max_life >> 1);
      ps2d::sources[i].vx = static_cast<int8_t>(random16(5) - 2);
      ps2d::sources[i].vy = static_cast<int8_t>((GuDisplay::HEIGHT >> 1) + (firespeed >> 4) + (p.custom1 >> 4));
      ps2d::sources[i].var = static_cast<int8_t>(2 + random16(static_cast<uint16_t>(2 + (firespeed >> 4))));
    }
  }

  if (state.call % 3 == 0) {
    state.aux0++;
    if (state.call % 10 == 0) state.aux1++;
    int8_t windspeed = static_cast<int8_t>(((static_cast<int16_t>(noise8(state.aux0, state.aux1)) - 127) * p.custom2) >> 7);
    ps2d::apply_force(windspeed, 0);
  }
  state.step++;

  if (p.option3) {
    if (state.call % static_cast<uint32_t>(map_range(firespeed, 0, 255, 4, 15)) == 0) {
      for (uint32_t i = 0; i < ps2d::used_particles; i++) {
        if (ps2d::particles[i].y < ps2d::max_y / 4) {
          int32_t curl = static_cast<int32_t>(
                             noise8(ps2d::particles[i].x, ps2d::particles[i].y, static_cast<int32_t>(state.step << 4))) -
                         127;
          ps2d::particles[i].vx = static_cast<int8_t>(ps2d::particles[i].vx + ((curl * (static_cast<int32_t>(firespeed) + 10)) >> 9));
        }
      }
    }
  }

  if (random8() < 10 + (p.intensity >> 2)) {
    for (uint32_t i = 0; i < ps2d::used_particles; i++) {
      if (ps2d::particles[i].ttl == 0) {
        ps2d::particles[i].ttl = static_cast<uint16_t>(random16(GuDisplay::HEIGHT) + 30);
        ps2d::particles[i].x = ps2d::sources[0].source.x;
        ps2d::particles[i].y = ps2d::sources[0].source.y;
        ps2d::particles[i].vx = ps2d::sources[0].source.vx;
        ps2d::particles[i].vy = static_cast<int8_t>((GuDisplay::HEIGHT >> 1) + (firespeed >> 4) +
                                                      ((30 + (p.intensity >> 1) + p.custom1) >> 4));
        break;
      }
    }
  }

  uint8_t j = static_cast<uint8_t>(random16());
  for (uint32_t i = 0; i < per_cycle; i++) {
    j = static_cast<uint8_t>((j + 1) % num_flames);
    ps2d::flame_emit(ps2d::sources[j]);
  }

  ps2d::set_colors(p.palette_id, p.primary, p.secondary, p.tertiary);
  ps2d::update_fire(p.intensity);
  ps2d::render(frame);
}
EFFECTS_REGISTER(Id::kParticlefire, mode_particlefire)

// wled00/FX.cpp:8138 mode_particlefireworks().
void mode_particlefireworks(uint32_t, const Params &p, State &state, Frame frame) {
  constexpr uint32_t kNumRocketRequest = 8;

  if (state.call == 0) {
    ps2d::begin(false, false);
    ps2d::set_kill_out_of_bounds(true);
    ps2d::set_wall_hardness(120);
    uint32_t num_rockets = std::min(ps2d::num_sources, kNumRocketRequest);
    for (uint32_t j = 0; j < num_rockets; j++) {
      ps2d::sources[j].source.ttl = static_cast<uint16_t>(500 * j);
      ps2d::sources[j].source.vy = -1;
    }
  }

  ps2d::update_system();
  uint32_t num_rockets = static_cast<uint32_t>(map_range(p.speed, 0, 255, 4, std::min(ps2d::num_sources, kNumRocketRequest)));

  ps2d::set_wrap_x(p.option1);
  ps2d::set_bounce_y(p.option2);
  ps2d::set_gravity(static_cast<int8_t>(map_range(p.custom3, 0, 255, p.option2 ? 1 : 0, 10)));
  ps2d::set_motion_blur(static_cast<uint8_t>(map_range(p.custom2, 0, 255, 0, 245)));

  for (uint32_t j = 0; j < num_rockets; j++) {
    ps2d::apply_gravity(ps2d::sources[j].source);
    ps2d::particle_move_update(ps2d::sources[j].source, ps2d::sources[j].source_flags);
    if (ps2d::sources[j].source.ttl == 0) {
      if (ps2d::sources[j].source.vy > 0) {
        ps2d::sources[j].source.vy = 0;
      } else if (ps2d::sources[j].source.vy < 0) {
        ps2d::sources[j].source.y = ps2d::kPRadius;
        ps2d::sources[j].source.x =
            static_cast<int16_t>((ps2d::max_x >> 2) + random16(static_cast<uint16_t>(ps2d::max_x >> 1)));
        ps2d::sources[j].source.vy =
            static_cast<int8_t>(custom3_native(p.custom3) + random16(static_cast<uint16_t>(p.custom1 >> 3)) + 5);
        ps2d::sources[j].source.vx = static_cast<int8_t>(random16(7) - 3);
        ps2d::sources[j].source.sat = 30;
        ps2d::sources[j].source.ttl = static_cast<uint16_t>(random16(p.custom1) + (p.custom1 >> 1));
        ps2d::sources[j].max_life = 40;
        ps2d::sources[j].min_life = 10;
        ps2d::sources[j].vx = 0;
        ps2d::sources[j].vy = -5;
        ps2d::sources[j].var = 4;
      }
    }
  }

  bool circular_explosion = false;
  int32_t counter = 0;
  int32_t speed = 0, currentspeed = 0, percircle = 0;
  uint32_t frequency = 0, baseangle = 0, hueincrement = 0;
  uint16_t angle = 0;
  uint32_t angleincrement = 0;

  for (uint32_t j = 0; j < num_rockets; j++) {
    uint32_t emit_particles;
    if (ps2d::sources[j].source.vy > 0) {
      emit_particles = 1;
    } else if (ps2d::sources[j].source.vy < 0) {
      emit_particles = 0;
    } else {
      ps2d::sources[j].source.hue = random16();
      ps2d::sources[j].source.sat = static_cast<uint8_t>(random16(55) + 200);
      ps2d::sources[j].max_life = 200;
      ps2d::sources[j].min_life = 100;
      uint32_t upper = 2000u - (static_cast<uint32_t>(p.speed) << 2);
      ps2d::sources[j].source.ttl = static_cast<uint16_t>(random16(static_cast<uint16_t>(upper)) + 550 - (p.speed << 1));
      ps2d::sources[j].var = static_cast<int8_t>((p.intensity >> 4) + 5);
      ps2d::sources[j].source.vy = -1;
      emit_particles = static_cast<uint32_t>(random16(static_cast<uint16_t>(p.intensity >> 2)) + (p.intensity >> 2) + 5);

      if (random8() & 1) {
        circular_explosion = true;
        speed = 2 + random16(3) + (p.intensity >> 6);
        currentspeed = speed;
        angleincrement = 2730 + random16(5461);
        angle = random16();
        baseangle = angle;
        percircle = 0xFFFF / static_cast<int32_t>(angleincrement) + 1;
        hueincrement = random16() & 127;
        int circles = 1 + random16(3) + (p.intensity >> 6);
        frequency = random16() & 127;
        emit_particles = static_cast<uint32_t>(percircle * circles);
        ps2d::sources[j].var = static_cast<int8_t>(angle & 1);
      }
    }

    uint32_t i;
    for (i = 0; i < emit_particles; i++) {
      if (circular_explosion) {
        int32_t sine_mod = 0xEFFF + sin16(static_cast<uint16_t>(((angle * frequency) >> 4) + baseangle));
        currentspeed = (speed / 2 + ((sine_mod * speed) >> 16)) >> 1;
        ps2d::angle_emit(ps2d::sources[j], angle, currentspeed);
        counter++;
        if (counter > percircle) {
          counter = 0;
          speed += 3 + (p.intensity >> 6);
          ps2d::sources[j].source.hue = static_cast<uint8_t>(ps2d::sources[j].source.hue + hueincrement);
          ps2d::sources[j].source.sat = static_cast<uint8_t>(100 + random16(156));
        }
        angle = static_cast<uint16_t>(angle + angleincrement);
      } else {
        ps2d::spray_emit(ps2d::sources[j]);
        if ((j % 3) == 0) ps2d::sources[j].source.hue = random16();
      }
    }
    if (i == 0) ps2d::sources[j].source.y = 1000;
    circular_explosion = false;
  }

  if (p.option3) {
    for (uint32_t i = 0; i < ps2d::used_particles; i++) {
      ps2d::particle_move_update(ps2d::particles[i], ps2d::particle_flags[i], nullptr, nullptr);
    }
  }

  ps2d::set_colors(p.palette_id, p.primary, p.secondary, p.tertiary);
  ps2d::update();
  ps2d::render(frame);
}
EFFECTS_REGISTER(Id::kParticlefireworks, mode_particlefireworks)

// wled00/FX.cpp:8024 mode_particlevortex().
void mode_particlevortex(uint32_t, const Params &p, State &state, Frame frame) {
  constexpr uint32_t kNumSourceRequest = 8;

  if (state.call == 0) {
    ps2d::begin(false, false);
    ps2d::set_motion_blur(130);
    uint32_t n = std::min(ps2d::num_sources, kNumSourceRequest);
    for (uint32_t i = 0; i < n; i++) {
      ps2d::sources[i].source.x = static_cast<int16_t>((ps2d::max_x + 1) >> 1);
      ps2d::sources[i].source.y = static_cast<int16_t>((ps2d::max_y + 1) >> 1);
      ps2d::sources[i].max_life = 900;
      ps2d::sources[i].min_life = 800;
    }
    ps2d::set_kill_out_of_bounds(true);
  }

  ps2d::update_system();
  uint32_t spraycount = std::min(ps2d::num_sources, static_cast<uint32_t>(1 + (p.custom1 >> 5)));

  ps2d::set_smear_blur(static_cast<uint8_t>(p.option1 ? 90 : 0));

  for (uint32_t i = 0; i < spraycount; i++) {
    uint32_t coloroffset = 0xFFu / spraycount;
    ps2d::sources[i].source.hue = static_cast<uint8_t>(coloroffset * i);
  }

  bool direction = p.option2;
  int32_t currentspeed = static_cast<int32_t>(state.step);

  if (p.custom2 > 0) {
    uint32_t changeinterval = 1040 - (static_cast<uint32_t>(p.custom2) << 2);
    direction = (state.aux1 & 0x01) != 0;
    if (p.option3) changeinterval = 20 + changeinterval + random16(static_cast<uint16_t>(changeinterval));
    if (state.call % changeinterval == 0) {
      state.aux1 = static_cast<uint16_t>(state.aux1 | 0x02);
      if (direction) state.aux1 = static_cast<uint16_t>(state.aux1 & ~0x01);
      else state.aux1 = static_cast<uint16_t>(state.aux1 | 0x01);
    }
  }

  int32_t targetspeed = (direction ? 1 : -1) * (static_cast<int32_t>(p.speed) << 3);
  int32_t speeddiff = targetspeed - currentspeed;
  int32_t speedincrement = speeddiff / 50;
  if (speedincrement == 0) {
    if (speeddiff < 0) speedincrement = -1;
    else if (speeddiff > 0) speedincrement = 1;
  }

  currentspeed += speedincrement;
  state.aux0 = static_cast<uint16_t>(state.aux0 + currentspeed);
  state.step = static_cast<uint32_t>(currentspeed);

  uint16_t angleoffset = static_cast<uint16_t>(0xFFFFu / spraycount);
  uint32_t skip = static_cast<uint32_t>(ps2d::kPHalfRadius) / (p.intensity + 1) + 1;
  if (state.call % skip == 0) {
    uint32_t j = random16(static_cast<uint16_t>(spraycount));
    for (uint32_t i = 0; i < spraycount; i++) {
      ps2d::sources[j].var = static_cast<int8_t>(custom3_native(p.custom3) >> 1);
      ps2d::angle_emit(ps2d::sources[j], static_cast<uint16_t>(state.aux0 + angleoffset * j), (p.intensity >> 2) + 1);
      j = (j + 1) % spraycount;
    }
  }

  ps2d::set_colors(p.palette_id, p.primary, p.secondary, p.tertiary);
  ps2d::update();
  ps2d::render(frame);
}
EFFECTS_REGISTER(Id::kParticlevortex, mode_particlevortex)

// wled00/FX.cpp:8681 mode_particleperlin().
void mode_particleperlin(uint32_t, const Params &p, State &state, Frame frame) {
  if (state.call == 0) {
    ps2d::begin(true, false);
    ps2d::set_kill_out_of_bounds(true);
    ps2d::set_motion_blur(230);
    ps2d::set_bounce_y(true);
    state.aux0 = random16();
  }

  ps2d::update_system();
  ps2d::set_wrap_x(p.option1);
  ps2d::set_bounce_x(!p.option1);
  ps2d::set_wall_hardness(p.custom1);
  ps2d::enable_particle_collisions(p.option3, p.custom1);
  ps2d::set_used_particles(static_cast<uint8_t>(map_range(p.intensity, 0, 255, 25, 128)));
  ps2d::set_smear_blur(static_cast<uint8_t>(p.option2 * 15));

  state.aux0 = static_cast<uint16_t>(state.aux0 + 1 + (p.speed >> 5));
  for (uint32_t i = 0; i < ps2d::used_particles; i++) {
    if (ps2d::particles[i].ttl == 0) {
      ps2d::particles[i].ttl = static_cast<uint16_t>(random16(500) + 200);
      ps2d::particles[i].x = static_cast<int16_t>(random16(static_cast<uint16_t>(ps2d::max_x)));
      ps2d::particles[i].y = static_cast<int16_t>(random16(static_cast<uint16_t>(ps2d::max_y)));
      ps2d::particle_flags[i].collide = true;
    }
    uint32_t scale = 16 - ((31 - custom3_native(p.custom3)) >> 1);
    uint16_t xnoise = static_cast<uint16_t>(ps2d::particles[i].x / static_cast<int32_t>(scale));
    uint16_t ynoise = static_cast<uint16_t>(ps2d::particles[i].y / static_cast<int32_t>(scale));
    int16_t baseheight = noise8(xnoise, ynoise, state.aux0);
    ps2d::particles[i].hue = static_cast<uint8_t>(baseheight);
    if (state.call % 8 == 0) {
      int8_t xslope = static_cast<int8_t>(baseheight + static_cast<int16_t>(noise8(xnoise - 10, ynoise, state.aux0)));
      int8_t yslope = static_cast<int8_t>(baseheight + static_cast<int16_t>(noise8(xnoise, ynoise - 10, state.aux0)));
      ps2d::apply_force(i, xslope, yslope);
    }
  }

  if (state.call % (16 - (p.custom2 >> 4)) == 0) ps2d::apply_friction(2);

  ps2d::set_colors(p.palette_id, p.primary, p.secondary, p.tertiary);
  ps2d::update();
  ps2d::render(frame);
}
EFFECTS_REGISTER(Id::kParticleperlin, mode_particleperlin)

// wled00/FX.cpp:8456 mode_particlepit().
void mode_particlepit(uint32_t, const Params &p, State &state, Frame frame) {
  if (state.call == 0) {
    ps2d::begin(true, false);
    ps2d::set_kill_out_of_bounds(true);
    ps2d::set_gravity();
    ps2d::set_used_particles(170);
  }

  ps2d::update_system();
  ps2d::set_wrap_x(p.option1);
  ps2d::set_bounce_x(p.option2);
  ps2d::set_bounce_y(p.option3);
  ps2d::set_wall_hardness(std::min(p.custom2, static_cast<uint8_t>(150)));
  if (p.custom2 > 0) ps2d::enable_particle_collisions(true, p.custom2);
  else ps2d::enable_particle_collisions(false);

  if (state.call % (128 - (p.intensity >> 1)) == 0 && p.intensity > 0) {
    for (uint32_t i = 0; i < ps2d::used_particles; i++) {
      if (ps2d::particles[i].ttl == 0) {
        ps2d::particles[i].ttl = static_cast<uint16_t>(1500 - (p.speed << 2) + random16(500));
        ps2d::particles[i].x = static_cast<int16_t>(random16(static_cast<uint16_t>(ps2d::max_x)));
        ps2d::particles[i].y = static_cast<int16_t>(ps2d::max_y << 1);
        ps2d::particles[i].vx = static_cast<int8_t>(random16(static_cast<uint16_t>(p.speed >> 1)) - (p.speed >> 2));
        ps2d::particles[i].vy = static_cast<int8_t>(map_range(p.speed, 0, 255, -5, -100));
        ps2d::particles[i].hue = static_cast<uint8_t>(random16());
        ps2d::particle_flags[i].collide = true;
        ps2d::particles[i].sat = static_cast<uint8_t>((custom3_native(p.custom3) << 3) + 7);
        if (p.custom1 == 255) {
          ps2d::per_particle_size = true;
          ps2d::adv_particles[i].size = static_cast<uint8_t>(random16(p.custom1));
        } else {
          ps2d::set_particle_size(p.custom1);
          ps2d::adv_particles[i].size = p.custom1;
        }
        break;
      }
    }
  }

  uint32_t friction_coeff = 1 + p.option1;
  if (p.speed < 50) friction_coeff = 50 - p.speed;
  if (state.call % 6 == 0) ps2d::apply_friction(static_cast<int32_t>(friction_coeff));

  ps2d::set_colors(p.palette_id, p.primary, p.secondary, p.tertiary);
  ps2d::update();
  ps2d::render(frame);
}
EFFECTS_REGISTER(Id::kParticlepit, mode_particlepit)

// wled00/FX.cpp:8595 mode_particlebox().
void mode_particlebox(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  constexpr int kMaxParticleSize = std::min((GuDisplay::WIDTH * GuDisplay::HEIGHT) >> 2, 255);

  if (state.call == 0) {
    ps2d::begin(true, false);
    ps2d::set_bounce_x(true);
    ps2d::set_bounce_y(true);
    state.aux0 = random16();
  }

  ps2d::update_system();
  ps2d::set_wall_hardness(std::min(p.custom2, static_cast<uint8_t>(200)));
  ps2d::enable_particle_collisions(true, static_cast<uint8_t>(std::max(2, static_cast<int>(p.custom2))));
  unsigned current_size = static_cast<unsigned>(map_range(p.custom3, 0, 255, 0, kMaxParticleSize));
  ps2d::set_used_particles(static_cast<uint8_t>(map_range(p.intensity, 0, 255, 2, 153) / (1 + (current_size >> 4))));
  if (p.custom3 < 255) ps2d::set_particle_size(static_cast<uint8_t>(current_size));
  else ps2d::per_particle_size = true;

  for (uint32_t i = 0; i < ps2d::used_particles; i++) {
    if (ps2d::particles[i].ttl < 260) {
      ps2d::particles[i].ttl = 260;
      ps2d::particles[i].x = static_cast<int16_t>(random16(static_cast<uint16_t>(ps2d::max_x)));
      ps2d::particles[i].y = static_cast<int16_t>(random16(static_cast<uint16_t>(ps2d::max_y)));
      ps2d::particles[i].hue = random8();
      ps2d::particle_flags[i].perpetual = true;
      ps2d::particle_flags[i].collide = true;
      ps2d::adv_particles[i].size = random8(static_cast<uint8_t>(kMaxParticleSize));
      break;
    }
  }

  if (state.call % (((255 - p.speed) >> 6) + 1) == 0 && p.speed > 0) {
    int32_t xgravity, ygravity;
    int32_t increment = (p.speed >> 6) + 1;

    if (p.option2) {
      int speed = tristate_square8(static_cast<uint8_t>(now_ms >> 7), 90, 15) / ((400 - p.speed) >> 3);
      state.aux0 = static_cast<uint16_t>(state.aux0 + speed);
      if (speed == 0) state.aux0 = 190;
    } else {
      state.aux0 = static_cast<uint16_t>(state.aux0 - increment);
    }

    if (p.option1) {
      xgravity = static_cast<int16_t>(noise8(state.aux0)) - 127;
      ygravity = static_cast<int16_t>(noise8(state.aux0 + 10000)) - 127;
      xgravity = (xgravity * p.custom1) / 128;
      ygravity = (ygravity * p.custom1) / 128;
    } else {
      xgravity = (static_cast<int32_t>(p.custom1) * cos16(static_cast<uint16_t>(state.aux0 << 8))) / 0xFFFF;
      ygravity = (static_cast<int32_t>(p.custom1) * sin16(static_cast<uint16_t>(state.aux0 << 8))) / 0xFFFF;
    }
    if (p.option3 && ygravity > 0) ygravity = -ygravity;

    ps2d::apply_force(static_cast<int8_t>(xgravity), static_cast<int8_t>(ygravity));
  }

  if ((state.call & 0x0F) == 0) ps2d::apply_friction(1);

  ps2d::set_colors(p.palette_id, p.primary, p.secondary, p.tertiary);
  ps2d::update();
  ps2d::render(frame);
}
EFFECTS_REGISTER(Id::kParticlebox, mode_particlebox)

// wled00/FX.cpp:8742 mode_particleimpact().
void mode_particleimpact(uint32_t, const Params &p, State &state, Frame frame) {
  constexpr uint32_t kNumMeteorRequest = 8;
  ps2d::Settings2D meteor_settings;
  meteor_settings.bounce_y = true;
  meteor_settings.use_gravity = true;

  if (state.call == 0) {
    ps2d::begin(false, false);
    ps2d::set_kill_out_of_bounds(true);
    ps2d::set_gravity();
    ps2d::set_bounce_y(true);
    ps2d::set_wall_roughness(220);
    uint32_t num_meteors = std::min(ps2d::num_sources, kNumMeteorRequest);
    for (uint32_t i = 0; i < num_meteors; i++) {
      ps2d::sources[i].source.ttl = random16(static_cast<uint16_t>(10 * i));
      ps2d::sources[i].source.vy = 10;
    }
  }

  ps2d::update_system();
  ps2d::set_wrap_x(p.option1);
  ps2d::set_bounce_x(p.option2);
  ps2d::set_motion_blur(static_cast<uint8_t>(p.custom3 << 3));
  uint8_t hardness = static_cast<uint8_t>(map_range(p.custom2, 0, 255, ps2d::kPMinSurfaceHardness - 2, 255));
  ps2d::set_wall_hardness(hardness);
  ps2d::enable_particle_collisions(p.option3, hardness);
  uint32_t num_meteors = std::min(ps2d::num_sources, kNumMeteorRequest);

  for (uint32_t i = 0; i < num_meteors; i++) {
    uint32_t emit_particles;
    if (ps2d::sources[i].source.vy < 0) emit_particles = 1;
    else if (ps2d::sources[i].source.vy > 0) emit_particles = 0;
    else {
      ps2d::sources[i].source.vy = 10;
      emit_particles =
          static_cast<uint32_t>(map_range(p.intensity, 0, 255, 10, random16(static_cast<uint16_t>(ps2d::used_particles >> 2))));
    }
    for (uint32_t e = 0; e < emit_particles; e++) ps2d::spray_emit(ps2d::sources[i]);
  }

  for (uint32_t i = 0; i < num_meteors; i++) {
    if (ps2d::sources[i].source.ttl) {
      ps2d::sources[i].source.ttl--;
      if (ps2d::sources[i].source.vy < 0) {
        ps2d::apply_gravity(ps2d::sources[i].source);
        ps2d::particle_move_update(ps2d::sources[i].source, ps2d::sources[i].source_flags, &meteor_settings);
        if (ps2d::sources[i].source.y < (ps2d::kPRadius << 1)) {
          ps2d::sources[i].source.vy = 0;
          ps2d::sources[i].source.vx = 0;
          ps2d::sources[i].source_flags.collide = true;
          ps2d::sources[i].max_life = 1250;
          ps2d::sources[i].min_life = 250;
          ps2d::sources[i].source.ttl =
              static_cast<uint16_t>(random16(static_cast<uint16_t>(768 - (p.speed << 1))) + 40);
          ps2d::sources[i].vy = static_cast<int8_t>(p.custom1 >> 2);
          ps2d::sources[i].var = static_cast<int8_t>(p.custom1 >> 2);
        }
      }
    } else if (ps2d::sources[i].source.vy > 0) {
      ps2d::sources[i].source.y = static_cast<int16_t>(ps2d::max_y + (ps2d::kPRadius << 2));
      ps2d::sources[i].source.x = static_cast<int16_t>(random16(static_cast<uint16_t>(ps2d::max_x)));
      ps2d::sources[i].source.vy = static_cast<int8_t>(-random16(30) - 30);
      ps2d::sources[i].source.vx = static_cast<int8_t>(random16(50) - 25);
      ps2d::sources[i].source.hue = random16();
      ps2d::sources[i].source.ttl = 500;
      ps2d::sources[i].source_flags.collide = false;
      ps2d::sources[i].max_life = 300;
      ps2d::sources[i].min_life = 100;
      ps2d::sources[i].vy = -9;
      ps2d::sources[i].var = 3;
    }
  }

  for (uint32_t i = 0; i < ps2d::used_particles; i++) {
    if (ps2d::particles[i].ttl > 5) ps2d::particles[i].ttl = static_cast<uint16_t>(ps2d::particles[i].ttl - 5);
  }

  ps2d::set_colors(p.palette_id, p.primary, p.secondary, p.tertiary);
  ps2d::update();
  ps2d::render(frame);
}
EFFECTS_REGISTER(Id::kParticleimpact, mode_particleimpact)

// wled00/FX.cpp:8524 mode_particlewaterfall().
void mode_particlewaterfall(uint32_t, const Params &p, State &state, Frame frame) {
  if (state.call == 0) {
    ps2d::begin(false, false);
    ps2d::set_gravity();
    ps2d::set_kill_out_of_bounds(true);
    ps2d::set_motion_blur(190);
    ps2d::set_smear_blur(30);
    for (uint32_t i = 0; i < ps2d::num_sources; i++) {
      ps2d::sources[i].source.hue = static_cast<uint8_t>(i * 90);
      ps2d::sources[i].source_flags.collide = true;
      ps2d::sources[i].max_life = 400;
      ps2d::sources[i].min_life = 150;
    }
  }

  ps2d::update_system();
  ps2d::set_wrap_x(p.option1);
  ps2d::set_bounce_x(p.option2);
  ps2d::set_bounce_y(p.option3);
  ps2d::set_wall_hardness(p.custom2);
  int32_t max_x_pixel = ps2d::max_x >> ps2d::kPRadiusShift;
  uint32_t num_sprays = std::min<uint32_t>(ps2d::num_sources, static_cast<uint32_t>(std::max<int32_t>(max_x_pixel / 6, 2)));
  if (p.custom2 > 0) {
    ps2d::enable_particle_collisions(true, p.custom2);
  } else {
    ps2d::enable_particle_collisions(false);
    ps2d::set_wall_hardness(120);
  }

  for (uint32_t i = 0; i < num_sprays; i++) {
    ps2d::sources[i].source.hue =
        static_cast<uint8_t>(ps2d::sources[i].source.hue + 1 + random16(static_cast<uint16_t>(p.custom1 >> 1)));
  }

  if (state.call % (12 - (p.intensity >> 5)) == 0 && p.intensity > 0) {
    for (uint32_t i = 0; i < num_sprays; i++) {
      ps2d::sources[i].vy = static_cast<int8_t>(-static_cast<int32_t>(p.speed) >> 3);
      ps2d::sources[i].source.x = static_cast<int16_t>(
          map_range(p.custom3, 0, 255, 0, (max_x_pixel - static_cast<int32_t>(num_sprays)) * ps2d::kPRadius) +
          static_cast<int32_t>(i) * ps2d::kPRadius * 2);
      ps2d::sources[i].source.y =
          static_cast<int16_t>(ps2d::max_y + (ps2d::kPRadius * ((static_cast<int32_t>(i) << 2) + 4)));
      ps2d::sources[i].var = static_cast<int8_t>(p.custom1 >> 3);
      ps2d::spray_emit(ps2d::sources[i]);
    }
  }

  if (state.call % 20 == 0) ps2d::apply_friction(1);

  ps2d::set_colors(p.palette_id, p.primary, p.secondary, p.tertiary);
  ps2d::update();
  ps2d::render(frame);
}
EFFECTS_REGISTER(Id::kParticlewaterfall, mode_particlewaterfall)

constexpr int32_t kMaxAngleStep = 2200;  // wled00/FX.cpp's MAXANGLESTEP (32767 == 180 deg)

// wled00/FX.cpp:9156 mode_particleghostrider().
void mode_particleghostrider(uint32_t, const Params &p, State &state, Frame frame) {
  ps2d::Settings2D ghost_settings;
  ghost_settings.wrap_x = true;
  ghost_settings.wrap_y = true;

  if (state.call == 0) {
    ps2d::begin(false, false);
    ps2d::set_kill_out_of_bounds(true);
    ps2d::sources[0].max_life = 260;
    ps2d::sources[0].min_life = 250;
    ps2d::sources[0].source.x = static_cast<int16_t>(random16(static_cast<uint16_t>(ps2d::max_x)));
    ps2d::sources[0].source.y = static_cast<int16_t>(random16(static_cast<uint16_t>(ps2d::max_y)));
    state.step = static_cast<uint32_t>(static_cast<int32_t>(random16(kMaxAngleStep)) - (kMaxAngleStep >> 1));
  }

  if (p.intensity > 0) {
    if (state.aux1) {
      state.step += p.intensity >> 3;
      if (static_cast<int32_t>(state.step) > kMaxAngleStep) state.aux1 = 0;
    } else {
      state.step -= p.intensity >> 3;
      if (static_cast<int32_t>(state.step) < -kMaxAngleStep) state.aux1 = 1;
    }
  }

  ps2d::update_system();
  ps2d::set_motion_blur(p.custom1);
  ps2d::sources[0].var = static_cast<int8_t>(custom3_native(p.custom3) >> 1);

  if (p.option1) {
    for (uint32_t i = 0; i < ps2d::used_particles; i++) {
      ps2d::particles[i].hue = static_cast<uint8_t>(ps2d::sources[0].source.hue + (ps2d::particles[i].ttl << 2));
    }
  }

  ghost_settings.bounce_x = p.option2;
  ghost_settings.bounce_y = p.option2;

  state.aux0 = static_cast<uint16_t>(state.aux0 + static_cast<int32_t>(state.step));
  uint16_t emit_angle = static_cast<uint16_t>(state.aux0 + 32767);
  int32_t speed = map_range(p.speed, 0, 255, 12, 64);
  ps2d::sources[0].source.vx = static_cast<int8_t>((static_cast<int32_t>(cos16(state.aux0)) * speed) / 32767);
  ps2d::sources[0].source.vy = static_cast<int8_t>((static_cast<int32_t>(sin16(state.aux0)) * speed) / 32767);
  ps2d::sources[0].source.ttl = 500;
  ps2d::particle_move_update(ps2d::sources[0].source, ps2d::sources[0].source_flags, &ghost_settings);

  ps2d::particles[ps2d::used_particles - 1].x = ps2d::sources[0].source.x;
  ps2d::particles[ps2d::used_particles - 1].y = ps2d::sources[0].source.y;
  ps2d::particles[ps2d::used_particles - 1].ttl = 255;
  ps2d::particles[ps2d::used_particles - 1].sat = 0;

  ps2d::angle_emit(ps2d::sources[0], emit_angle, speed);
  ps2d::angle_emit(ps2d::sources[0], emit_angle, speed);

  if (state.call % (11 - (p.custom2 / 25)) == 0) ps2d::sources[0].source.hue++;
  if (p.custom2 > 190) ps2d::sources[0].source.hue = static_cast<uint8_t>(ps2d::sources[0].source.hue + ((p.custom2 - 190) >> 2));

  ps2d::set_colors(p.palette_id, p.primary, p.secondary, p.tertiary);
  ps2d::update();
  ps2d::render(frame);
}
EFFECTS_REGISTER(Id::kParticleghostrider, mode_particleghostrider)

// wled00/FX.cpp:9309 mode_particlegalaxy().
void mode_particlegalaxy(uint32_t, const Params &p, State &state, Frame frame) {
  ps2d::Settings2D source_settings;
  source_settings.bounce_x = true;
  source_settings.bounce_y = true;

  if (state.call == 0) {
    ps2d::begin(true, false);
    ps2d::sources[0].source.vx = -4;
    ps2d::sources[0].source.x = static_cast<int16_t>(ps2d::max_x >> 1);
    ps2d::sources[0].source.y = static_cast<int16_t>(ps2d::max_y >> 1);
    ps2d::sources[0].source_flags.perpetual = true;
    ps2d::sources[0].max_life = 4000;
    ps2d::sources[0].min_life = 800;
    ps2d::sources[0].source.hue = random16();
    ps2d::set_wall_hardness(255);
    ps2d::set_wall_roughness(200);
  }

  ps2d::update_system();
  ps2d::set_particle_size(p.custom1);
  ps2d::set_motion_blur(static_cast<uint8_t>(250 * p.option3));

  uint8_t custom3 = custom3_native(p.custom3);
  if ((state.call % static_cast<uint32_t>((33 - custom3) >> 1)) == 0)
    ps2d::sources[0].source.hue = static_cast<uint8_t>(ps2d::sources[0].source.hue + 2);

  if (random8() < (10 + (p.intensity >> 1))) ps2d::spray_emit(ps2d::sources[0]);

  if ((state.call & 0x3) == 0) ps2d::particle_move_update(ps2d::sources[0].source, ps2d::sources[0].source_flags, &source_settings);

  int32_t centerx = ps2d::max_x >> 1;
  int32_t centery = ps2d::max_y >> 1;
  if (p.option2) {
    ps2d::set_kill_out_of_bounds(true);
    ps2d::sources[0].var = 7;
    ps2d::sources[0].source.x = static_cast<int16_t>(centerx);
    ps2d::sources[0].source.y = static_cast<int16_t>(centery);
  } else {
    ps2d::set_kill_out_of_bounds(false);
    ps2d::sources[0].var = 1;
  }

  for (uint32_t i = 0; i < ps2d::used_particles; i++) {
    if (ps2d::particles[i].ttl == 0) continue;
    int32_t dx = centerx - ps2d::particles[i].x;
    int32_t dy = centery - ps2d::particles[i].y;
    int32_t distance = isqrt(dx * dx + dy * dy);
    if (distance < 20) distance = 20;
    int32_t speedfactor;
    if (p.option2) {
      speedfactor = 1 + (1 + (p.speed >> 1)) * distance;
      ps2d::particles[i].x = static_cast<int16_t>(ps2d::particles[i].x + (-speedfactor * dx) / 400000 - (dy >> 6));
      ps2d::particles[i].y = static_cast<int16_t>(ps2d::particles[i].y + (-speedfactor * dy) / 400000 + (dx >> 6));
    } else {
      speedfactor = 2 + (((50 + p.speed) << 6) / distance);
      int32_t temp_vx = -speedfactor * dy;
      int32_t temp_vy = speedfactor * dx;
      int32_t vxc = (dx << 9) / (distance - 19);
      int32_t vyc = (dy << 9) / (distance - 19);
      ps2d::particles[i].x = static_cast<int16_t>(ps2d::particles[i].x + (temp_vx + vxc) / 1024);
      ps2d::particles[i].y = static_cast<int16_t>(ps2d::particles[i].y + (temp_vy + vyc) / 1024);

      if (distance < 128) {
        if (ps2d::particles[i].ttl > 3) ps2d::particles[i].ttl = static_cast<uint16_t>(ps2d::particles[i].ttl - 4);
        ps2d::particles[i].sat = static_cast<uint8_t>(distance << 1);
      }
    }
    if (custom3 == 31) ps2d::particles[i].hue = static_cast<uint8_t>(ps2d::particles[i].ttl >> 2);
    else if (custom3 == 0)
      ps2d::particles[i].hue = static_cast<uint8_t>(map_range(distance, 20, (ps2d::max_x + ps2d::max_y) >> 2, 0, 180));
  }

  ps2d::set_colors(p.palette_id, p.primary, p.secondary, p.tertiary);
  ps2d::update();
  ps2d::render(frame);
}
EFFECTS_REGISTER(Id::kParticlegalaxy, mode_particlegalaxy)

}  // namespace
}  // namespace effects
