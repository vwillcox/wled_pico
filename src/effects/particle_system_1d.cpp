#include "particle_system_1d.h"

#include <algorithm>
#include <cstdlib>

#include "wled_compat.h"
#include "palettes.h"

// Ported from real WLED's wled00/FXparticleSystem.h/.cpp (ParticleSystem1D
// only - see particle_system_1d.h's top comment for the two mandatory
// adaptations this port makes (no dynamic memory, no white channel) and
// the reasoning behind kMaxParticles/kMaxSources). Line/behavior
// references below point at the real class's method of the same name
// (converted from PascalCase to this codebase's snake_case free-function
// convention) unless noted otherwise.
//
// Simplifications beyond the two mandatory adaptations (each noted again
// at its call site):
//  - gamma8inv() calls (WLED pre-compensates its own brightness split so
//    the *sum* of two adjacent anti-aliased pixels stays linear after its
//    own later gamma pass) are dropped, like every other gamma8inv() call
//    ported into this codebase - gu_display.h already applies its own
//    gamma curve downstream, so there's nothing upstream left to
//    pre-compensate (see gen_batch1.cpp's Aurora header comment for the
//    same call).
//  - effects::color_from_palette() has no LINEARBLEND vs LINEARBLEND_NOWRAP
//    distinction (WLED switches to NOWRAP for colorByAge/colorByPosition
//    so a particle's hue doesn't wrap from the palette's index 255 back to
//    0 as it ages/moves) - this port always uses the wrapping lookup, a
//    minor visual difference only at the palette's exact seam and only in
//    those two color modes.
//  - advanced-particle saturation (PSadvancedParticle1D::sat < 255) is
//    applied by desaturate() below via a luma-blend approximation rather
//    than WLED's real RGB->HSV->RGB round trip (hsv2rgb_spectrum/CHSV32) -
//    this codebase's wled_compat.h has no HSV conversion helper to build
//    that on, and a luma blend produces the same "less colorful" effect
//    for the uses this feeds (desaturating an already palette-resolved
//    color), just not bit-exact against FastLED's spectrum-based HSV math.
//  - applyFriction()'s WLED_HAVE_FAST_int_DIVIDE branch (division-based,
//    used on ESP32/S2/S3 which have a hardware divider) is dropped; only
//    the bitshift branch is ported, since that's the one real WLED itself
//    uses for MCUs without a fast divider (ESP8266) - true of the RP2040's
//    Cortex-M0+ too.
//  - real WLED's private void bounce() (declared in ParticleSystem1D's
//    header) has no definition anywhere in FXparticleSystem.cpp - confirmed
//    by inspecting the fetched source - the 1D class's wall-bounce logic
//    is inlined directly into particleMoveUpdate() instead (2D's class
//    does define and use its own bounce()). Not ported here either, for
//    the same reason: there is nothing to port. particle_move_update()
//    below inlines the same bounce logic 1D's particleMoveUpdate() does.
//  - update() and render(Frame) are two separate calls here where WLED's
//    update() ends by calling its own render() - unavoidable, since a
//    Frame to draw into only exists in the caller's mode_ function, not
//    inside this module. Call both once per frame.
namespace effects {
namespace ps1d {

// -- public singleton state (WLED's public class members) -----------------

Particle particles[kMaxParticles];
ParticleFlags particle_flags[kMaxParticles];
Source sources[kMaxSources];
AdvancedParticle adv_particles[kMaxParticles];

uint32_t used_particles = kMaxParticles;
uint32_t num_sources = kMaxSources;
int32_t max_x = static_cast<int32_t>(kStripLength * static_cast<uint32_t>(kRadius)) - 1;

namespace {

// -- internal singleton state (WLED's private class members) --------------

Settings settings_;
int32_t gforce_ = 0;                          // WLED's gforce
uint8_t gforce_counter_ = 0;                  // WLED's gforcecounter
uint8_t force_counter_ = 0;                   // WLED's forcecounter
uint32_t wall_hardness_ = 255;                // WLED's wallHardness
int32_t collision_hardness_ = 255;            // WLED's collisionHardness
int32_t particle_hard_radius_ = kMinHardRadius >> 1;  // WLED's particleHardRadius
uint8_t particle_size_ = 0;                   // WLED's particlesize
uint8_t motion_blur_ = 0;                     // WLED's motionBlur
uint8_t smear_blur_ = 0;                      // WLED's smearBlur
uint32_t emit_index_ = 0;                     // WLED's emitIndex
uint32_t collision_start_idx_ = 0;            // WLED's collisionStartIdx
bool per_particle_size_ = false;              // WLED's perParticleSize
bool advanced_enabled_ = false;               // stands in for WLED's `advPartProps != nullptr`

// update()'s stand-in for WLED's SEGMENT.call - collideParticles() reads
// this to apply extra "stickiness" friction on soft collisions every 8th
// frame. This module has no State reference of its own, so it keeps its
// own frame counter rather than taking one as a parameter on every call.
uint32_t frame_counter_ = 0;

// Fed by set_palette_colors() - see that function's header comment for why
// this exists (WLED reads SEGPALETTE/SEGCOLOR directly off SEGMENT here).
uint8_t palette_id_ = 0;
Rgb primary_{255, 0, 0};
Rgb secondary_{0, 0, 0};
Rgb tertiary_{0, 0, 0};

// WLED's local/segment framebuffer this system accumulates particle
// brightness into before the final color write - see render() below.
// Persists across frames (not cleared per-call) because motion_blur_
// needs last frame's content to decay rather than start from black.
Rgb accum_buf[kStripLength];

// Scratch space for handle_collisions()'s position-binning pass - sized
// kMaxParticles rather than WLED's VLA (`uint16_t binIndices[maxBinParticles]`)
// since max_bin_particles computed below is always <= used_particles <=
// kMaxParticles.
uint16_t bin_indices[kMaxParticles];

// -- small math helpers -----------------------------------------------

int32_t limit_speed(int32_t speed) {
  return speed > kMaxSpeed ? kMaxSpeed : (speed < -kMaxSpeed ? -kMaxSpeed : speed);
}

// WLED's calcForce_dv(): force is in 3.4 fixed-point notation (force=16
// means +1 velocity/frame); `counter` must persist between calls for
// whatever this force is being applied to.
int32_t calc_force_dv(int8_t force, uint8_t &counter) {
  if (force == 0) return 0;
  int32_t force_abs = std::abs(static_cast<int32_t>(force));
  int32_t dv = 0;
  if (force_abs < 16) {
    counter = static_cast<uint8_t>(counter + force_abs);
    if (counter > 15) {
      counter -= 16;
      dv = force < 0 ? -1 : 1;
    }
  } else {
    dv = force / 16;
  }
  return dv;
}

// WLED's checkBoundsAndWrap(): wraps `position` into [0, max_pos] if
// `wrap`, else reports whether it's still within `particle_radius` of
// that range. Returns false ("out of bounds") only once fully departed.
bool check_bounds_and_wrap(int32_t &position, int32_t max_pos, int32_t particle_radius, bool wrap) {
  if (static_cast<uint32_t>(position) > static_cast<uint32_t>(max_pos)) {
    if (wrap) {
      position = position % (max_pos + 1);
      if (position < 0) position += max_pos + 1;
    } else if ((position < -particle_radius) || (position > max_pos + particle_radius)) {
      return false;
    }
  }
  return true;
}

// WLED's fast_color_scale(): color*scale/256 per channel.
Rgb scale_color(const Rgb &c, uint8_t scale) {
  return Rgb{
      static_cast<uint8_t>((static_cast<uint32_t>(c.r) * scale) >> 8),
      static_cast<uint8_t>((static_cast<uint32_t>(c.g) * scale) >> 8),
      static_cast<uint8_t>((static_cast<uint32_t>(c.b) * scale) >> 8),
  };
}

// WLED's fast_color_scaleAdd(): adds `scale`/255 of `add` onto `base`,
// rescaling all three channels together (not clamping independently) on
// overflow so an over-bright sum keeps its hue instead of blowing out
// individual channels - this is what makes overlapping particles look
// like a single brighter blob instead of clipping to a washed-out color.
// This is *the* accumulation the task calls out as needing a faithful
// port; the algorithm is identical to WLED's, just written against this
// codebase's unpacked {r,g,b} Rgb instead of WLED's packed-uint32 bit
// tricks (which existed there purely for speed on a packed CRGBW word -
// nothing here depends on that packing).
Rgb scale_add(const Rgb &base, const Rgb &add, uint8_t scale) {
  uint32_t r = base.r + ((static_cast<uint32_t>(add.r) * scale) >> 8);
  uint32_t g = base.g + ((static_cast<uint32_t>(add.g) * scale) >> 8);
  uint32_t b = base.b + ((static_cast<uint32_t>(add.b) * scale) >> 8);
  uint32_t m = std::max(r, std::max(g, b));
  if (m > 255) {
    r = (r * 255) / m;
    g = (g * 255) / m;
    b = (b * 255) / m;
  }
  return Rgb{static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b)};
}

// Approximates WLED's per-particle saturation control (real WLED converts
// the palette-resolved RGB to HSV, overwrites S, converts back) - see this
// file's top comment for why a luma-blend stands in here instead.
Rgb desaturate(const Rgb &c, uint8_t sat) {
  uint8_t lum = static_cast<uint8_t>((77u * c.r + 150u * c.g + 29u * c.b) >> 8);
  return blend(Rgb{lum, lum, lum}, c, sat);
}

// wled00/FX_fcn.cpp's Segment::blur(amount, /*smear=*/true) - exactly what
// ParticleSystem1D::render() calls for its smearBlur pass (always with
// smear=true, so `keep` there is always 255 - no decay of the pixel
// itself, only an additive `seep = amount>>1` bleed into each neighbor).
// Ported directly from that function (fast_color_scale -> scale_color,
// color_add(..., preserveCR=false) -> per-channel qadd8, exactly as WLED's
// own default for color_add's preserveCR argument).
void apply_smear(Rgb (&buf)[kStripLength], uint8_t amount) {
  constexpr uint8_t keep = 255;  // WLED forces keep=255 here (always calls blur(amount, /*smear=*/true))
  const uint8_t seep = static_cast<uint8_t>(amount >> 1);
  Rgb cur = buf[0];
  Rgb carryover = scale_color(cur, seep);
  buf[0] = scale_color(cur, keep);  // not a no-op: fast_color_scale(x,255) rounds down slightly, same as WLED's own
  for (uint32_t i = 1; i < kStripLength; i++) {
    cur = buf[i];
    Rgb part = scale_color(cur, seep);
    cur = scale_color(cur, keep);
    buf[i - 1] = Rgb{
        qadd8(buf[i - 1].r, part.r),
        qadd8(buf[i - 1].g, part.g),
        qadd8(buf[i - 1].b, part.b),
    };
    buf[i] = Rgb{
        qadd8(cur.r, carryover.r),
        qadd8(cur.g, carryover.g),
        qadd8(cur.b, carryover.b),
    };
    carryover = part;
  }
}

// -- rendering (WLED's private renderParticle/renderLargeParticle) --------

// WLED's renderParticle(): splits one particle's brightness across its 1
// (size==0), 2 (standard), or more (large) nearest pixels based on its
// sub-pixel position, accumulating into `buf` via scale_add() above.
void render_particle(Rgb (&buf)[kStripLength], uint32_t idx, uint8_t brightness, const Rgb &color, bool wrap);
void render_large_particle(Rgb (&buf)[kStripLength], uint32_t size, uint32_t idx, uint8_t brightness,
                            const Rgb &color, bool wrap);

void render_particle(Rgb (&buf)[kStripLength], uint32_t idx, uint8_t brightness, const Rgb &color, bool wrap) {
  uint32_t size = particle_size_;
  if (per_particle_size_ && advanced_enabled_) size = 1u + adv_particles[idx].size;  // +1: collisions don't support single-pixel size

  if (size == 0) {  // single pixel particle - can be out of bounds, oob checking is done for 2-pixel particles
    int32_t xi = particles[idx].x >> kRadiusShift;
    if (xi >= 0 && static_cast<uint32_t>(xi) < kStripLength) {
      uint32_t x = static_cast<uint32_t>(xi);
      buf[x] = scale_add(buf[x], color, brightness);
    }
    return;
  }
  if (size > 1) {  // size > 1: render as gradient line
    render_large_particle(buf, size, idx, brightness, color, wrap);
    return;
  }

  // standard rendering (2 pixels per particle)
  bool in_frame[2] = {true, true};
  int32_t px_brightness[2];
  int32_t pix[2];  // physical pixel coordinates of the two pixels representing a particle

  int32_t xoffset = particles[idx].x + kHalfRadius;
  int32_t dx = xoffset & (kRadius - 1);  // relative particle position in subpixel space
  int32_t x = xoffset >> kRadiusShift;

  pix[1] = x;  // right pixel
  x--;
  pix[0] = x;  // left pixel

  px_brightness[0] = ((kRadius - dx) * static_cast<int32_t>(brightness)) >> kSurfaceShift;
  px_brightness[1] = (dx * static_cast<int32_t>(brightness)) >> kSurfaceShift;

  const int32_t last_pixel = static_cast<int32_t>(kStripLength) - 1;
  if (pix[0] < 0) {  // left pixel out of frame
    if (wrap) {
      pix[0] = last_pixel;
    } else {
      in_frame[0] = false;
      if (pix[0] < -1) return;  // both pixels out of frame
    }
  } else if (pix[1] > last_pixel) {  // right pixel, only checked if left pixel didn't underflow
    if (wrap) {
      pix[1] = 0;
    } else {
      in_frame[1] = false;
      if (pix[0] > last_pixel) return;  // both pixels out of frame
    }
  }
  for (int i = 0; i < 2; i++) {
    if (in_frame[i]) {
      uint32_t p = static_cast<uint32_t>(pix[i]);
      buf[p] = scale_add(buf[p], color, static_cast<uint8_t>(px_brightness[i]));
    }
  }
}

// WLED's renderLargeParticle(): renders a particle as a line with linear
// brightness falloff and sub-pixel precision; `size` is 2-256 (1-9 pixel
// radius). Note: WLED's own version computes an `x_offset` local here that
// it never uses (confirmed - not referenced anywhere else in the
// function); dropped rather than ported as dead code.
void render_large_particle(Rgb (&buf)[kStripLength], uint32_t size, uint32_t idx, uint8_t brightness,
                            const Rgb &color, bool wrap) {
  int32_t x_subcenter = particles[idx].x;                 // particle position in sub-pixel space
  int32_t x_center = x_subcenter >> kRadiusShift;          // integer pixel position, rounded down

  int32_t r_subpixel = static_cast<int32_t>(size) + kRadius + 1;  // size=255 -> radius of 9 pixels
  int32_t r_pixels = r_subpixel >> kRadiusShift;

  int32_t x_min = x_center - r_pixels - 1;  // extend by one for smoother movement
  int32_t x_max = x_center + r_pixels + 1;

  const int32_t matrix_x = static_cast<int32_t>(kStripLength);

  for (int32_t px = x_min; px <= x_max; px++) {
    int32_t render_x = px;
    if (render_x < 0) {
      if (!wrap) continue;
      render_x += matrix_x;
    } else if (render_x >= matrix_x) {
      if (!wrap) continue;
      render_x -= matrix_x;
    }
    int32_t dx = (px << kRadiusShift) - x_subcenter + kHalfRadius;  // sub-pixel distance from particle center
    int32_t dx_sq = dx * dx;
    int32_t rx_sq = r_subpixel * r_subpixel;
    uint32_t dist_sq = static_cast<uint32_t>(dx_sq << 8) / static_cast<uint32_t>(rx_sq);  // normalized (0-256)

    uint8_t pixel_brightness =
        dist_sq >= 256 ? 0 : static_cast<uint8_t>(((256 - dist_sq) * brightness) >> 8);

    uint32_t rx = static_cast<uint32_t>(render_x);
    buf[rx] = scale_add(buf[rx], color, pixel_brightness);
  }
}

// -- physics internals (WLED's private applyGravity()/handleCollisions()/collideParticles()) --

// WLED's private no-argument applyGravity(): applies gravity to every used
// particle using the global gforce setting (only called from update() when
// settings_.useGravity is set).
void apply_gravity_all() {
  int32_t dv_raw = calc_force_dv(static_cast<int8_t>(gforce_), gforce_counter_);
  for (uint32_t i = 0; i < used_particles; i++) {
    int32_t dv = particle_flags[i].reversegrav ? -dv_raw : dv_raw;
    particles[i].vx = static_cast<int8_t>(limit_speed(particles[i].vx - dv));
  }
}

// WLED's collideParticles(): resolves one candidate pair - pushes apart if
// merely close, applies an impulse (scaled by collision hardness, and by a
// mass ratio if per-particle sizing is active) if actually colliding.
void collide_particles(uint32_t idx1, uint32_t idx2, int32_t dx, uint32_t collisiondistance) {
  int32_t massratio1 = 0;  // 0 means don't use a mass ratio (equal mass)
  int32_t massratio2 = 0;
  if (per_particle_size_ && advanced_enabled_) {
    collisiondistance = static_cast<uint32_t>(kMinHardRadius * 2) +
                         (((static_cast<uint32_t>(adv_particles[idx1].size) + adv_particles[idx2].size) * 52) >> 6);
    uint32_t mass1 = static_cast<uint32_t>(kRadius) + adv_particles[idx1].size;
    uint32_t mass2 = static_cast<uint32_t>(kRadius) + adv_particles[idx2].size;
    uint32_t totalmass = mass1 + mass2 - 2;  // -2 to account for rounding
    massratio1 = static_cast<int32_t>((mass2 << 8) / totalmass);
    massratio2 = static_cast<int32_t>((mass1 << 8) / totalmass);
  }

  int32_t dv = static_cast<int32_t>(particles[idx2].vx) - particles[idx1].vx;
  int32_t absdv = std::abs(dv);
  int32_t dot_product = dx * dv;  // negative if moving towards each other
  uint32_t dx_abs = static_cast<uint32_t>(std::abs(dx));

  if (dot_product < 0) {
    uint32_t lookahead = collisiondistance + static_cast<uint32_t>(absdv);
    if (dx_abs <= lookahead) {
      if (particle_flags[idx1].fixed) {
        particles[idx2].vx = static_cast<int8_t>(-(particles[idx2].vx * collision_hardness_) / 255);
        particles[idx2].x =
            particles[idx1].x + (dx < 0 ? -static_cast<int32_t>(collisiondistance) : static_cast<int32_t>(collisiondistance));
        return;
      }
      if (particle_flags[idx2].fixed) {
        particles[idx1].vx = static_cast<int8_t>(-(particles[idx1].vx * collision_hardness_) / 255);
        particles[idx1].x =
            particles[idx2].x + (dx < 0 ? static_cast<int32_t>(collisiondistance) : -static_cast<int32_t>(collisiondistance));
        return;
      }
      int32_t surfacehardness = std::max(collision_hardness_, kMinSurfaceHardness);
      int32_t impulse = (dv * surfacehardness + ((dv >> 31) & 0xFF)) >> 8;

      if (massratio1) {
        int32_t vx1 = particles[idx1].vx + ((impulse * massratio1) >> 7);
        int32_t vx2 = particles[idx2].vx - ((impulse * massratio2) >> 7);
        particles[idx1].vx = static_cast<int8_t>(limit_speed(vx1));
        particles[idx2].vx = static_cast<int8_t>(limit_speed(vx2));
      } else {
        particles[idx1].vx = static_cast<int8_t>(particles[idx1].vx + impulse);
        particles[idx2].vx = static_cast<int8_t>(particles[idx2].vx - impulse);
      }

      if (collision_hardness_ < kMinSurfaceHardness && (frame_counter_ & 0x07) == 0) {  // soft particles get sticky
        int32_t coeff = collision_hardness_ + (250 - kMinSurfaceHardness);
        int32_t v1 = particles[idx1].vx;
        int32_t v2 = particles[idx2].vx;
        particles[idx1].vx = static_cast<int8_t>((v1 * coeff + ((v1 >> 31) & 0xFF)) >> 8);
        particles[idx2].vx = static_cast<int8_t>((v2 * coeff + ((v2 >> 31) & 0xFF)) >> 8);
      }
    } else {
      return;  // not close enough yet
    }
  }

  // particles have volume: push apart if too close (adds a little speed too
  // at low relative speed, otherwise soft piles collapse instead of stacking)
  if (dx_abs < collisiondistance) {
    int32_t pushamount = 1 + static_cast<int32_t>((collisiondistance - dx_abs) >> 3);
    int32_t addspeed = 1;
    if (dx < 0) {
      pushamount = -pushamount;
      addspeed = -addspeed;
    }
    if (absdv < 4) {
      particles[idx1].vx = static_cast<int8_t>(particles[idx1].vx - addspeed);
      particles[idx2].vx = static_cast<int8_t>(particles[idx2].vx + addspeed);
    }
    bool fairlyrandom = (dot_product & 0x01) != 0;
    if (fairlyrandom) {
      particles[idx1].x -= pushamount;
    } else {
      particles[idx2].x += pushamount;
    }
  }
}

// WLED's handleCollisions(): bins particles by position (to keep collision
// checks roughly O(n) instead of O(n^2)), then checks every pair within
// each bin. On this board max_x is always 32*kRadius-1 = 1023, far below
// bin_width (64*kRadius = 2048), so num_bins below always resolves to 1 in
// practice - a direct consequence of the fixed 32-pixel strip length, not
// a shortcut: the general (WLED-identical) binning logic is still ported
// faithfully in case that ever changes.
void handle_collisions() {
  uint32_t collisiondistance = static_cast<uint32_t>(particle_hard_radius_) << 1;
  uint32_t check_dist = std::max<uint32_t>(2u * kMaxSpeed, collisiondistance);
  if (per_particle_size_ && advanced_enabled_) check_dist = std::max<uint32_t>(2u * kMaxSpeed, (512u * 52u) >> 6);
  uint32_t check_dist_sq = check_dist * check_dist;

  int32_t bin_width = 64 * kRadius;
  int32_t overlap = static_cast<int32_t>(collisiondistance) + (2 * kMaxSpeed);
  if (per_particle_size_ && advanced_enabled_) overlap = 512;  // 2 * max radius

  uint32_t max_bin_particles = std::max<uint32_t>(50u, (used_particles + 1) / 4);
  uint32_t num_bins = static_cast<uint32_t>((max_x + (bin_width - 1)) / bin_width);
  if (used_particles < max_bin_particles) {
    num_bins = 1;
    bin_width = max_x + 1;
  }

  uint32_t bin_particle_count = 0;
  uint32_t next_frame_start_idx = random16(static_cast<uint16_t>(used_particles));
  uint32_t pidx = collision_start_idx_;

  for (uint32_t bin = 0; bin < num_bins; bin++) {
    bin_particle_count = 0;
    int32_t bin_start = static_cast<int32_t>(bin) * bin_width - overlap;
    int32_t bin_end = bin_start + bin_width + (overlap << 1);

    for (uint32_t i = 0; i < used_particles; i++) {
      if (particles[pidx].ttl > 0) {
        if (particles[pidx].x >= bin_start && particles[pidx].x <= bin_end) {
          if (!particle_flags[pidx].outofbounds && particle_flags[pidx].collide) {
            if (bin_particle_count >= max_bin_particles) {
              next_frame_start_idx = pidx;  // more particles than fit this bin, finish next frame
              break;
            }
            bin_indices[bin_particle_count++] = static_cast<uint16_t>(pidx);
          }
        }
      }
      pidx++;
      if (pidx >= used_particles) pidx = 0;
    }

    for (uint32_t i = 0; i < bin_particle_count; i++) {
      uint32_t idx_i = bin_indices[i];
      for (uint32_t j = i + 1; j < bin_particle_count; j++) {
        uint32_t idx_j = bin_indices[j];
        int32_t dx = particles[idx_j].x - particles[idx_i].x;
        uint32_t dx_sq = static_cast<uint32_t>(dx * dx);
        if (dx_sq <= check_dist_sq) collide_particles(idx_i, idx_j, dx, collisiondistance);
      }
    }
  }
  collision_start_idx_ = next_frame_start_idx;
}

}  // namespace

// -- lifecycle --------------------------------------------------------

void begin(bool advanced) {
  for (auto &p : particles) p = Particle{};
  for (auto &f : particle_flags) f = ParticleFlags{};
  for (auto &s : sources) {
    s = Source{};
    s.source.ttl = 1;  // WLED: "set source alive"
  }
  for (auto &a : adv_particles) a = AdvancedParticle{};  // default member init already sets sat=255
  for (auto &c : accum_buf) c = Rgb{0, 0, 0};

  used_particles = kMaxParticles;
  num_sources = kMaxSources;
  max_x = static_cast<int32_t>(kStripLength * static_cast<uint32_t>(kRadius)) - 1;

  settings_ = Settings{};
  wall_hardness_ = 255;
  collision_hardness_ = 255;
  set_gravity(0);          // gravity disabled by default
  set_particle_size(0);    // 1-pixel size by default
  motion_blur_ = 0;
  smear_blur_ = 0;
  emit_index_ = 0;
  collision_start_idx_ = 0;
  force_counter_ = 0;
  gforce_counter_ = 0;
  frame_counter_ = 0;

  per_particle_size_ = advanced;  // enabled by default for advanced systems so FX don't need to set it explicitly
  advanced_enabled_ = advanced;

  palette_id_ = 0;
  primary_ = Rgb{255, 0, 0};
  secondary_ = Rgb{0, 0, 0};
  tertiary_ = Rgb{0, 0, 0};
}

void set_palette_colors(uint8_t palette_id, Rgb primary, Rgb secondary, Rgb tertiary) {
  palette_id_ = palette_id;
  primary_ = primary;
  secondary_ = secondary;
  tertiary_ = tertiary;
}

void update() {
  frame_counter_++;

  if (settings_.useGravity) apply_gravity_all();

  if (settings_.useCollisions) {
    handle_collisions();
    if (per_particle_size_) handle_collisions();  // second pass helps small/large-particle "slip through"
  }

  for (uint32_t i = 0; i < used_particles; i++) {
    particle_move_update(particles[i], particle_flags[i], nullptr, advanced_enabled_ ? &adv_particles[i] : nullptr);
  }

  if (settings_.colorByPosition) {
    uint32_t scale = (255u << 16) / static_cast<uint32_t>(max_x);
    for (uint32_t i = 0; i < used_particles; i++) {
      particles[i].hue = static_cast<uint8_t>((scale * static_cast<uint32_t>(particles[i].x)) >> 16);
    }
  }
}

void render(Frame frame) {
  if (motion_blur_) {
    for (uint32_t x = 0; x < kStripLength; x++) accum_buf[x] = scale_color(accum_buf[x], motion_blur_);
  } else {
    for (uint32_t x = 0; x < kStripLength; x++) accum_buf[x] = Rgb{0, 0, 0};
  }

  for (uint32_t i = 0; i < used_particles; i++) {
    if (particles[i].ttl == 0 || particle_flags[i].outofbounds) continue;

    uint8_t brightness = static_cast<uint8_t>(std::min<uint32_t>(static_cast<uint32_t>(particles[i].ttl) << 1, 255));
    Rgb base_color = color_from_palette(palette_id_, particles[i].hue, primary_, secondary_, tertiary_);
    if (advanced_enabled_ && adv_particles[i].sat < 255) base_color = desaturate(base_color, adv_particles[i].sat);

    render_particle(accum_buf, i, brightness, base_color, settings_.wrap);
  }

  if (smear_blur_) apply_smear(accum_buf, smear_blur_);

  if (secondary_.r || secondary_.g || secondary_.b) {  // add background color, if not black
    for (uint32_t x = 0; x < kStripLength; x++) accum_buf[x] = scale_add(accum_buf[x], secondary_, 255);
  }

  for (uint32_t x = 0; x < kStripLength; x++) fill_column(frame, static_cast<int>(x), accum_buf[x]);
}

// -- emitters -----------------------------------------------------------

int32_t spray_emit(const Source &emitter) {
  for (uint32_t i = 0; i < used_particles; i++) {
    emit_index_++;
    if (emit_index_ >= used_particles) emit_index_ = 0;
    if (particles[emit_index_].ttl == 0) {  // found a dead particle
      particles[emit_index_].vx =
          static_cast<int8_t>(emitter.v + random16(static_cast<uint16_t>(emitter.var << 1)) - emitter.var);
      particles[emit_index_].x = emitter.source.x;
      particles[emit_index_].hue = emitter.source.hue;
      particles[emit_index_].ttl = random16(emitter.min_life, emitter.max_life);
      particle_flags[emit_index_].collide = emitter.source_flags.collide;
      particle_flags[emit_index_].reversegrav = emitter.source_flags.reversegrav;
      particle_flags[emit_index_].perpetual = emitter.source_flags.perpetual;
      if (advanced_enabled_) {
        adv_particles[emit_index_].sat = emitter.sat;
        adv_particles[emit_index_].size = emitter.size;
      }
      return static_cast<int32_t>(emit_index_);
    }
  }
  return -1;
}

// -- physics --------------------------------------------------------------

void particle_move_update(Particle &part, ParticleFlags &part_flags, const Settings *options,
                           AdvancedParticle *advanced_properties) {
  if (options == nullptr) options = &settings_;

  if (part.ttl == 0) return;

  if (!part_flags.perpetual) part.ttl--;
  if (options->colorByAge) part.hue = static_cast<uint8_t>(std::min<int>(part.ttl, 255));

  int32_t renderradius = kHalfRadius - 1 + particle_size_;  // default for 2-pixel rendering
  int32_t newX = part.x + static_cast<int32_t>(part.vx);
  part_flags.outofbounds = false;  // reset (particle may have been created outside the strip and is now moving into view)

  if (per_particle_size_ && advanced_properties != nullptr) {
    renderradius = kHalfRadius - 1 + advanced_properties->size;
    if (advanced_properties->size > 1)
      particle_hard_radius_ = kMinHardRadius + ((static_cast<int32_t>(advanced_properties->size) * 52) >> 6);
    else
      particle_hard_radius_ = kMinHardRadius >> 1;  // single pixel particles use half the collision distance
  }

  // if wall collisions are enabled, bounce before reaching the edge - looks
  // nicer than a particle half out of view. (WLED's 1D class inlines this
  // directly rather than calling a shared bounce() helper - see this
  // file's top comment.)
  if (options->bounce) {
    if ((newX < particle_hard_radius_) || (newX > (max_x - particle_hard_radius_))) {
      bool bouncethis = true;
      if (options->useGravity) {
        if (part_flags.reversegrav) {
          if (newX < particle_hard_radius_) bouncethis = false;  // skip bouncing at x = 0
        } else if (newX > particle_hard_radius_) {
          bouncethis = false;  // skip bouncing at x = max
        }
      }
      if (bouncethis) {
        part.vx = static_cast<int8_t>(-part.vx);
        part.vx = static_cast<int8_t>((static_cast<int32_t>(part.vx) * static_cast<int32_t>(wall_hardness_)) / 255);
        if (newX < particle_hard_radius_)
          newX = particle_hard_radius_;  // fast particles never reach the edge if position is inverted - looks better
        else
          newX = max_x - particle_hard_radius_;
      }
    }
  }

  if (!check_bounds_and_wrap(newX, max_x, renderradius, options->wrap)) {
    part_flags.outofbounds = true;
    if (options->killoutofbounds) {
      bool killthis = true;
      if (options->useGravity) {  // if gravity is used, only kill below "floor level"
        if (part_flags.reversegrav) {
          if (newX < 0 || newX > (max_x << 2)) killthis = false;  // skip at x = 0, not far out of bounds
        } else {
          if (newX > 0 && newX < (max_x << 2)) killthis = false;  // skip at x = max, not far out of bounds
        }
      }
      if (killthis) part.ttl = 0;
    }
  }

  if (!part_flags.fixed)
    part.x = newX;
  else
    part.vx = 0;  // particle can still gain speed via collisions; if unfixed later it shouldn't speed away
}

void apply_force(Particle &part, int8_t xforce, uint8_t &counter) {
  int32_t dv = calc_force_dv(xforce, counter);
  part.vx = static_cast<int8_t>(limit_speed(static_cast<int32_t>(part.vx) + dv));
}

void apply_force(int8_t xforce) {
  int32_t dv = calc_force_dv(xforce, force_counter_);
  for (uint32_t i = 0; i < used_particles; i++) {
    particles[i].vx = static_cast<int8_t>(limit_speed(static_cast<int32_t>(particles[i].vx) + dv));
  }
}

void apply_gravity(Particle &part, ParticleFlags &part_flags) {
  uint8_t counter_backup = gforce_counter_;
  int32_t dv = calc_force_dv(static_cast<int8_t>(gforce_), gforce_counter_);
  if (part_flags.reversegrav) dv = -dv;
  gforce_counter_ = counter_backup;  // does not consume the shared gravity counter's frame-to-frame carry
  part.vx = static_cast<int8_t>(limit_speed(static_cast<int32_t>(part.vx) - dv));
}

void apply_friction(int32_t coefficient) {
  int32_t friction = 256 - coefficient;
  for (uint32_t i = 0; i < used_particles; i++) {
    if (particles[i].ttl) {
      int32_t v = particles[i].vx;
      particles[i].vx = static_cast<int8_t>((v * friction + ((v >> 31) & 0xFF)) >> 8);
    }
  }
}

// -- settings -------------------------------------------------------------

void set_used_particles(uint8_t percentage) {
  uint32_t v = (kMaxParticles * (static_cast<uint32_t>(percentage) + 1)) >> 8;
  used_particles = std::max<uint32_t>(1, v);
}

void set_wall_hardness(uint8_t hardness) { wall_hardness_ = hardness; }
void set_wrap(bool enable) { settings_.wrap = enable; }
void set_bounce(bool enable) { settings_.bounce = enable; }
void set_kill_out_of_bounds(bool enable) { settings_.killoutofbounds = enable; }
void set_color_by_age(bool enable) { settings_.colorByAge = enable; }
void set_color_by_position(bool enable) { settings_.colorByPosition = enable; }
void set_motion_blur(uint8_t blur_amount) { motion_blur_ = blur_amount; }
void set_smear_blur(uint8_t blur_amount) { smear_blur_ = blur_amount; }

void set_particle_size(uint8_t size) {
  particle_size_ = size;
  particle_hard_radius_ = kMinHardRadius;  // ~1 pixel
  per_particle_size_ = false;              // disable per-particle size control if global size is set
  if (particle_size_ > 1) {
    particle_hard_radius_ = kMinHardRadius + ((static_cast<int32_t>(particle_size_) * 52) >> 6);
  } else if (particle_size_ == 0) {
    particle_hard_radius_ = kMinHardRadius >> 1;  // single pixel particles have half the radius
  }
}

void set_gravity(int8_t force) {
  if (force) {
    gforce_ = force;
    settings_.useGravity = true;
  } else {
    settings_.useGravity = false;
  }
}

void enable_particle_collisions(bool enable, uint8_t hardness) {
  settings_.useCollisions = enable;
  collision_hardness_ = hardness;
}

bool advanced_active() { return advanced_enabled_; }

}  // namespace ps1d
}  // namespace effects
