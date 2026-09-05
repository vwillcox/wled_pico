#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "effects.h"
#include "wled_compat.h"
#include "palettes.h"
#include "particle_system_1d.h"
#include "display/gu_display.h"

// Real WLED's 11 non-audio-reactive mode_particle*() effects
// (wled00/FX.cpp) built on ParticleSystem1D, ported onto particle_system_1d.h's
// engine API (effects::ps1d::*). Each function below cites its exact WLED
// source function/line.
//
// Porting notes that apply across this whole file (stated once here rather
// than re-derived at every call site):
//
//  - No init-failure fallback. Real WLED's initParticleSystem1D() can fail
//    (segment too short, allocation failure) and every mode_ function
//    bails to mode_static() if so; this port's particle system is a fixed-
//    size static singleton (see particle_system_1d.h's top comment) that
//    can't fail to "allocate", so that check has nothing to port.
//  - WLED's Segment::custom3 is a 5-bit field (0-31); this port's
//    Params::custom3 is 0-255 (see effects.h). Every formula below that
//    assumes a 0-31 custom3 first rescales it via `p.custom3 >> 3` (exact:
//    255>>3==31) into a local `c3`, then uses `c3` exactly as WLED's source
//    uses SEGMENT.custom3 - this keeps every downstream formula an
//    unmodified transliteration instead of re-deriving each threshold's
//    equivalent under a wider input range.
//  - hw_random()/hw_random(x) (WLED's 32-bit hardware-RNG helper, wled00/
//    fcn_declare.h) has no 32-bit-range equivalent in wled_compat.h; every
//    value range actually used below (particle positions/speeds/counters,
//    all far under 65536) fits random16() cleanly, so hw_random()/
//    hw_random(x) calls map to random16()/random16(x) - matching
//    wled_compat.h's own documented "shape not bit-exact" tolerance.
//    hw_random8()/hw_random16() map to random8()/random16() directly.
//  - Arduino's map(), FastLED's cubicwave8()/triwave8()/ease8InOutCubic(),
//    and WLED's perlin8() aren't in wled_compat.h (only helpers shared by
//    every landed batch live there) - local map_range()/cubicwave8()/
//    perlin8() below are the same small helpers gen_batch3.cpp/
//    gen_batch5.cpp/gen_batch7.cpp already duplicate per-TU for the same
//    reason (see gen_batch7.cpp's header comment).
namespace effects {
namespace {

// -- small local helpers (see this file's top comment) ---------------------

// Arduino's map(), wled00 uses this constantly. Same helper gen_batch7.cpp
// already carries under this name.
long map_range(long x, long in_min, long in_max, long out_min, long out_max) {
  if (in_max == in_min) return out_min;
  return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

// FastLED's triwave8()/ease8InOutCubic()/cubicwave8() (lib8tion.h) - used
// only by mode_particleDancingShadows()'s gradient spotlight types below.
// Identical copy of gen_batch3.cpp/gen_batch5.cpp's own.
uint8_t triwave8(uint8_t in) {
  if (in & 0x80) in = static_cast<uint8_t>(255 - in);
  return static_cast<uint8_t>(in << 1);
}
uint8_t ease8InOutCubic(uint8_t i) {
  uint16_t ii = scale8(i, i);
  uint16_t iii = scale8(static_cast<uint8_t>(ii), i);
  uint16_t r1 = static_cast<uint16_t>((3 * ii) - (2 * iii));
  return (r1 & 0x100) ? 255 : static_cast<uint8_t>(r1);
}
uint8_t cubicwave8(uint8_t in) { return ease8InOutCubic(triwave8(in)); }

// WLED's perlin8() (FastLED's inoise8()) - needed only for
// mode_particleBalance()'s "random tilt" mode, which calls it with a single
// (x-only) coordinate. A plain linear interpolation between two hashed
// lattice points stands in for real Perlin noise's gradient/smoothstep
// machinery - continuous and smooth enough for a decorative tilt wobble,
// same "shape not bit-exact" tolerance as gen_batch3.cpp's fuller (3D,
// smoothstepped) version of this same idea.
uint8_t perlin8(uint32_t x) {
  int32_t xi = static_cast<int32_t>(x >> 8);
  int32_t xf = static_cast<int32_t>(x & 0xFF);
  auto hash8 = [](int32_t v) -> int32_t {
    uint32_t h = static_cast<uint32_t>(v) * 374761393u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return static_cast<int32_t>((h ^ (h >> 16)) & 0xFFu);
  };
  int32_t a = hash8(xi);
  int32_t b = hash8(xi + 1);
  return static_cast<uint8_t>(a + (((b - a) * xf) >> 8));
}

// wled00/FX.cpp:4474 SPOT_TYPE_* #defines - local to mode_particleDancingShadows()'s spotlight shapes below (not part of the particle engine itself).
constexpr uint32_t kSpotTypeSolid = 0;
constexpr uint32_t kSpotTypeGradient = 1;
constexpr uint32_t kSpotType2xGradient = 2;
constexpr uint32_t kSpotType2xDot = 3;
constexpr uint32_t kSpotType3xDot = 4;
constexpr uint32_t kSpotType4xDot = 5;
constexpr uint32_t kSpotTypesCount = 6;

// ---------------------------------------------------------------------------

// wled00/FX.cpp:9415 mode_particleDrip() - "PS DripDrop". initParticleSystem1D(PartSys, 4) requests 4 sources but the effect only ever touches sources[0].
void mode_particle_drip(uint32_t /*now_ms*/, const Params &p, State &state, Frame frame) {
  if (state.call == 0) {
    ps1d::begin(false);
    ps1d::set_kill_out_of_bounds(true);
    ps1d::sources[0].source.hue = random8();
    state.aux1 = 0xFFFF;  // invalidate, guarantees the intensity-changed branch below fires this frame (and thus sets aux0 nonzero before it's used as a modulus)
  }

  ps1d::set_bounce(true);
  ps1d::set_wall_hardness(50);
  ps1d::set_motion_blur(p.custom2);
  ps1d::set_gravity(static_cast<int8_t>(p.custom3 >> 4));  // WLED: custom3>>1 assuming 0-31 (see file header); >>4 (+3 shift) keeps the same effective 0-15 result over 0-255
  ps1d::set_particle_size(p.option3 ? 1 : 0);
  ps1d::enable_particle_collisions(p.option2);

  ps1d::sources[0].source_flags.collide = false;

  if (p.option1) {  // rain mode: emit at random position, short life
    if (p.custom1 == 0) ps1d::set_bounce(false);
    ps1d::sources[0].var = 5;
    ps1d::sources[0].v = static_cast<int8_t>(-(8 + (p.speed >> 2)));
    ps1d::sources[0].min_life = 30;
    ps1d::sources[0].max_life = 200;
    ps1d::sources[0].source.x = static_cast<int32_t>(random16(static_cast<uint16_t>(ps1d::max_x)));
  } else {  // drip
    ps1d::sources[0].var = 0;
    ps1d::sources[0].v = static_cast<int8_t>(-(p.speed >> 1));
    ps1d::sources[0].min_life = 3000;
    ps1d::sources[0].max_life = 3000;
    ps1d::sources[0].source.x = ps1d::max_x - ps1d::kRadius;
  }

  if (state.aux1 != p.intensity) state.aux0 = 1;
  state.aux1 = p.intensity;

  if (state.call % state.aux0 == 0) {
    int32_t interval = 300 / (p.intensity + 1);
    state.aux0 = static_cast<uint16_t>(interval + random16(static_cast<uint16_t>(interval + 5)));
    ps1d::sources[0].source.hue = random8();
    ps1d::spray_emit(ps1d::sources[0]);
  }

  for (uint32_t i = 0; i < ps1d::used_particles; i++) {
    if (ps1d::particles[i].ttl) {
      if (!ps1d::particle_flags[i].collide) {  // collide flag distinguishes drop particles from splash particles here
        if (ps1d::particles[i].x < (ps1d::kRadius << 1)) {  // reached bottom
          if (ps1d::particles[i].ttl > 120) ps1d::particles[i].ttl = 120;
          if (p.custom1 > 0) {  // splash enabled
            ps1d::particles[i].ttl = 0;
            ps1d::sources[0].max_life = 160;
            ps1d::sources[0].min_life = 40;
            ps1d::sources[0].var = static_cast<int8_t>(10 + (p.custom1 >> 3));
            ps1d::sources[0].v = 0;
            ps1d::sources[0].source.hue = ps1d::particles[i].hue;
            ps1d::sources[0].source.x = ps1d::kRadius;
            ps1d::sources[0].source_flags.collide = true;
            for (int j = 0; j < 2 + (p.custom1 >> 2); j++) ps1d::spray_emit(ps1d::sources[0]);
          }
        }
      } else {
        ps1d::particles[i].ttl--;  // age splash particles faster
      }
    }
    if (p.option1 && ps1d::particles[i].hue < 245) ps1d::particles[i].hue = static_cast<uint8_t>(ps1d::particles[i].hue + 8);
    if (p.speed > 200) ps1d::particle_move_update(ps1d::particles[i], ps1d::particle_flags[i]);
  }

  ps1d::set_palette_colors(p.palette_id, p.primary, p.secondary, p.tertiary);
  ps1d::update();
  ps1d::render(frame);
}
EFFECTS_REGISTER(Id::kPsdrip, mode_particle_drip)

// wled00/FX.cpp:9527 mode_particlePinball() - "PS Pinball". Real WLED
// toggles a public `perParticleSize` bool directly when custom1==255 (all
// particles get individually-random size via advPartProps) -
// particle_system_1d.h now exposes that the same way (ps1d::per_particle_size),
// so this sets it directly too, matching real WLED exactly.
void mode_particle_pinball(uint32_t /*now_ms*/, const Params &p, State &state, Frame frame) {
  if (state.call == 0) {
    ps1d::begin(true);
    ps1d::sources[0].source_flags.collide = true;
    ps1d::sources[0].source.x = -1000;  // shoot up from below
    state.aux0 = 1;
    state.aux1 = 5000;  // out of range, forces the settings-changed branch below on the first real frame
  }

  const uint8_t c3 = static_cast<uint8_t>(p.custom3 >> 3);
  ps1d::set_gravity(static_cast<int8_t>(map_range(c3, 0, 31, 0, 8)));
  ps1d::set_bounce(c3 != 0);
  ps1d::set_motion_blur(p.custom2);
  ps1d::enable_particle_collisions(p.option1, 255);
  ps1d::set_color_by_position(p.option3);

  uint32_t max_particles = std::max<uint32_t>(20, p.intensity / (1u + (p.option2 * (p.custom1 >> 5))));
  if (p.custom1 < 255) {
    ps1d::set_particle_size(p.custom1);
  } else {
    ps1d::per_particle_size = true;
    max_particles *= 2;  // more headroom since individual sizing uses more space
  }
  ps1d::set_used_particles(static_cast<uint8_t>(max_particles));  // WLED passes an absolute particle count here too, implicitly truncated to the uint8_t percentage setUsedParticles() actually takes - transliterated as-is

  bool updateballs = false;
  if (state.aux1 != p.speed + p.intensity + p.option2 + p.custom1 + ps1d::used_particles) {
    state.step = state.call;
    updateballs = true;
    ps1d::sources[0].max_life = c3 ? 1000 : 0xFFFF;
    ps1d::sources[0].min_life = static_cast<uint16_t>(ps1d::sources[0].max_life >> 1);
  }

  if (p.option2) {  // rolling balls
    ps1d::set_gravity(0);
    ps1d::set_wall_hardness(255);
    int32_t speedsum = 0;
    for (uint32_t i = 0; i < ps1d::used_particles; i++) {
      ps1d::particles[i].ttl = 500;
      if (updateballs) {
        ps1d::particle_flags[i].collide = true;
        if (ps1d::particles[i].x == 0) {
          ps1d::particles[i].x = static_cast<int32_t>(random16(static_cast<uint16_t>(ps1d::max_x)));
          ps1d::particles[i].vx = (random16() & 0x01) ? 1 : -1;
        }
        ps1d::particles[i].hue = random8();
        ps1d::adv_particles[i].sat = 255;
        ps1d::adv_particles[i].size = random8();
      }
      speedsum += std::abs(static_cast<int32_t>(ps1d::particles[i].vx));
    }
    int32_t avg_speed = speedsum / static_cast<int32_t>(ps1d::used_particles);
    int32_t set_speed = 2 + (p.speed >> 2);
    if (avg_speed < set_speed) {
      for (int32_t i = 0; i < set_speed - avg_speed; i++) {
        uint32_t idx = random16(static_cast<uint16_t>(ps1d::used_particles));
        if (std::abs(static_cast<int32_t>(ps1d::particles[idx].vx)) < ps1d::kMaxSpeed)
          ps1d::particles[idx].vx = static_cast<int8_t>(ps1d::particles[idx].vx + (ps1d::particles[idx].vx >= 0 ? 1 : -1));
      }
    } else if (avg_speed > set_speed + 8) {
      ps1d::apply_friction(1);
    }
  } else {  // bouncing balls
    ps1d::set_wall_hardness(220);
    ps1d::sources[0].var = static_cast<int8_t>(p.speed >> 3);
    int32_t newspeed = 2 + (p.speed >> 1) - (p.speed >> 3);
    ps1d::sources[0].v = static_cast<int8_t>(newspeed);
    for (uint32_t i = 0; i < ps1d::used_particles; i++) {
      if (ps1d::particles[i].ttl < 50) ps1d::particles[i].ttl = 0;
      else if (ps1d::particles[i].vx == 0 && ps1d::particles[i].x < (ps1d::kRadius + p.custom1))
        ps1d::particles[i].ttl = static_cast<uint16_t>(ps1d::particles[i].ttl - 50);

      if (updateballs && c3 == 0) {
        ps1d::particles[i].vx = static_cast<int8_t>(ps1d::particles[i].vx > 0 ? newspeed : -newspeed);
      }
    }

    if (state.call > state.step) {
      int32_t interval = 260 - static_cast<int32_t>(p.intensity);
      state.step += static_cast<uint32_t>(interval + random16(static_cast<uint16_t>(interval)));
      ps1d::sources[0].source.hue = random16();
      ps1d::sources[0].sat = 255;
      ps1d::sources[0].size = random8();
      ps1d::spray_emit(ps1d::sources[0]);
    }
  }
  state.aux1 = static_cast<uint16_t>(p.speed + p.intensity + p.option2 + p.custom1 + ps1d::used_particles);

  ps1d::set_palette_colors(p.palette_id, p.primary, p.secondary, p.tertiary);
  ps1d::update();
  ps1d::render(frame);
}
EFFECTS_REGISTER(Id::kPspinball, mode_particle_pinball)

// wled00/FX.cpp:9645 mode_particleDancingShadows() - "PS Dancing Shadows".
void mode_particle_dancing_shadows(uint32_t /*now_ms*/, const Params &p, State &state, Frame frame) {
  if (state.call == 0) {
    ps1d::begin(false);
    ps1d::sources[0].max_life = 1000;
    ps1d::sources[0].min_life = ps1d::sources[0].max_life;
  }

  ps1d::set_motion_blur(p.custom1);
  ps1d::set_smear_blur(p.option1 ? 120 : 0);
  ps1d::set_particle_size(p.option3 ? 1 : 0);
  ps1d::set_color_by_position(p.option2);
  ps1d::set_used_particles(static_cast<uint8_t>(map_range(p.intensity, 0, 255, 10, 255)));

  uint32_t deadparticles = 0;
  for (uint32_t i = 0; i < ps1d::used_particles; i++) {
    if ((state.call & 0x07) == 0 && ps1d::particle_flags[i].outofbounds) {
      if (static_cast<int32_t>(ps1d::particles[i].vx) * ps1d::particles[i].x > 0) ps1d::particles[i].ttl = 0;
    }
    ps1d::particle_flags[i].perpetual = true;
    if (state.call % (32 / (1 + (p.custom2 >> 3))) == 0)
      ps1d::particles[i].hue = static_cast<uint8_t>(ps1d::particles[i].hue + 2 + (p.custom2 >> 5));
    if (state.aux0 != p.speed)  // speed changed - can't retroactively rescale which particle is which spotlight, so just re-set all current speeds
      ps1d::particles[i].vx = static_cast<int8_t>(ps1d::particles[i].vx > 0 ? (p.speed >> 3) : -(p.speed >> 3));
    if (ps1d::particles[i].ttl == 0) deadparticles++;
  }
  state.aux0 = p.speed;

  if (deadparticles > 5 && (state.call & 0x03) == 0) {
    uint32_t type = random16(kSpotTypesCount);
    int8_t speed = static_cast<int8_t>(2 + random16(static_cast<uint16_t>(2 + (p.speed >> 1))) + (p.speed >> 4));
    int32_t width = static_cast<int32_t>(random16(1, 10));
    uint32_t ttl = 300;
    int32_t position;
    if (random8() & 0x01) {
      position = static_cast<int32_t>(ps1d::kStripLength) - 1;
      speed = static_cast<int8_t>(-speed);
    } else {
      position = -width;
    }

    ps1d::sources[0].v = speed;
    ps1d::sources[0].source.hue = random8();
    for (int32_t i = 0; i < width; i++) {
      if (width > 1) {
        switch (type) {
          case kSpotTypeSolid:
            break;
          case kSpotTypeGradient: {
            uint8_t t = cubicwave8(static_cast<uint8_t>(map_range(i, 0, width - 1, 0, 255)));
            ttl = (static_cast<uint32_t>(t) * t) >> 8;
            break;
          }
          case kSpotType2xGradient: {
            uint8_t t = cubicwave8(static_cast<uint8_t>(2 * map_range(i, 0, width - 1, 0, 255)));
            ttl = (static_cast<uint32_t>(t) * t) >> 8;
            break;
          }
          case kSpotType2xDot:
            if (i > 0) position++;
            i++;
            break;
          case kSpotType3xDot:
            if (i > 0) position += 2;
            i += 2;
            break;
          case kSpotType4xDot:
            if (i > 0) position += 3;
            i += 3;
            break;
          default:
            break;
        }
      }
      ps1d::sources[0].source.x = position * ps1d::kRadius;
      int32_t partidx = ps1d::spray_emit(ps1d::sources[0]);
      if (partidx >= 0) ps1d::particles[static_cast<uint32_t>(partidx)].ttl = static_cast<uint16_t>(ttl);  // WLED assumes sprayEmit() always succeeds here; this port guards the -1 (no free particle) case instead of writing particles[-1]
      position++;
    }
  }

  ps1d::set_palette_colors(p.palette_id, p.primary, p.secondary, p.tertiary);
  ps1d::update();
  ps1d::render(frame);
}
EFFECTS_REGISTER(Id::kPsdancingshadows, mode_particle_dancing_shadows)

// wled00/FX.cpp:9759 mode_particleFireworks1D() - "PS Fireworks 1D". The
// extra `forcecounter[0]` byte WLED tacks onto PSdataEnd (additionalbytes=4
// requested, only index 0 used) becomes this one-field struct.
struct FireworksState {
  uint8_t force_counter;
};
static_assert(sizeof(FireworksState) <= State::kDataSize, "FireworksState too big");

void mode_particle_fireworks_1d(uint32_t /*now_ms*/, const Params &p, State &state, Frame frame) {
  auto &s = *reinterpret_cast<FireworksState *>(state.data);
  if (state.call == 0) {
    ps1d::begin(true);
    ps1d::set_kill_out_of_bounds(true);
    ps1d::sources[0].source_flags.custom1 = true;  // rocket state: standby
    s.force_counter = 0;
  }

  ps1d::set_motion_blur(p.custom2);
  int32_t gravity = 1 + (p.speed >> 3);
  ps1d::set_gravity(static_cast<int8_t>(p.speed ? gravity : 0));
  ps1d::set_particle_size(p.option3 ? 1 : 0);

  if (ps1d::sources[0].source_flags.custom1) {  // rocket on standby
    ps1d::sources[0].source.ttl--;
    if (ps1d::sources[0].source.ttl == 0) {  // relaunch
      state.aux0 = (random8() < p.custom1) ? 1 : 0;
      ps1d::sources[0].source_flags.custom1 = false;
      ps1d::sources[0].source.hue = random16();
      ps1d::sources[0].var = static_cast<int8_t>(10 * p.option2);
      ps1d::sources[0].v = static_cast<int8_t>(-10 * p.option2);
      ps1d::sources[0].min_life = 180;
      ps1d::sources[0].max_life = p.option2 ? 700 : 240;
      ps1d::sources[0].source.x = state.aux0 ? ps1d::max_x : 0;
      uint32_t speed = static_cast<uint32_t>(std::sqrt(static_cast<double>(
          (gravity * ((ps1d::max_x >> 2) + random16(static_cast<uint16_t>(ps1d::max_x >> 1)))) >> 4)));
      ps1d::sources[0].source.vx = static_cast<int8_t>(std::min<uint32_t>(speed, 127));
      ps1d::sources[0].source.ttl = 4000;
      ps1d::sources[0].sat = 30;
      ps1d::sources[0].source_flags.reversegrav = false;

      if (state.aux0) {
        ps1d::sources[0].source_flags.reversegrav = true;
        ps1d::sources[0].source.vx = static_cast<int8_t>(-ps1d::sources[0].source.vx);
        ps1d::sources[0].v = static_cast<int8_t>(-ps1d::sources[0].v);
      }
    }
  } else {  // rocket is launched
    int32_t rocketgravity = -gravity;
    int32_t currentspeed = ps1d::sources[0].source.vx;
    if (state.aux0) {
      rocketgravity = -rocketgravity;
      currentspeed = -currentspeed;
    }
    ps1d::apply_force(ps1d::sources[0].source, static_cast<int8_t>(rocketgravity), s.force_counter);
    ps1d::particle_move_update(ps1d::sources[0].source, ps1d::sources[0].source_flags);
    ps1d::particle_move_update(ps1d::sources[0].source, ps1d::sources[0].source_flags);  // called twice: doubles rocket speed/aging, matches WLED
    uint32_t rocketheight = state.aux0 ? static_cast<uint32_t>(ps1d::max_x - ps1d::sources[0].source.x)
                                        : static_cast<uint32_t>(ps1d::sources[0].source.x);

    if (currentspeed < 0 && ps1d::sources[0].source.ttl > 50) ps1d::sources[0].source.ttl = static_cast<uint16_t>(50 - gravity);

    if (ps1d::sources[0].source.ttl < 2) {  // explode
      ps1d::sources[0].source_flags.custom1 = true;
      ps1d::sources[0].var = static_cast<int8_t>(
          5 + ((((ps1d::max_x >> 1) + rocketheight) * (20 + (p.intensity << 1))) / (ps1d::max_x << 2)));
      ps1d::sources[0].min_life = 1200;
      ps1d::sources[0].max_life = 2600;
      ps1d::sources[0].source.ttl = static_cast<uint16_t>(100 + random16(static_cast<uint16_t>(64 - (p.speed >> 2))));
      const uint8_t c3 = static_cast<uint8_t>(p.custom3 >> 3);
      ps1d::sources[0].sat = c3 < 16 ? static_cast<uint8_t>(10 + (c3 << 4)) : 255;
      ps1d::sources[0].size = p.option3 ? static_cast<uint8_t>(random16(p.intensity)) : 0;
      uint32_t explosionsize = 8 + ((ps1d::kStripLength - 1) >> 2) +
                                (static_cast<uint32_t>(ps1d::sources[0].source.x) >> (ps1d::kRadiusShift - 1));
      explosionsize += random16(static_cast<uint16_t>((explosionsize * p.intensity) >> 8));
      ps1d::set_color_by_age(false);
      ps1d::set_color_by_position(false);
      for (uint32_t e = 0; e < explosionsize; e++) {
        int32_t idx = ps1d::spray_emit(ps1d::sources[0]);
        if (idx < 0) break;
        if (c3 > 23) {
          if (c3 == 31) {
            ps1d::set_color_by_age(p.option1);
            ps1d::set_color_by_position(!p.option1);
          } else {
            uint32_t uidx = static_cast<uint32_t>(idx);
            ps1d::particles[uidx].hue = static_cast<uint8_t>(map_range(
                std::abs(static_cast<int32_t>(ps1d::particles[uidx].vx)), 0, ps1d::sources[0].var, 0,
                16 + random16(200)));
            ps1d::particles[uidx].hue = static_cast<uint8_t>(ps1d::particles[uidx].hue + ps1d::sources[0].source.hue);
          }
        } else if (p.option1) {
          ps1d::sources[0].source.hue = random16();
        }
      }
    }
  }

  if ((state.call & 0x01) == 0 && !ps1d::sources[0].source_flags.custom1) ps1d::spray_emit(ps1d::sources[0]);
  if ((state.call & 0x03) == 0) ps1d::apply_friction(1);

  ps1d::set_palette_colors(p.palette_id, p.primary, p.secondary, p.tertiary);
  ps1d::update();
  ps1d::render(frame);

  for (uint32_t i = 0; i < ps1d::used_particles; i++) {  // ttl doubles as brightness; shorten the spark lifespan after this frame's render used the pre-decay value
    if (ps1d::particles[i].ttl > 20) ps1d::particles[i].ttl = static_cast<uint16_t>(ps1d::particles[i].ttl - 20);
    else ps1d::particles[i].ttl = 0;
  }
}
EFFECTS_REGISTER(Id::kPsfireworks1d, mode_particle_fireworks_1d)

// wled00/FX.cpp:9879 mode_particleSparkler() - "PS Sparkler". WLED requests
// 16 sources; this engine's kMaxSources is 8 (particle_system_1d.h's
// documented scaling for a 32-pixel strip), so this degrades gracefully to
// at most 8 simultaneous sparklers instead of 16 - not a bug, a consequence
// of that documented trade-off.
void mode_particle_sparkler(uint32_t /*now_ms*/, const Params &p, State &state, Frame frame) {
  if (state.call == 0) {
    ps1d::begin(true);
  }

  ps1d::Settings sparkler_settings{};
  sparkler_settings.wrap = !p.option2;
  sparkler_settings.bounce = p.option2;  // bounce always takes priority over wrap in particle_move_update()

  uint32_t num_sparklers = ps1d::num_sources;
  ps1d::set_motion_blur(p.custom2);
  ps1d::set_particle_size(p.option3 ? 60 : 0);

  for (uint32_t i = 0; i < num_sparklers; i++) {
    ps1d::sources[i].source.hue = random16();
    ps1d::sources[i].var = 0;
    ps1d::sources[i].min_life = static_cast<uint16_t>(150 + p.intensity);
    ps1d::sources[i].max_life = static_cast<uint16_t>(250 + (p.intensity << 1));
    int32_t speed = p.speed >> 1;
    if (p.option1) ps1d::sources[i].var = static_cast<int8_t>(p.intensity >> 3);
    ps1d::sources[i].source.vx = static_cast<int8_t>(ps1d::sources[i].source.vx > 0 ? speed : -speed);
    ps1d::sources[i].source.ttl = 400;
    ps1d::sources[i].sat = p.custom1;
    if (p.speed == 255) {
      ps1d::sources[i].source.x = static_cast<int32_t>(random16(static_cast<uint16_t>(ps1d::max_x)));
    } else {
      ps1d::particle_move_update(ps1d::sources[i].source, ps1d::sources[i].source_flags, &sparkler_settings);
    }
  }

  const uint8_t c3 = static_cast<uint8_t>(p.custom3 >> 3);
  num_sparklers = std::min<uint32_t>(1u + (c3 >> 1), num_sparklers);

  if (state.aux0 != c3) {  // used-sparkler count changed, redistribute
    for (uint32_t i = 1; i < num_sparklers; i++) {
      ps1d::sources[i].source.x =
          (ps1d::sources[0].source.x + (ps1d::max_x / static_cast<int32_t>(num_sparklers)) * static_cast<int32_t>(i)) %
          ps1d::max_x;
    }
  }
  state.aux0 = c3;

  for (uint32_t i = 0; i < num_sparklers; i++) {
    if (random8() % (((271 - p.intensity) >> 4)) == 0) ps1d::spray_emit(ps1d::sources[i]);
  }

  ps1d::set_palette_colors(p.palette_id, p.primary, p.secondary, p.tertiary);
  ps1d::update();
  ps1d::render(frame);

  uint32_t decay = 64 - (p.intensity >> 2);
  for (uint32_t i = 0; i < ps1d::used_particles; i++) {
    if (ps1d::particles[i].ttl > decay) ps1d::particles[i].ttl = static_cast<uint16_t>(ps1d::particles[i].ttl - decay);
    else ps1d::particles[i].ttl = 0;
  }
}
EFFECTS_REGISTER(Id::kPssparkler, mode_particle_sparkler)

// wled00/FX.cpp:9949 mode_particleHourglass() - "PS Hourglass". WLED's 8
// extra PSdataEnd bytes (a uint32_t settingTracker + a bool direction)
// become this struct - zero-initialized the same way state.data always is
// on a mode switch (State::reset()), matching WLED's own zeroed SEGENV.data,
// so the `p.intensity != s.setting_tracker` check below is guaranteed true
// on the first real frame unless intensity is set to exactly 0.
struct HourglassState {
  uint32_t setting_tracker;
  bool direction;
};
static_assert(sizeof(HourglassState) <= State::kDataSize, "HourglassState too big");

void mode_particle_hourglass(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  constexpr int32_t kPositionOffset = ps1d::kRadius / 2;
  auto &s = *reinterpret_cast<HourglassState *>(state.data);

  if (state.call == 0) {
    ps1d::begin(false);
    ps1d::set_bounce(true);
    ps1d::set_wall_hardness(100);
  }

  ps1d::set_used_particles(static_cast<uint8_t>(1 + ((static_cast<uint32_t>(p.intensity) * 255) >> 8)));
  ps1d::set_motion_blur(p.custom2);
  const uint8_t c3 = static_cast<uint8_t>(p.custom3 >> 3);
  ps1d::set_gravity(static_cast<int8_t>(map_range(c3, 0, 31, 1, 30)));
  ps1d::enable_particle_collisions(true, 64);

  uint32_t colormode = p.custom1 >> 5;

  if (p.intensity != s.setting_tracker) {  // (re)initialize
    s.setting_tracker = p.intensity;
    for (uint32_t i = 0; i < ps1d::used_particles; i++) {
      ps1d::particle_flags[i].reversegrav = true;
      s.direction = false;
      state.aux1 = 1;
    }
    state.aux0 = static_cast<uint16_t>(ps1d::used_particles - 1);
  }

  // re-order particles in case heavy collisions flipped them
  for (uint32_t i = 0; i + 1 < ps1d::used_particles; i++) {
    if (ps1d::particles[i].x < ps1d::particles[i + 1].x && !ps1d::particle_flags[i].fixed &&
        !ps1d::particle_flags[i + 1].fixed) {
      std::swap(ps1d::particles[i].x, ps1d::particles[i + 1].x);
    }
  }

  auto calc_target_pos = [&](uint32_t i) -> int32_t {
    return ps1d::particle_flags[i].reversegrav
               ? ps1d::max_x - static_cast<int32_t>(i) * ps1d::kRadius - kPositionOffset
               : (static_cast<int32_t>(ps1d::used_particles) - static_cast<int32_t>(i)) * ps1d::kRadius - kPositionOffset;
  };

  for (uint32_t i = 0; i < ps1d::used_particles; i++) {
    if (!ps1d::particle_flags[i].fixed && std::abs(static_cast<int32_t>(ps1d::particles[i].vx)) < 5) {
      int32_t targetposition = calc_target_pos(i);
      bool belowtarget = ps1d::particle_flags[i].reversegrav ? (ps1d::particles[i].x > targetposition)
                                                              : (ps1d::particles[i].x < targetposition);
      bool closetotarget = std::abs(targetposition - ps1d::particles[i].x) < ps1d::kRadius;
      if (belowtarget || closetotarget) {
        ps1d::particles[i].x = targetposition;
        ps1d::particle_flags[i].fixed = true;
      }
    }
    if (colormode == 7) {
      ps1d::set_color_by_position(true);
    } else {
      ps1d::set_color_by_position(false);
      uint8_t basehue = static_cast<uint8_t>((p.custom1 & 0x1F) << 3);
      switch (colormode) {
        case 0:
          ps1d::particles[i].hue = 120;
          break;
        case 1:
          ps1d::particles[i].hue = basehue;
          break;
        case 2:
        case 3:
          ps1d::particles[i].hue = static_cast<uint8_t>(((p.custom1 & 0x1F) << 1) + (i % 3) * 74);
          break;
        case 4:
          ps1d::particles[i].hue = static_cast<uint8_t>(basehue + (i * 255) / ps1d::used_particles);
          break;
        case 5:
          ps1d::particles[i].hue = static_cast<uint8_t>(basehue + (i * 1024) / ps1d::used_particles);
          break;
        case 6:
          ps1d::particles[i].hue = static_cast<uint8_t>(i + (now_ms >> 3));
          break;
        default:
          break;
      }
    }
    if (p.option1 && !ps1d::particle_flags[i].reversegrav)
      ps1d::particles[i].hue = static_cast<uint8_t>(ps1d::particles[i].hue + 120);
  }

  if (state.aux1 == 1) {  // last countdown tick before dropping starts: reset all particles
    for (uint32_t i = 0; i < ps1d::used_particles; i++) {
      ps1d::particle_flags[i].collide = true;
      ps1d::particle_flags[i].perpetual = true;
      ps1d::particles[i].ttl = 260;
      ps1d::particles[i].x = calc_target_pos(i);
      ps1d::particle_flags[i].fixed = true;
    }
  }

  if (state.aux1 == 0) {  // countdown passed, run
    if (now_ms >= state.step) {
      if (p.option3 && s.direction) state.step = now_ms + 100;
      else state.step = now_ms + std::max<uint32_t>(100, static_cast<uint32_t>(p.speed) * 100);
      if (state.aux0 < ps1d::used_particles) {
        ps1d::particle_flags[state.aux0].reversegrav = s.direction;
        ps1d::particle_flags[state.aux0].fixed = false;
      } else {  // overflow
        s.direction = !s.direction;
        state.aux1 = static_cast<uint16_t>((p.option2 ? ps1d::kStripLength : 0) + 100);
      }
      if (!s.direction) state.aux0 = static_cast<uint16_t>(state.aux0 - 1);
      else state.aux0 = static_cast<uint16_t>(state.aux0 + 1);
    }
  } else if (p.option2) {
    state.aux1 = static_cast<uint16_t>(state.aux1 - 1);
  }

  ps1d::set_palette_colors(p.palette_id, p.primary, p.secondary, p.tertiary);
  ps1d::update();
  ps1d::render(frame);
}
EFFECTS_REGISTER(Id::kPshourglass, mode_particle_hourglass)

// wled00/FX.cpp:10072 mode_particle1Dspray() - "PS Spray 1D".
void mode_particle_1d_spray(uint32_t /*now_ms*/, const Params &p, State &state, Frame frame) {
  if (state.call == 0) {
    ps1d::begin(false);
    ps1d::set_kill_out_of_bounds(true);
    ps1d::set_wall_hardness(150);
    ps1d::set_particle_size(1);
  }

  ps1d::set_bounce(p.option2);
  ps1d::set_motion_blur(p.custom2);
  const uint8_t c3 = static_cast<uint8_t>(p.custom3 >> 3);
  int32_t gravity = -(static_cast<int32_t>(c3) - 16);  // 0-15 (of c3) is positive/down, 17-31 is negative/up
  ps1d::set_gravity(static_cast<int8_t>(std::abs(gravity)));

  ps1d::sources[0].source.hue = static_cast<uint8_t>(state.aux0);
  ps1d::sources[0].var = 20;
  ps1d::sources[0].min_life = 200;
  ps1d::sources[0].max_life = 400;
  ps1d::sources[0].source.x = static_cast<int32_t>(map_range(p.custom1, 0, 255, 0, ps1d::max_x));
  ps1d::sources[0].v =
      static_cast<int8_t>(map_range(p.speed, 0, 255, -127 + ps1d::sources[0].var, 127 - ps1d::sources[0].var));
  ps1d::sources[0].source_flags.reversegrav = gravity < 0;

  if (random8() % (1 + ((255 - p.intensity) >> 3)) == 0) {
    ps1d::spray_emit(ps1d::sources[0]);
    state.aux0++;
  }

  ps1d::set_color_by_age(p.option1);  // overruled by color-by-position below
  ps1d::set_color_by_position(p.option3);
  for (uint32_t i = 0; i < ps1d::used_particles; i++) {
    ps1d::particle_flags[i].reversegrav = ps1d::sources[0].source_flags.reversegrav;
  }

  ps1d::set_palette_colors(p.palette_id, p.primary, p.secondary, p.tertiary);
  ps1d::update();
  ps1d::render(frame);
}
EFFECTS_REGISTER(Id::kPs1dspray, mode_particle_1d_spray)

// wled00/FX.cpp:10122 mode_particleBalance() - "PS 1D Balance".
void mode_particle_balance(uint32_t /*now_ms*/, const Params &p, State &state, Frame frame) {
  if (state.call == 0) {
    ps1d::begin(false);
    ps1d::set_particle_size(1);
  }

  ps1d::set_motion_blur(p.custom2);
  ps1d::set_bounce(!p.option2);
  ps1d::set_wrap(p.option2);
  uint8_t hardness = p.custom1 > 0 ? static_cast<uint8_t>(map_range(p.custom1, 0, 255, 50, 250)) : 200;
  ps1d::enable_particle_collisions(p.custom1 != 0, hardness);
  ps1d::set_wall_hardness(200);
  ps1d::set_used_particles(static_cast<uint8_t>(map_range(p.intensity, 0, 255, 10, 255)));

  if (ps1d::used_particles > state.aux1) {  // more particles available, reinitialize
    for (uint32_t i = 0; i < ps1d::used_particles; i++) {
      ps1d::particles[i].x = static_cast<int32_t>(i) * ps1d::kRadius;
      ps1d::particles[i].ttl = 300;
      ps1d::particle_flags[i].perpetual = true;
      ps1d::particle_flags[i].collide = true;
    }
  }
  state.aux1 = static_cast<uint16_t>(ps1d::used_particles);

  for (uint32_t i = 0; i + 1 < ps1d::used_particles; i++) {  // re-order particles in case collisions flipped them
    if (ps1d::particles[i].x > ps1d::particles[i + 1].x) {
      if (p.option2 && (ps1d::particles[i].x - ps1d::particles[i + 1].x > 3 * ps1d::kRadius)) continue;  // wrapped around, not actually out of order
      std::swap(ps1d::particles[i].x, ps1d::particles[i + 1].x);
    }
  }

  const uint8_t c3 = static_cast<uint8_t>(p.custom3 >> 3);
  if (state.call % (((255u - p.speed) >> 6) + 1) == 0) {
    int32_t xgravity;
    int32_t increment = (p.speed >> 6) + 1;
    state.aux0 = static_cast<uint16_t>(state.aux0 + increment);
    if (p.option3) xgravity = static_cast<int32_t>(perlin8(state.aux0)) - 128;
    else xgravity = static_cast<int32_t>(cos8(static_cast<uint8_t>(state.aux0))) - 128;
    xgravity = (xgravity * ((static_cast<int32_t>(c3) + 1) << 2)) / 128;
    ps1d::apply_force(static_cast<int8_t>(xgravity));
  }

  uint32_t randomindex = random16(static_cast<uint16_t>(ps1d::used_particles));
  ps1d::particles[randomindex].vx =
      static_cast<int8_t>((static_cast<int32_t>(ps1d::particles[randomindex].vx) * 200) / 255);  // friction on one random particle, reduces clumping

  if ((state.call & 0x0F) == 0 && c3 > 4) ps1d::apply_friction(1);

  ps1d::set_color_by_position(p.option1);
  if (!p.option1) {
    for (uint32_t i = 0; i < ps1d::used_particles; i++) {
      ps1d::particles[i].hue = static_cast<uint8_t>((1024u * i) / ps1d::used_particles);
    }
  }

  ps1d::set_palette_colors(p.palette_id, p.primary, p.secondary, p.tertiary);
  ps1d::update();
  ps1d::render(frame);
}
EFFECTS_REGISTER(Id::kPsbalance, mode_particle_balance)

// wled00/FX.cpp:10202 mode_particleChase() - "PS Chase". WLED's 2 extra
// PSdataEnd bytes (huedir, stepdir) become this struct.
struct ChaseState {
  int8_t hue_dir;
  int8_t step_dir;
};
static_assert(sizeof(ChaseState) <= State::kDataSize, "ChaseState too big");

void mode_particle_chase(uint32_t now_ms, const Params &p, State &state, Frame frame) {
  auto &s = *reinterpret_cast<ChaseState *>(state.data);
  if (state.call == 0) {
    ps1d::begin(true);
    state.aux0 = 0xFFFF;  // invalidate
    s.hue_dir = 1;
    s.step_dir = 1;
  }

  ps1d::set_color_by_position(p.option3);
  const uint8_t c3 = static_cast<uint8_t>(p.custom3 >> 3);
  ps1d::set_motion_blur(static_cast<uint8_t>(7 + (c3 << 3)));

  uint32_t numparticles = static_cast<uint32_t>(
      1 + map_range(p.intensity, 0, 255, 0, static_cast<long>(ps1d::used_particles) / (1 + (p.custom1 >> 5))));
  numparticles = std::min<uint32_t>(numparticles, ps1d::used_particles);
  int32_t huestep = 1 + static_cast<int32_t>(((static_cast<uint32_t>(p.custom2) << 19) / numparticles) >> 16);
  uint32_t settingssum = static_cast<uint32_t>(p.speed) + p.intensity + p.custom1 + p.custom2 + p.option1 + p.option2 + p.option3;

  if (state.aux0 != settingssum) {  // settings changed, redistribute
    if (p.option1) {
      state.step = static_cast<uint32_t>(ps1d::adv_particles[0].size / 2 + (ps1d::max_x / static_cast<int32_t>(numparticles)));
    } else {
      state.step = static_cast<uint32_t>((ps1d::max_x + (ps1d::kRadius << 6)) / static_cast<int32_t>(numparticles));
      state.step = (state.step / static_cast<uint32_t>(ps1d::kRadius)) * static_cast<uint32_t>(ps1d::kRadius);  // round to nearest subpixel unit so particles move in unison
    }
    for (uint32_t i = 0; i < ps1d::used_particles; i++) {
      ps1d::adv_particles[i].sat = 255;
      ps1d::particles[i].x = (static_cast<int32_t>(i) - 1) * static_cast<int32_t>(state.step);
      ps1d::particles[i].vx = static_cast<int8_t>(p.speed >> 2);
      ps1d::adv_particles[i].size = p.custom1;
      if (p.custom2 < 255) ps1d::particles[i].hue = static_cast<uint8_t>(i * huestep);
      else ps1d::particles[i].hue = random16();
    }
    state.aux0 = static_cast<uint16_t>(settingssum);
  }

  if (p.option1) {  // playful mode: vary the gradient spread over time
    huestep = 1 + ((std::max<int32_t>(huestep, 3) *
                     (static_cast<int32_t>(sin16(static_cast<uint16_t>(now_ms * 3))) + 32767)) >>
                    15);
  }

  for (int32_t i = static_cast<int32_t>(ps1d::used_particles) - 1; i >= 0; i--) {  // check from the back: last particle wraps first
    uint32_t ui = static_cast<uint32_t>(i);
    if (ps1d::particles[ui].x > ps1d::max_x + ps1d::kRadius + ps1d::adv_particles[ui].size) {
      uint32_t nextindex = static_cast<uint32_t>(i + 1) % ps1d::used_particles;
      ps1d::particles[ui].x = ps1d::particles[nextindex].x - static_cast<int32_t>(state.step);
      if (p.option1)
        ps1d::adv_particles[ui].size = static_cast<uint8_t>(std::max<int32_t>(
            1 + (p.custom1 >> 1), (static_cast<int32_t>(sin16(static_cast<uint16_t>(now_ms << 1))) + 32767) >> 8));
      if (p.custom2 < 255) ps1d::particles[ui].hue = static_cast<uint8_t>(ps1d::particles[nextindex].hue - huestep);
      else ps1d::particles[ui].hue = random16();
    }
    ps1d::particles[ui].ttl = 300;  // can't use perpetual: the memory backing particles[] never moves in this port, but kept for parity with WLED's own comment/behavior
  }

  if (p.option1) {  // playful mode: dynamically vary hue/size/speed/density
    if (s.step_dir == 0) s.step_dir = 1;
    if (s.hue_dir == 0) s.hue_dir = 1;
    if (state.step >= static_cast<uint32_t>(ps1d::adv_particles[0].size + ps1d::kRadius * 4) +
                           static_cast<uint32_t>(ps1d::max_x) / numparticles)
      s.step_dir = -1;
    else if (state.step <= static_cast<uint32_t>(ps1d::adv_particles[0].size >> 1) +
                                (static_cast<uint32_t>(ps1d::max_x) / numparticles))
      s.step_dir = 1;
    if (state.aux1 > 512) s.hue_dir = -1;
    else if (state.aux1 < 50) s.hue_dir = 1;
    if (state.call % (1024 / (1 + (p.speed >> 2))) == 0) state.aux1 = static_cast<uint16_t>(state.aux1 + s.hue_dir);
    int8_t globalhuestep = 0;
    if (state.call % (1 + (static_cast<uint32_t>(static_cast<int32_t>(sin16(static_cast<uint16_t>(now_ms)) + 32767)) >> 12)) == 0)
      globalhuestep = 2;
    if ((state.call & 0x1F) == 0) state.step = static_cast<uint32_t>(static_cast<int32_t>(state.step) + s.step_dir);
    for (uint32_t i = 0; i < ps1d::used_particles; i++) {
      ps1d::particles[i].hue = static_cast<uint8_t>(ps1d::particles[i].hue - globalhuestep);
      ps1d::particles[i].vx = static_cast<int8_t>(
          1 + (p.speed >> 2) +
          ((static_cast<int32_t>(sin16(static_cast<uint16_t>(now_ms >> 1)) + 32767) * (p.speed >> 2)) >> 16));
    }
  }

  ps1d::set_palette_colors(p.palette_id, p.primary, p.secondary, p.tertiary);
  ps1d::update();
  ps1d::render(frame);
}
EFFECTS_REGISTER(Id::kPschase, mode_particle_chase)

// wled00/FX.cpp:10297 mode_particleStarburst() - "PS Starburst".
void mode_particle_starburst(uint32_t /*now_ms*/, const Params &p, State &state, Frame frame) {
  if (state.call == 0) {
    ps1d::begin(true);
    ps1d::set_kill_out_of_bounds(true);
    ps1d::enable_particle_collisions(true, 200);
    ps1d::sources[0].source.ttl = 1;
    ps1d::sources[0].sat = 0;
  }

  ps1d::set_motion_blur(p.custom2);
  ps1d::set_gravity(static_cast<int8_t>(p.option1 ? 8 : 0));

  if (ps1d::sources[0].source.ttl-- == 0) {  // standby time elapsed - relies on the same ttl underflow-then-overwrite WLED's own source does
    uint32_t explosionsize = 4 + random16(static_cast<uint16_t>(p.intensity >> 2));
    ps1d::sources[0].source.hue = random16();
    ps1d::sources[0].var = static_cast<int8_t>(10 + (explosionsize << 1));
    ps1d::sources[0].min_life = 150;
    ps1d::sources[0].max_life = 300;
    ps1d::sources[0].source.x = static_cast<int32_t>(random16(static_cast<uint16_t>(ps1d::max_x)));
    ps1d::sources[0].source.ttl = static_cast<uint16_t>(10 + random16(static_cast<uint16_t>(255 - p.speed)));
    ps1d::sources[0].size = p.custom1;
    ps1d::sources[0].source_flags.collide = p.option3;
    for (uint32_t e = 0; e < explosionsize; e++) {
      if (p.option2) ps1d::sources[0].source.hue = random16();
      ps1d::spray_emit(ps1d::sources[0]);
    }
  }

  const uint8_t c3 = static_cast<uint8_t>(p.custom3 >> 3);
  for (uint32_t i = 0; i < ps1d::used_particles; i++) {  // shrink and desaturate all particles over time
    if (ps1d::adv_particles[i].size) ps1d::adv_particles[i].size--;
    if (ps1d::adv_particles[i].sat < 250)
      ps1d::adv_particles[i].sat = static_cast<uint8_t>(ps1d::adv_particles[i].sat + 2 + (c3 >> 3));
  }

  if (state.call % 5 == 0) ps1d::apply_friction(1);

  ps1d::set_palette_colors(p.palette_id, p.primary, p.secondary, p.tertiary);
  ps1d::update();
  ps1d::render(frame);
}
EFFECTS_REGISTER(Id::kPsstarburst, mode_particle_starburst)

// wled00/FX.cpp:10427 mode_particleFire1D() - "PS Fire 1D".
void mode_particle_fire_1d(uint32_t /*now_ms*/, const Params &p, State &state, Frame frame) {
  if (state.call == 0) {
    ps1d::begin(false);
    ps1d::set_kill_out_of_bounds(true);
    ps1d::set_particle_size(1);
    ps1d::num_sources = 5;  // WLED requests exactly 5 (3 base flames + 2 extra); comfortably within this engine's kMaxSources=8
  }

  ps1d::set_motion_blur(static_cast<uint8_t>(128 + (p.custom2 >> 1)));
  ps1d::set_color_by_age(true);
  uint32_t emitparticles = 1;
  uint32_t j = random16();
  for (uint32_t i = 0; i < 3; i++) {  // 3 base flames
    if (ps1d::sources[i].source.ttl > 50) ps1d::sources[i].source.ttl = static_cast<uint16_t>(ps1d::sources[i].source.ttl - 10);
    else ps1d::sources[i].source.ttl = static_cast<uint16_t>(100 + random16(200));
  }
  for (uint32_t i = 0; i < ps1d::num_sources; i++) {
    j = (j + 1) % ps1d::num_sources;
    ps1d::sources[j].source.x = 0;
    ps1d::sources[j].var = static_cast<int8_t>(2 + (p.speed >> 4));
    if (j > 2) {
      ps1d::sources[j].min_life = static_cast<uint16_t>(150 + p.intensity + (j << 2));
      ps1d::sources[j].max_life = static_cast<uint16_t>(200 + p.intensity + (j << 3));
      ps1d::sources[j].v = static_cast<int8_t>(p.speed >> (2 + (j << 1)));
      if (emitparticles) {
        emitparticles--;
        ps1d::spray_emit(ps1d::sources[j]);
      }
    } else {
      ps1d::sources[j].min_life = static_cast<uint16_t>(ps1d::sources[j].source.ttl + p.intensity);
      ps1d::sources[j].max_life = static_cast<uint16_t>(ps1d::sources[j].min_life + 50);
      ps1d::sources[j].v = static_cast<int8_t>(p.speed >> 2);
      if (state.call & 0x01) ps1d::spray_emit(ps1d::sources[j]);
    }
  }

  for (uint32_t i = 0; i < ps1d::used_particles; i++) {
    ps1d::particles[i].x += ps1d::particles[i].ttl >> 7;  // 'hot' particles move faster
    if (ps1d::particles[i].ttl > 3 + ((255 - p.custom1) >> 1))
      ps1d::particles[i].ttl =
          static_cast<uint16_t>(ps1d::particles[i].ttl - static_cast<uint16_t>(map_range(p.custom1, 0, 255, 1, 3)));
  }

  ps1d::set_palette_colors(p.palette_id, p.primary, p.secondary, p.tertiary);
  ps1d::update();
  ps1d::render(frame);
}
EFFECTS_REGISTER(Id::kPsfire1d, mode_particle_fire_1d)

}  // namespace
}  // namespace effects
