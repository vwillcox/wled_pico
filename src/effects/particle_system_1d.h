#pragma once

#include <cstddef>
#include <cstdint>

#include "effects.h"

// Port of real WLED's ParticleSystem1D (wled00/FXparticleSystem.h/.cpp,
// https://github.com/wled/WLED, by Damian Schneider "DedeHai") onto this
// board's fixed 32-wide "compute one column, broadcast it down every row
// via fill_column()" 1D convention (see effects.h's top comment - every
// de facto 1D effect in this codebase already works this way since there
// is no separate 1D strip, only one row of the 32x32 matrix used as one).
//
// ParticleSystem2D is being ported separately into its own
// particle_system_2d.h/.cpp - nothing here references it, and every type
// below lives in its own `ps1d` sub-namespace (Particle, ParticleFlags,
// Source, AdvancedParticle - WLED's PSparticle1D/PSparticleFlags1D/
// PSsource1D/PSadvancedParticle1D with the "PS"/"1D" dropped per this
// codebase's naming convention) precisely so it cannot collide with the
// 2D port's identically-named types.
//
// Two deliberate adaptations from real WLED, applied throughout this port:
//
//  1. No dynamic memory. Real WLED slices SEGENV.data at runtime, sized by
//     calculateNumberOfParticles1D()/calculateNumberOfSources1D() off
//     segment length and per-MCU tiers (MAXPARTICLES_1D/MAXSOURCES_1D:
//     320/16 on ESP8266 up to 2600/64 elsewhere). None of that applies to
//     an RP2040 driving a fixed 32-pixel row out of plain static RAM, so
//     this is a singleton: exactly one particle system's state, held in
//     the fixed-size static arrays below. There is no constructor/
//     destructor, no updatePSpointers(), no allocateParticleSystemMemory1D()
//     - begin() (re)initializes the singleton in place instead.
//
//     kMaxParticles/kMaxSources: WLED's smallest (ESP8266) tier is 320
//     particles / 16 sources, sized for segments running to several
//     hundred pixels. Scaled down for a fixed 32-pixel strip:
//     kMaxParticles=128 (4 particles/pixel of headroom - enough for dense
//     bursts like fireworks/sparkler without wasting this board's RAM,
//     roughly the same "particles per pixel of typical segment" ratio
//     ESP8266's 320-particles-per-few-hundred-pixel budget implies) and
//     kMaxSources=8 (half of ESP8266's 16 - no 1D effect in WLED's own
//     set needs more than a handful of simultaneous emitters).
//
//  2. No white channel. WLED renders particles through CRGBW; this board's
//     LEDs are plain RGB, so effects::Rgb (no W) replaces CRGBW everywhere,
//     matching every other port in this codebase (e.g. gen_batch1.cpp's
//     Aurora, which documents the same drop).
//
// Everything else (sub-pixel positions/movement, gravity, friction, force
// application, wall bounce/wrap/kill, particle-particle collisions with
// hardness, anti-aliased 2-nearest-pixel (or wider, for large particles)
// rendering, motion blur / smeared blur) is a faithful port - WLED's own
// method names become free functions here (e.g. `PartSys->setGravity(8)`
// -> `effects::ps1d::set_gravity(8)`) so porting an actual 1D particle
// effect on top of this is close to a 1:1 transliteration of its WLED
// source.
namespace effects {
namespace ps1d {

// -- constants (WLED's PS_P_*_1D #defines) -------------------------------
inline constexpr int32_t kMaxSpeed = 120;            // PS_P_MAXSPEED
inline constexpr int32_t kRadius = 32;               // PS_P_RADIUS_1D: sub-pixel resolution (must be a power of 2)
inline constexpr int32_t kHalfRadius = kRadius >> 1;  // PS_P_HALFRADIUS_1D
inline constexpr int32_t kRadiusShift = 5;           // PS_P_RADIUS_SHIFT_1D (1 << kRadiusShift == kRadius)
inline constexpr int32_t kSurfaceShift = 5;          // PS_P_SURFACE_1D (2^kSurfaceShift == kRadius)
inline constexpr int32_t kMinHardRadius = 32;        // PS_P_MINHARDRADIUS_1D
inline constexpr int32_t kMinSurfaceHardness = 120;  // PS_P_MINSURFACEHARDNESS_1D

inline constexpr uint32_t kStripLength = 32;  // fixed: this board's row length (GuDisplay::WIDTH)
inline constexpr uint32_t kMaxParticles = 128;
inline constexpr uint32_t kMaxSources = 8;

// -- public struct types --------------------------------------------------

// WLED's PSparticle1D.
struct Particle {
  int32_t x = 0;      // sub-pixel position, 0..(kStripLength*kRadius-1) when in bounds
  uint16_t ttl = 0;   // time to live, in frames; 0 = dead
  int8_t vx = 0;      // velocity, sub-pixels/frame
  uint8_t hue = 0;    // palette lookup index (WLED calls this "hue"; it indexes a palette, not raw HSV hue)
};

// WLED's PSparticleFlags1D - a 1-byte bitfield union there (packed for RAM
// headroom across hundreds of particles). Plain bools here: kMaxParticles
// is only 128, so the packing isn't worth the fragility.
struct ParticleFlags {
  bool outofbounds = false;     // set when the particle is outside the strip
  bool collide = false;         // if set, particle takes part in collisions
  bool perpetual = false;       // if set, ttl is not decremented (still dies from kill-out-of-bounds)
  bool reversegrav = false;     // if set, gravity is reversed for this particle
  bool forcedirection = false;  // WLED: vestigial/unused even there, kept for 1:1 field parity
  bool fixed = false;           // if set, particle does not move (collisions push others off it instead)
  bool custom1 = false;         // free for an effect to track its own per-particle state
  bool custom2 = false;
};

// WLED's PSadvancedParticle1D. Only meaningful once begin(true) has run -
// see advanced_active() below (mirrors WLED's `advPartProps != nullptr`).
struct AdvancedParticle {
  uint8_t sat = 255;          // color saturation
  uint8_t size = 0;           // particle diameter override (see set_particle_size())
  uint8_t forcecounter = 0;   // per-particle counter for apply_force()'s sub-integer force accumulation
};

// WLED's PSsource1D - a particle used as a template for spray_emit().
struct Source {
  uint16_t min_life = 0;  // minimum ttl of emitted particles
  uint16_t max_life = 0;  // maximum ttl of emitted particles
  Particle source;        // emitter position/speed/color
  ParticleFlags source_flags;
  int8_t var = 0;   // +/-var random speed variation added on emit
  int8_t v = 0;      // emitting speed
  uint8_t sat = 255;  // advanced-property saturation carried to emitted particles
  uint8_t size = 0;   // advanced-property size carried to emitted particles
};

// WLED's PSsettings1D. Public because it's a parameter type of
// particle_move_update() below (WLED's particleMoveUpdate() takes an
// optional PSsettings1D* override too) - the singleton's own settings_
// (set via the set_*/enable_particle_collisions functions below) stay
// internal, matching WLED's private `particlesettings` member.
struct Settings {
  bool wrap = false;
  bool bounce = false;
  bool killoutofbounds = false;
  bool useGravity = false;
  bool useCollisions = false;
  bool colorByAge = false;
  bool colorByPosition = false;
};

// -- lifecycle -------------------------------------------------------------

// (Re)initializes the singleton for a freshly-selected effect - call once
// from an effect's `state.call == 0` check (see effects.h's State comment
// and this convention's other users, e.g. gen_batch4.cpp's mode_meteor).
// Resets every particle/flag to dead (ttl=0), resets settings to WLED's
// constructor defaults (wrap/bounce/collisions/gravity all off, wall
// hardness 255, particle size 0/one-pixel, no motion/smear blur, all
// particles used), and marks the advanced-property array active (all
// saturations reset to 255) only if `advanced` is true - mirrors WLED's
// advPartProps being null unless the effect requested an advanced system.
void begin(bool advanced = false);

// Bridges an effect's Params (WLED reads SEGCOLOR/SEGPALETTE directly off
// SEGMENT since it has one; this port doesn't) into the singleton for
// render()'s palette lookups and background-color add. Cheap - call once
// per frame before update()/render(), typically straight from the
// mode_ function's own `p.palette_id/p.primary/p.secondary/p.tertiary`.
void set_palette_colors(uint8_t palette_id, Rgb primary, Rgb secondary, Rgb tertiary);

// Real WLED's ParticleSystem1D::update(): applies gravity/collisions per
// current settings, moves every used particle, and updates colorByPosition
// hues. Real WLED's update() ends by calling its own (SEGMENT-backed)
// render() too; that step is split out below as a separate render(Frame)
// call instead, since a Frame to draw into only exists in the caller's
// mode_ function - call update() then render(frame) every frame.
void update();

// Real WLED's ParticleSystem1D::render(): resolves current particle state
// into one color per column (0..kStripLength-1) and broadcasts each down
// every row via fill_column() - see this file's top comment and
// effects.h's "Where this strip lives" convention every de facto 1D
// effect in this codebase already follows.
void render(Frame frame);

// -- emitters ---------------------------------------------------------

// WLED's sprayEmit(): finds a dead particle and (re)spawns it from
// `emitter`'s source/variation settings. Returns the emitted particle's
// index, or -1 if none were free.
int32_t spray_emit(const Source &emitter);

// -- physics ------------------------------------------------------------

// WLED's particleMoveUpdate(): ages, moves, bounces/wraps/kills one
// particle for one frame. `options` defaults to the singleton's own
// settings; pass an explicit Settings to move a particle under different
// rules (WLED uses this for e.g. a source that should always bounce
// regardless of the system's wrap/bounce setting).
void particle_move_update(Particle &part, ParticleFlags &part_flags, const Settings *options = nullptr,
                           AdvancedParticle *advanced_properties = nullptr);

// WLED's applyForce(part, xforce, counter): apply a sub-integer force to
// one particle. `counter` must be a value that persists between calls for
// this particle (e.g. an AdvancedParticle::forcecounter field) - force is
// in WLED's 3.4 fixed-point notation (force=16 means +1 velocity/frame).
void apply_force(Particle &part, int8_t xforce, uint8_t &counter);
// WLED's applyForce(xforce): applies to every used particle.
void apply_force(int8_t xforce);

// WLED's applyGravity(part, partFlags): single-particle gravity, for
// particles not in the particles[] array the system-wide gravity pass
// iterates (WLED's own comment: "use this for sources"). Requires
// set_gravity() to have set a nonzero force first (does not touch the
// gravity force counter's frame-to-frame carry the way the whole-system
// pass does).
void apply_gravity(Particle &part, ParticleFlags &part_flags);

// WLED's applyFriction(coefficient): applies to every used particle.
void apply_friction(int32_t coefficient);

// -- settings (WLED's set*/enableParticleCollisions) ---------------------

void set_used_particles(uint8_t percentage);  // 255 = 100% of kMaxParticles
void set_wall_hardness(uint8_t hardness);
void set_wrap(bool enable);
void set_bounce(bool enable);
void set_kill_out_of_bounds(bool enable);
void set_color_by_age(bool enable);
void set_color_by_position(bool enable);
void set_motion_blur(uint8_t blur_amount);  // note: only useful if particle size is 0 (see WLED's own caveat)
void set_smear_blur(uint8_t blur_amount);
void set_particle_size(uint8_t size);  // 0 = 1px, 1 = 2px, 255 = ~10px; disables per-particle size control
void set_gravity(int8_t force = 8);
void enable_particle_collisions(bool enable, uint8_t hardness = 255);

// True once begin(true) has (re)activated the advanced-property array for
// the current selection - mirrors WLED's `advPartProps != nullptr` checks.
// adv_particles[] below is always physically present (no heap to null
// out), so this flag is the equivalent gate.
bool advanced_active();

// -- direct state access (WLED exposes these as public members too) ------

extern Particle particles[kMaxParticles];
extern ParticleFlags particle_flags[kMaxParticles];
extern Source sources[kMaxSources];
extern AdvancedParticle adv_particles[kMaxParticles];  // see advanced_active()

extern uint32_t used_particles;  // WLED's usedParticles
extern uint32_t num_sources;     // WLED's numSources - defaults to kMaxSources in begin(), lower to use fewer
extern int32_t max_x;            // WLED's maxX: kStripLength*kRadius - 1, this strip's fixed sub-pixel bound

}  // namespace ps1d
}  // namespace effects
