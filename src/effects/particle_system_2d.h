#pragma once

#include <cstdint>

#include "effects.h"

// Port of real WLED's 2D particle-physics engine (wled00/FXparticleSystem.h/
// .cpp's ParticleSystem2D class, by DedeHai) - the shared core that every
// particle-based 2D effect (fire, fireworks, vortex, ballpit, box, impact,
// waterfall, ghostrider, galaxy, volcano, fuzzy-noise, ...) is built on top
// of. ParticleSystem1D (a separate class in the same real-WLED file) is out
// of scope here - it's ported into a different file by a different agent.
//
// Two deliberate adaptations from real WLED, made everywhere below without
// further comment at each site:
//  1. No dynamic memory. Real WLED slices a shared per-effect memory pool
//     (SEGENV.data) at runtime, with particle/source counts computed from
//     segment pixel count and available heap (calculateNumberOfParticles2D(),
//     allocateParticleSystemMemory2D(), MAXPARTICLES_2D tiers for ESP8266 /
//     ESP32-S2 / ESP32). This board is always exactly 32x32 on an RP2040
//     with plain static RAM, so there's no equivalent to port: particle/
//     source/advanced-property storage is fixed-size static arrays sized
//     once below (kMaxParticles/kMaxSources), and this whole module is a
//     singleton (there's only ever one particle-based effect selected at a
//     time) rather than a constructible class - the same shape as this
//     codebase's palettes.cpp/realtime_udp.cpp, each owning one piece of
//     module-level state. begin() replaces the constructor AND real WLED's
//     separate initParticleSystem2D()/allocateParticleSystemMemory2D() init
//     path (which had nothing left to do once sizing is fixed at compile
//     time) - call it once from an effect's `state.call == 0` check, the
//     same one-time-setup convention every other stateful effect here uses.
//  2. No white channel. Real WLED renders particles through CRGBW (RGB + W).
//     This board's LEDs are plain RGB, so CRGBW becomes this codebase's
//     Rgb{r,g,b} everywhere, W dropped entirely - matching how already-
//     ported effects here (e.g. gen_batch1.cpp's Aurora) already drop
//     WLED's white-channel handling.
//
// A handful of smaller, forced deviations (not requested, but unavoidable
// given the two adaptations above and this codebase's architecture) are
// noted at the specific declarations they affect, and summarized again in
// particle_system_2d.cpp's top comment.
namespace effects {
namespace ps2d {

// ---- sizing (adaptation #1 - see file header) ----
// Sized in the same relative proportion as real WLED's own ESP8266 tier
// (MAXPARTICLES_2D=256, MAXSOURCES_2D=24 for that tier), which targets a
// pixel count in the same ballpark as this board's 32x32=1024 pixels.
// kMaxSources is fixed at 16 per this port's explicit sizing brief (a
// deliberately rounder, slightly more conservative number than WLED's own
// 24 for the same tier, still generous for any effect built on this).
inline constexpr uint32_t kMaxParticles = 256;
inline constexpr uint32_t kMaxSources = 16;

// ---- fixed-point sub-pixel geometry (wled00/FXparticleSystem.h) ----
// Each pixel is subdivided into kPRadius sub-pixel units for particle
// movement math - unchanged from real WLED's 2D constants.
inline constexpr int32_t kPRadius = 64;               // PS_P_RADIUS
inline constexpr int32_t kPHalfRadius = kPRadius / 2;  // PS_P_HALFRADIUS
inline constexpr int32_t kPRadiusShift = 6;            // PS_P_RADIUS_SHIFT
inline constexpr int32_t kPSurface = 12;               // PS_P_SURFACE: 2^12 == kPRadius^2
inline constexpr int32_t kPMinHardRadius = 64;         // PS_P_MINHARDRADIUS
inline constexpr int32_t kPMinSurfaceHardness = 128;   // PS_P_MINSURFACEHARDNESS
inline constexpr int32_t kMaxParticleSpeed = 120;      // PS_P_MAXSPEED

// wled00/FXparticleSystem.h's file-scope limitSpeed() - clamps a velocity
// to +/-kMaxParticleSpeed (kept below int8_t's +/-127 range so collision
// math never overflows on rounding).
inline int32_t limit_speed(int32_t speed) {
  return speed > kMaxParticleSpeed ? kMaxParticleSpeed : (speed < -kMaxParticleSpeed ? -kMaxParticleSpeed : speed);
}

// wled00/FXparticleSystem.h's file-scope calculateEllipseBrightness() -
// distance-based brightness (0-255) for large-particle ellipse rendering.
inline uint8_t calculate_ellipse_brightness(int32_t dx, int32_t dy, int32_t rxsq, int32_t rysq, uint8_t max_brightness) {
  uint32_t dx_sq = static_cast<uint32_t>(dx * dx);
  uint32_t dy_sq = static_cast<uint32_t>(dy * dy);
  uint32_t dist_sq = ((dx_sq << 8) / static_cast<uint32_t>(rxsq)) + ((dy_sq << 8) / static_cast<uint32_t>(rysq));
  if (dist_sq >= 256) return 0;
  int32_t falloff = 256 - static_cast<int32_t>(dist_sq);
  return static_cast<uint8_t>((max_brightness * falloff) >> 8);
}

// ---- public structs (PSparticle/PSparticleFlags/PSsource/PSadvancedParticle
// /PSsizeControl/PSsettings2D, "PS" prefix dropped per this port's naming) --

// PSparticle (10 bytes in real WLED). Coordinates are in sub-pixel units
// (see kPRadius above): a pixel's on-screen position is x >> kPRadiusShift.
struct Particle {
  int16_t x;     // x position, sub-pixel units
  int16_t y;     // y position, sub-pixel units
  uint16_t ttl;  // time to live, in frames; 0 == dead
  int8_t vx;     // horizontal velocity
  int8_t vy;     // vertical velocity
  uint8_t hue;   // color hue / palette index
  uint8_t sat;   // color saturation
};

// PSparticleFlags. Real WLED packs this into a 1-byte bitfield union purely
// to keep per-particle RAM down under its own dynamic-allocation budget;
// with fixed static arrays that budget pressure doesn't apply here, so this
// is a plain bitfield struct (no byte-at-a-time union access) - same total
// footprint, simpler to construct/read.
struct ParticleFlags {
  bool out_of_bounds : 1;  // set by particle_move_update() if outside the matrix
  bool collide : 1;        // if set, this particle takes part in collisions
  bool perpetual : 1;      // if set, ttl is not decremented (still dies from kill-out-of-bounds)
  bool custom1 : 1;        // unused by the engine - free for an effect to track its own per-particle state
  bool custom2 : 1;
  bool custom3 : 1;
  bool custom4 : 1;
  bool custom5 : 1;
};

// PSadvancedParticle (optional per-particle size).
struct AdvancedParticle {
  uint8_t size;          // particle size; 255 == 10px diameter (see per_particle_size)
  uint8_t force_counter;  // per-particle counter for apply_force()/apply_angle_force()
};

// PSsizeControl (optional per-particle grow/shrink/wobble/asymmetry state
// machine, driven each frame by the private update_size() helper inside
// update()).
struct SizeControl {
  uint8_t asymmetry;         // 0 = symmetrical, 255 = fully asymmetric
  uint8_t asymdir;           // asymmetry direction; 64 == x-axis, 192 == y-axis
  uint8_t maxsize;           // target size when growing
  uint8_t minsize;           // target size when shrinking (0 kills the particle)
  uint8_t size_counter : 4;  // sub-frame accumulator for slow grow/shrink speeds
  uint8_t wobble_counter : 4;
  uint8_t grow_speed : 4;
  uint8_t shrink_speed : 4;
  uint8_t wobble_speed : 4;
  bool grow : 1;
  bool shrink : 1;
  bool pulsate : 1;  // grow, then shrink, then grow again, ...
  bool wobble : 1;   // alternate asymmetry between x and y
};

// PSsource (20 bytes in real WLED) - a particle emitter: itself a Particle
// (for position/velocity/color), plus emission parameters.
struct Source {
  uint16_t min_life;  // minimum ttl of emitted particles
  uint16_t max_life;  // maximum ttl of emitted particles
  Particle source;    // emitter's own position/velocity/color
  ParticleFlags source_flags;
  int8_t var;  // +/- random speed variation applied to emitted particles
  int8_t vx;   // emitting speed
  int8_t vy;
  uint8_t size;  // advanced per-particle size given to emitted particles
};

// PSsettings2D - per-axis wrap/bounce/gravity/collision/color options. A
// plain bool struct rather than real WLED's packed 1-byte bitfield union:
// there's exactly one instance of this driving default physics (private,
// set via the set_*() functions below) plus whatever an effect builds ad
// hoc to pass as particle_move_update()'s `options` override, so the byte-
// packing real WLED relies on to keep per-instance RAM down doesn't buy
// anything here.
struct Settings2D {
  bool wrap_x = false;
  bool wrap_y = false;
  bool bounce_x = false;
  bool bounce_y = false;
  bool kill_out_of_bounds = false;  // out-of-bounds particles die immediately
  bool use_gravity = false;         // disables bounce_y at the top while set
  bool use_collisions = false;
  bool color_by_age = false;  // hue is set from ttl each frame in particle_move_update()
};

// ---- lifecycle ----

// Real WLED's ParticleSystem2D constructor + initParticleSystem2D() +
// allocateParticleSystemMemory2D() collapsed into one call, since none of
// those exist to size or allocate anything on this fixed-size board (see
// adaptation #1 above) - what's left is exactly the constructor's state
// reset. Call once from an effect's `state.call == 0` check before using
// anything else in this namespace. Resets every particle/source/advanced-
// property slot and all settings to real WLED's own constructor defaults
// (wall hardness 255, wall roughness 0, gravity off, particle size 1,
// motion/smear blur off, all wrap/bounce/collision settings off).
// `advanced`/`size_control` mirror the constructor's isadvanced/sizecontrol
// parameters - requesting size_control implies advanced (matches real
// WLED's initParticleSystem2D(), which forces the same). When not
// requested, adv_particles/adv_size are left null, matching real WLED's
// pointer-null convention for "not in use".
void begin(bool advanced, bool size_control);

// Real WLED's updateSystem() - refreshes the class's cached matrix
// dimensions from the current segment size every FX call, since real WLED
// segments can be resized/reconfigured between calls. This board's matrix
// is always exactly 32x32, so there's nothing to refresh; kept as a no-op
// for call-site parity with real WLED's own per-frame call pattern.
void update_system();

// Real WLED's update(): applies gravity (if enabled), advances advanced
// per-particle size control, handles collisions (if enabled), then moves
// every used particle. Real WLED's update() also renders at the end of
// this same call (it can, since its render() reaches the segment's pixel
// buffer through global state); this port's render() instead takes an
// explicit Frame (see below) that update() has no way to receive, so here
// the two are split - call update() once per frame after emitting/forcing
// whatever the effect needs this frame, then call render(frame) right
// after it, every frame, to actually draw the result.
void update();

// Real WLED's updateFire(): fire-specific move step (fireParticleupdate()
// - upward drift that increases as a particle cools, no gravity/collisions,
// no rendering) and sets the fire brightness ramp render() below uses in
// place of ttl/hue for every particle until the next update()/update_fire()
// call. Same update()/render() split as above: call render(frame) right
// after this, every frame.
void update_fire(uint8_t intensity);

// Real WLED's render(): blits the current particle state into `frame`.
// Public here (real WLED's is private, reached only from inside update()/
// update_fire()) because this codebase has no equivalent to WLED's global
// "current segment" the class can render into on its own - an effect must
// call this explicitly, every frame, right after update() or update_fire()
// (see both above). Accumulates every particle's contribution into a local
// 32x32 buffer first (particles overlapping the same pixel add, saturating
// without hue-shifting toward white - see particle_system_2d.cpp's
// fast_color_scale_add()), then resolves that buffer into `frame` in one
// pass - matching real WLED's own accumulate-then-blit rendering exactly.
//
// set_colors() has no real-WLED equivalent: real WLED's render() reads the
// segment's live palette/colors directly (SEGPALETTE/SEGCOLOR()); this
// module has no segment to read, so an effect must push its current
// Params-derived palette/colors in before calling render() (typically once
// per frame, since a user can change them live).
void set_colors(uint8_t palette_id, Rgb primary, Rgb secondary, Rgb tertiary);
void render(Frame frame);

// ---- particle emitters ----

// Real WLED's sprayEmit(): finds a dead particle and emits it from
// `emitter` with +/-emitter.var random velocity variation (circular for
// var > 5, for nicer "explosions"). Returns the emitted particle's index,
// or -1 if every particle slot is alive.
int32_t spray_emit(const Source &emitter);

// Real WLED's flameEmit(): spray_emit() plus adding the emitter's own ttl
// on top (so a longer-lived flame source produces longer-lived particles).
void flame_emit(const Source &emitter);

// Real WLED's angleEmit(): sets emitter.vx/vy from `angle` (0-65535 ==
// 0-360deg, 0 == +x direction) and `speed`, then spray_emit()s it.
int32_t angle_emit(Source &emitter, uint16_t angle, int32_t speed);

// ---- particle physics ----

void apply_gravity(Particle &part);  // apply system gravity to one particle (e.g. a source)

// Apply a linear force (3.4 fixed-point notation: force=16 means +1 speed/
// frame, the default force=8 means +1 every other frame) to one particle.
// `counter` must be a per-particle uint8_t that persists between calls
// (packs x/y sub-frame accumulators into its low/high nibbles).
void apply_force(Particle &part, int8_t xforce, int8_t yforce, uint8_t &counter);
// Same, but sourcing the counter from adv_particles[particle_index] -
// requires begin(true, ...) to have been called.
void apply_force(uint32_t particle_index, int8_t xforce, int8_t yforce);
// Apply to every used particle at once (uses a shared internal counter).
void apply_force(int8_t xforce, int8_t yforce);

// Same as apply_force(), but the force direction is given as an angle
// (0-65535 == 0-360deg, 0 == +x direction) instead of separate x/y.
void apply_angle_force(Particle &part, int8_t force, uint16_t angle, uint8_t &counter);
void apply_angle_force(uint32_t particle_index, int8_t force, uint16_t angle);
void apply_angle_force(int8_t force, uint16_t angle);

// Slow a particle down by `coefficient`/255 per frame (255 == instant
// stop; negative speeds it up instead - "a feature, not a bug", per real
// WLED's own comment).
void apply_friction(Particle &part, int32_t coefficient);
void apply_friction(int32_t coefficient);  // apply to every used particle

// Inverse-square attraction of particles[particle_index] toward
// `attractor`; if `swallow` is set, particles that get very close instead
// age out quickly (a black-hole-style effect). Requires begin(true, ...).
void point_attractor(uint32_t particle_index, Particle &attractor, uint8_t strength, bool swallow);

// Real WLED's particleMoveUpdate(): ages, colors (if color_by_age),
// bounces/wraps/kills-out-of-bounds per axis, and advances one particle by
// its velocity. `options` defaults to the module's own settings (as set
// via the set_*() functions below) when null - pass a custom Settings2D to
// move a particle under different rules (real WLED's own use case: an FX
// running several independent "virtual strips" with different settings
// sharing one particle pool).
void particle_move_update(Particle &part, ParticleFlags &part_flags, Settings2D *options = nullptr,
                           AdvancedParticle *advanced_properties = nullptr);

// ---- settings (real WLED's set*()/enableParticleCollisions()) ----

void set_used_particles(uint8_t percentage);  // 255 == 100% of kMaxParticles
void set_collision_hardness(uint8_t hardness);
void set_wall_hardness(uint8_t hardness);
void set_wall_roughness(uint8_t roughness);
// Real WLED's setMatrixSize(): recomputes max_x/max_y (physics bounds, in
// sub-pixel units) from a pixel size. Kept for call-site parity with real
// WLED's own updateSystem()/constructor, but unlike real WLED, the
// rendering pixel bounds (render()'s kMaxXPixel/kMaxYPixel, always
// GuDisplay::WIDTH-1/HEIGHT-1) do NOT follow this call - this board's
// framebuffer is always exactly 32x32. Calling this with anything other
// than (32, 32) shrinks/grows where particles are allowed to move without
// changing where they can actually be drawn, so don't.
void set_matrix_size(uint32_t x, uint32_t y);
void set_wrap_x(bool enable);
void set_wrap_y(bool enable);
void set_bounce_x(bool enable);
void set_bounce_y(bool enable);
void set_kill_out_of_bounds(bool enable);
// Real WLED declares this but never defines or calls it anywhere in its
// own source (verified against wled00/FXparticleSystem.cpp and FX.cpp) -
// dead API surface upstream. Implemented here with the only sensible
// meaning given there's no dedicated "global saturation" field to set:
// stamps `sat` onto every used particle directly, same as source.sat
// seeding new ones.
void set_saturation(uint8_t sat);
void set_color_by_age(bool enable);
// Motion blur only has an effect when particle_size (below) is 0.
void set_motion_blur(uint8_t bluramount);
void set_smear_blur(uint8_t bluramount);  // 2D smeared blur of the whole rendered frame
void set_particle_size(uint8_t size);     // 0 == 1px, 1 == 2px (default), 255 == ~10px; disables per_particle_size
void set_gravity(int8_t force = 8);
void enable_particle_collisions(bool enable, uint8_t hardness = 255);

// ---- direct state access (real WLED's public data members) ----

extern Particle *particles;                  // [0, kMaxParticles) - always valid after begin()
extern ParticleFlags *particle_flags;        // [0, kMaxParticles)
extern Source *sources;                      // [0, kMaxSources)
extern AdvancedParticle *adv_particles;      // [0, kMaxParticles) if begin(true, ...); else null
extern SizeControl *adv_size;                // [0, kMaxParticles) if begin(_, true); else null
extern int32_t max_x, max_y;                 // matrix bounds, sub-pixel units (32*kPRadius - 1 by default)
extern uint32_t used_particles;              // number of particles in [0, kMaxParticles) actually simulated
extern uint32_t num_sources;                 // number of sources in [0, kMaxSources) available (always kMaxSources)
extern bool per_particle_size;               // if true, particle size comes from adv_particles[i] instead of the global set_particle_size()

}  // namespace ps2d
}  // namespace effects
