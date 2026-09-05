#include "particle_system_2d.h"

#include "palettes.h"
#include "wled_compat.h"

// Port of real WLED's ParticleSystem2D (wled00/FXparticleSystem.h/.cpp).
// See particle_system_2d.h's top comment for the two mandatory adaptations
// (no dynamic memory, no white channel) applied throughout without further
// comment at each site. Smaller, forced deviations from those two, or from
// this codebase's own architecture, are called out at the specific spot
// they happen and summarized here:
//
//  - update()/update_fire() no longer render internally (real WLED's do,
//    since their render() can reach the segment's pixel buffer through
//    global state). render(Frame) takes an explicit target this module has
//    no other way to get, so it's a separate public call - see the header.
//  - set_colors() has no real-WLED equivalent - a bridge for the same
//    reason (this module has no "current segment" to read a live palette/
//    colors from); an effect pushes them in before render(). See header.
//  - color_from_palette() here has no blend-mode parameter, so real WLED's
//    LINEARBLEND vs LINEARBLEND_NOWRAP distinction (used for colorByAge)
//    isn't replicated - a very minor visual difference only at the
//    palette's wrap seam when colorByAge is on.
//  - Saturation-reduced particle color (particle.sat < 255) is real WLED's
//    "convert to HSV, clamp S, convert back" (hsv2rgb_spectrum) - this
//    codebase has no HSV<->RGB conversion helper to port that bit-for-bit,
//    so it's approximated by blending toward a gray at the pixel's own
//    peak channel value (same hue/value, less saturation) - see
//    apply_saturation() below.
//  - SEGMENT.call (used by collideParticles() to only apply "sticky"
//    friction on 1-in-8 frames) has no equivalent; a private frame counter
//    incremented once per update()/update_fire() call stands in for it.
//  - set_saturation() is declared but never defined or called anywhere in
//    real WLED's own source (verified against current wled00/
//    FXparticleSystem.cpp and FX.cpp) - dead API surface upstream.
//    Implemented here the only way that makes sense given there's no
//    dedicated "global saturation" field: stamps `sat` onto every used
//    particle directly.
//  - set_used_particles()'s percentage is now relative to the fixed
//    kMaxParticles ceiling rather than real WLED's per-effect `numParticles`
//    (itself a runtime value, sized down from MAXPARTICLES_2D by available
//    heap) - a direct, unavoidable consequence of adaptation #1.
//  - num_sources is always kMaxSources (no calculateNumberOfSources2D():
//    real WLED clamps an effect's *requested* source count against a
//    memory budget; there's no budget left to clamp against here, so any
//    effect can simply use as many of the kMaxSources slots as it needs).
//  - handleCollisions()'s per-bin index scratch space was a stack VLA
//    (`uint16_t binIndices[maxBinParticles]`) in real WLED - not valid
//    ISO C++, replaced with a fixed `kMaxParticles`-sized local array
//    (always large enough, since maxBinParticles <= usedParticles <=
//    kMaxParticles).
namespace effects {
namespace ps2d {

namespace {

constexpr int32_t kMaxXPixel = GuDisplay::WIDTH - 1;
constexpr int32_t kMaxYPixel = GuDisplay::HEIGHT - 1;
constexpr int kPixelCount = GuDisplay::WIDTH * GuDisplay::HEIGHT;

// ---- backing storage for the singleton (adaptation #1: static, not
// heap-allocated; sized once at kMaxParticles/kMaxSources) ----
Particle g_particles_storage[kMaxParticles];
ParticleFlags g_particle_flags_storage[kMaxParticles];
Source g_sources_storage[kMaxSources];
AdvancedParticle g_adv_particles_storage[kMaxParticles];
SizeControl g_adv_size_storage[kMaxParticles];

// Render accumulator (real WLED's `framebuffer`/SEGMENT.getPixels()):
// packed 0x00RRGGBB per pixel (top byte always 0 - stands in for CRGBW's
// W, adaptation #2), row-major, row 0 = top of the display (see the
// coordinate-flip note in render_particle()/render_large_particle()).
uint32_t g_framebuffer[kPixelCount];

// ---- private engine state (real WLED's private class members) ----
Settings2D g_settings;
uint32_t g_emit_index = 0;
int32_t g_collision_hardness = 0;
uint32_t g_wall_hardness = 255;
uint32_t g_wall_roughness = 0;
uint32_t g_particle_hard_radius = kPMinHardRadius;
uint16_t g_collision_start_idx = 0;
uint8_t g_fire_intensity = 0;
uint8_t g_force_counter = 0;
uint8_t g_gforce_counter = 0;
int8_t g_gforce = 0;
uint8_t g_particle_size = 1;
uint8_t g_motion_blur = 0;
uint8_t g_smear_blur = 0;
// Stands in for real WLED's SEGMENT.call inside collide_particles() - see
// this file's top comment.
uint32_t g_frame_counter = 0;
// Fed by set_colors() - see this file's top comment.
uint8_t g_palette_id = 0;
Rgb g_primary{255, 0, 0};
Rgb g_secondary{0, 0, 0};
Rgb g_tertiary{0, 0, 0};

inline int32_t abs32(int32_t v) { return v < 0 ? -v : v; }

inline uint32_t pack_color(Rgb c) {
  return (static_cast<uint32_t>(c.r) << 16) | (static_cast<uint32_t>(c.g) << 8) | c.b;
}
inline Rgb unpack_color(uint32_t c) {
  return Rgb{static_cast<uint8_t>(c >> 16), static_cast<uint8_t>(c >> 8), static_cast<uint8_t>(c)};
}

// wled00/colors.h fast_color_scale(): scales all channels by scale/256.
// Verbatim from upstream - works unmodified on our 0x00RRGGBB layout since
// the top ("W") byte it also touches is always zero here.
inline uint32_t fast_color_scale(uint32_t c, uint8_t scale) {
  uint32_t rb = (((c) & 0x00FF00FFu) * scale >> 8) & 0x00FF00FFu;
  uint32_t wg = (((c >> 8) & 0x00FF00FFu) * scale) & ~0x00FF00FFu;
  return rb | wg;
}

// wled00/colors.cpp color_add(preserveCR=false): branchless per-channel
// saturating add - real WLED's blur2D() always calls it with the default
// (false), so that is the only variant ported.
inline uint32_t color_add(uint32_t c1, uint32_t c2) {
  if (c1 == 0) return c2;
  if (c2 == 0) return c1;
  constexpr uint32_t kTwoChannelMask = 0x00FF00FFu;
  uint32_t rb = (c1 & kTwoChannelMask) + (c2 & kTwoChannelMask);
  uint32_t wg = ((c1 >> 8) & kTwoChannelMask) + ((c2 >> 8) & kTwoChannelMask);
  rb |= ((rb & 0x01000100u) - ((rb >> 8) & 0x00010001u)) & 0x00FF00FFu;
  wg |= ((wg & 0x01000100u) - ((wg >> 8) & 0x00010001u)) & 0x00FF00FFu;
  wg <<= 8;
  return rb | wg;
}

// wled00/FXparticleSystem.cpp's file-scope fast_color_scaleAdd(): adds
// `scale`/255 of c2 onto c1, rescaling all channels together (preserving
// hue) rather than clipping per-channel if the sum would overflow - what
// makes overlapping particles blend/saturate together sensibly instead of
// shifting toward white. Verbatim from upstream.
uint32_t fast_color_scale_add(uint32_t c1, uint32_t c2, uint8_t scale) {
  constexpr uint32_t kMaskRb = 0x00FF00FFu;
  constexpr uint32_t kMaskG = 0x0000FF00u;
  uint32_t rb = c2 & kMaskRb;
  uint32_t g = c2 & kMaskG;
  rb = ((rb * scale) >> 8) & kMaskRb;
  g = ((g * scale) >> 8) & kMaskG;
  rb = (c1 & kMaskRb) + rb;
  g = (c1 & kMaskG) + g;
  if ((rb | (g >> 8)) & 0x01000100u) {
    g = g >> 8;
    uint32_t max_val = rb >> 16;
    max_val = ((rb & 0xFFFFu) > max_val) ? (rb & 0xFFFFu) : max_val;
    max_val = (g > max_val) ? g : max_val;
    uint32_t scale_factor = (255u << 8) / max_val;
    rb = ((rb * scale_factor) >> 8) & kMaskRb;
    g = (g * scale_factor) & kMaskG;
  }
  return rb | g;
}

// Approximates real WLED's CHSV32 saturation clamp + hsv2rgb_spectrum()
// round trip - see this file's top comment for why it's approximated
// rather than ported bit-for-bit.
Rgb apply_saturation(Rgb color, uint8_t sat) {
  uint8_t m = color.r > color.g ? color.r : color.g;
  if (color.b > m) m = color.b;
  return blend(Rgb{m, m, m}, color, sat);
}

// wled00/FXparticleSystem.cpp's file-scope calcForce_dv().
int32_t calc_force_dv(int8_t force, uint8_t &counter) {
  if (force == 0) return 0;
  int32_t force_abs = abs32(force);
  int32_t dv = 0;
  if (force_abs < 16) {
    counter = static_cast<uint8_t>(counter + force_abs);
    if (counter > 15) {
      counter = static_cast<uint8_t>(counter - 16);
      dv = force < 0 ? -1 : 1;
    }
  } else {
    dv = force / 16;
  }
  return dv;
}

// wled00/FXparticleSystem.cpp's file-scope checkBoundsAndWrap().
bool check_bounds_and_wrap(int32_t &position, int32_t max, int32_t particleradius, bool wrap) {
  if (static_cast<uint32_t>(position) > static_cast<uint32_t>(max)) {
    if (wrap) {
      position = position % (max + 1);
      if (position < 0) position += max + 1;
    } else if ((position < -particleradius) || (position > max + particleradius)) {
      return false;
    }
  }
  return true;
}

// ParticleSystem2D::bounce() (private).
void bounce(int8_t &incomingspeed, int8_t &parallelspeed, int32_t &position, uint32_t maxposition) {
  incomingspeed = static_cast<int8_t>(-incomingspeed);
  incomingspeed = static_cast<int8_t>((incomingspeed * g_wall_hardness + 128) >> 8);
  if (position < static_cast<int32_t>(g_particle_hard_radius))
    position = static_cast<int32_t>(g_particle_hard_radius);
  else
    position = static_cast<int32_t>(maxposition) - static_cast<int32_t>(g_particle_hard_radius);
  if (g_wall_roughness) {
    int32_t incomingspeed_abs = abs32(incomingspeed);
    int32_t totalspeed = incomingspeed_abs + abs32(parallelspeed);
    int32_t donatespeed = ((static_cast<int32_t>(random16(static_cast<uint16_t>(incomingspeed_abs << 1))) - incomingspeed_abs) *
                            static_cast<int32_t>(g_wall_roughness)) /
                           255;
    parallelspeed = static_cast<int8_t>(limit_speed(static_cast<int32_t>(parallelspeed) + donatespeed));
    donatespeed = totalspeed - abs32(parallelspeed);
    incomingspeed = incomingspeed > 0 ? static_cast<int8_t>(donatespeed) : static_cast<int8_t>(-donatespeed);
  }
}

// ParticleSystem2D::getParticleXYsize() (private).
void get_particle_xy_size(AdvancedParticle *advprops, SizeControl *advsize, uint32_t &xsize, uint32_t &ysize) {
  if (advsize == nullptr) return;
  int32_t size = advprops->size;
  int32_t asymdir = advsize->asymdir;
  int32_t deviation = static_cast<int32_t>((static_cast<uint32_t>(size) * advsize->asymmetry + 255) >> 8);
  if (asymdir < 64)
    deviation = (asymdir * deviation) >> 6;
  else if (asymdir < 192)
    deviation = ((128 - asymdir) * deviation) >> 6;
  else
    deviation = ((asymdir - 255) * deviation) >> 6;
  int32_t xs = size - deviation;
  int32_t ys = size + deviation;
  xsize = static_cast<uint32_t>(xs < 255 ? xs : 255);
  ysize = static_cast<uint32_t>(ys < 255 ? ys : 255);
}

// ParticleSystem2D::updateSize() (private). Returns false if the particle
// shrank to zero size (caller kills it).
bool update_size(AdvancedParticle *advprops, SizeControl *advsize) {
  if (advsize == nullptr) return false;
  int32_t newsize = advprops->size;
  uint32_t counter = advsize->size_counter;
  uint32_t increment = 0;
  if (advsize->grow)
    increment = advsize->grow_speed;
  else if (advsize->shrink)
    increment = advsize->shrink_speed;
  if (increment < 9) {
    counter += increment;
    if (counter > 7) {
      counter -= 8;
      increment = 1;
    } else {
      increment = 0;
    }
    advsize->size_counter = static_cast<uint8_t>(counter);
  } else {
    increment = (increment - 8) << 1;
  }

  if (advsize->grow) {
    if (newsize < advsize->maxsize) {
      newsize += static_cast<int32_t>(increment);
      if (newsize >= advsize->maxsize) {
        advsize->grow = false;
        newsize = advsize->maxsize;
        if (advsize->pulsate) advsize->shrink = true;
      }
    }
  } else if (advsize->shrink) {
    if (newsize > advsize->minsize) {
      newsize -= static_cast<int32_t>(increment);
      if (newsize <= advsize->minsize) {
        if (advsize->minsize == 0) return false;
        advsize->shrink = false;
        newsize = advsize->minsize;
        if (advsize->pulsate) advsize->grow = true;
      }
    }
  }
  advprops->size = static_cast<uint8_t>(newsize);
  if (advsize->wobble) advsize->asymdir = static_cast<uint8_t>(advsize->asymdir + advsize->wobble_speed);
  return true;
}

void render_large_particle(uint32_t size, uint32_t particleindex, uint8_t brightness, uint32_t color, bool wrapX,
                            bool wrapY) {
  int32_t x_subcenter = particles[particleindex].x;
  int32_t y_subcenter = particles[particleindex].y;
  int32_t x_center = x_subcenter >> kPRadiusShift;
  int32_t y_center = y_subcenter >> kPRadiusShift;

  uint32_t xsize = size;
  uint32_t ysize = size;
  if (adv_size != nullptr && adv_size[particleindex].asymmetry > 0) {
    get_particle_xy_size(&adv_particles[particleindex], &adv_size[particleindex], xsize, ysize);
  }

  int32_t rx_subpixel = static_cast<int32_t>(xsize) + kPRadius + 1;
  int32_t ry_subpixel = static_cast<int32_t>(ysize) + kPRadius + 1;
  int32_t rx_pixels = rx_subpixel >> kPRadiusShift;
  int32_t ry_pixels = ry_subpixel >> kPRadiusShift;

  int32_t x_min = x_center - rx_pixels;
  int32_t x_max = x_center + rx_pixels;
  int32_t y_min = y_center - ry_pixels;
  int32_t y_max = y_center + ry_pixels;

  constexpr uint32_t kMatrixX = static_cast<uint32_t>(kMaxXPixel) + 1;
  constexpr uint32_t kMatrixY = static_cast<uint32_t>(kMaxYPixel) + 1;
  int32_t rx_sq = rx_subpixel * rx_subpixel;
  int32_t ry_sq = ry_subpixel * ry_subpixel;

  for (int32_t py = y_min; py <= y_max; py++) {
    for (int32_t px = x_min; px <= x_max; px++) {
      int32_t render_x = px;
      int32_t render_y = py;
      if (render_x < 0) {
        if (!wrapX) continue;
        render_x += static_cast<int32_t>(kMatrixX);
      } else if (render_x > kMaxXPixel) {
        if (!wrapX) continue;
        render_x -= static_cast<int32_t>(kMatrixX);
      }
      if (render_y < 0) {
        if (!wrapY) continue;
        render_y += static_cast<int32_t>(kMatrixY);
      } else if (render_y > kMaxYPixel) {
        if (!wrapY) continue;
        render_y -= static_cast<int32_t>(kMatrixY);
      }

      int32_t dx_subpixel = (px << kPRadiusShift) - x_subcenter + kPHalfRadius;
      int32_t dy_subpixel = (py << kPRadiusShift) - y_subcenter + kPHalfRadius;
      uint8_t pixel_brightness = calculate_ellipse_brightness(dx_subpixel, dy_subpixel, rx_sq, ry_sq, brightness);
      if (pixel_brightness == 0) continue;

      // flip y: (0,0) is bottom-left in particle space, top-left in frame
      uint32_t idx = static_cast<uint32_t>(render_x) + (static_cast<uint32_t>(kMaxYPixel) - static_cast<uint32_t>(render_y)) * kMatrixX;
      g_framebuffer[idx] = fast_color_scale_add(g_framebuffer[idx], color, pixel_brightness);
    }
  }
}

// ParticleSystem2D::renderParticle() (private) - dispatches to the
// single-pixel/standard-2x2/large-ellipse cases, matching real WLED
// exactly.
void render_particle(uint32_t particleindex, uint8_t brightness, uint32_t color, bool wrapX, bool wrapY) {
  uint32_t size = g_particle_size;
  if (per_particle_size && adv_particles != nullptr) size = 1u + adv_particles[particleindex].size;

  if (size == 0) {
    uint32_t x = static_cast<uint32_t>(particles[particleindex].x >> kPRadiusShift);
    uint32_t y = static_cast<uint32_t>(particles[particleindex].y >> kPRadiusShift);
    if (x <= static_cast<uint32_t>(kMaxXPixel) && y <= static_cast<uint32_t>(kMaxYPixel)) {
      uint32_t index = x + (static_cast<uint32_t>(kMaxYPixel) - y) * (static_cast<uint32_t>(kMaxXPixel) + 1);
      g_framebuffer[index] = fast_color_scale_add(g_framebuffer[index], color, brightness);
    }
    return;
  }

  if (size > 1) {
    render_large_particle(size, particleindex, brightness, color, wrapX, wrapY);
    return;
  }

  // size == 1: standard 2x2 bilinear rendering.
  uint8_t pxlbrightness[4];
  struct {
    int32_t x, y;
  } pixco[4];
  bool pixelvalid[4] = {true, true, true, true};

  int32_t xoffset = particles[particleindex].x + kPHalfRadius;
  int32_t yoffset = particles[particleindex].y + kPHalfRadius;
  int32_t dx = xoffset & (kPRadius - 1);
  int32_t dy = yoffset & (kPRadius - 1);
  int32_t x = xoffset >> kPRadiusShift;
  int32_t y = yoffset >> kPRadiusShift;

  pixco[1].x = pixco[2].x = x;
  pixco[2].y = pixco[3].y = y;
  x--;
  y--;
  pixco[0].x = pixco[3].x = x;
  pixco[0].y = pixco[1].y = y;

  int32_t precal1 = kPRadius - dx;
  int32_t precal2 = (kPRadius - dy) * brightness;
  int32_t precal3 = dy * brightness;
  pxlbrightness[0] = static_cast<uint8_t>((precal1 * precal2) >> kPSurface);
  pxlbrightness[1] = static_cast<uint8_t>((dx * precal2) >> kPSurface);
  pxlbrightness[2] = static_cast<uint8_t>((dx * precal3) >> kPSurface);
  pxlbrightness[3] = static_cast<uint8_t>((precal1 * precal3) >> kPSurface);

  if (pixco[0].x < 0) {
    if (wrapX) {
      pixco[0].x = pixco[3].x = kMaxXPixel;
    } else {
      pixelvalid[0] = pixelvalid[3] = false;
      if (pixco[0].x < -1) return;
    }
  } else if (pixco[1].x > kMaxXPixel) {
    if (wrapX) {
      pixco[1].x = pixco[2].x = 0;
    } else {
      pixelvalid[1] = pixelvalid[2] = false;
      if (pixco[0].x > kMaxXPixel) return;
    }
  }

  if (pixco[0].y < 0) {
    if (wrapY) {
      pixco[0].y = pixco[1].y = kMaxYPixel;
    } else {
      pixelvalid[0] = pixelvalid[1] = false;
      if (pixco[0].y < -1) return;
    }
  } else if (pixco[2].y > kMaxYPixel) {
    if (wrapY) {
      pixco[2].y = pixco[3].y = 0;
    } else {
      pixelvalid[2] = pixelvalid[3] = false;
      if (pixco[2].y > kMaxYPixel + 1) return;
    }
  }

  for (uint32_t i = 0; i < 4; i++) {
    if (pixelvalid[i]) {
      // flip y: (0,0) is bottom-left in particle space, top-left in frame
      uint32_t idx = static_cast<uint32_t>(pixco[i].x) +
                     (static_cast<uint32_t>(kMaxYPixel) - static_cast<uint32_t>(pixco[i].y)) *
                         (static_cast<uint32_t>(kMaxXPixel) + 1);
      g_framebuffer[idx] = fast_color_scale_add(g_framebuffer[idx], color, pxlbrightness[i]);
    }
  }
}

// Segment::blur2D(), smear=true branch only - the only variant real WLED's
// render() ever calls it with (`SEGMENT.blur2D(smearBlur, smearBlur,
// true)`), applied directly to the accumulator instead of a live segment
// pixel buffer.
void smear_blur_2d(uint8_t blur_x, uint8_t blur_y) {
  constexpr unsigned kCols = GuDisplay::WIDTH;
  constexpr unsigned kRows = GuDisplay::HEIGHT;
  // smear=true is the only mode real WLED's render() ever calls this with
  // (SEGMENT.blur2D(smearBlur, smearBlur, true)) - "keep" fixed at 255
  // rather than 255-blur, matching that call site exactly.
  constexpr uint8_t kKeep = 255;
  auto xy = [](unsigned x, unsigned y) { return x + y * kCols; };
  if (blur_x) {
    uint8_t seepx = blur_x >> 1;
    for (unsigned row = 0; row < kRows; row++) {
      uint32_t cur = g_framebuffer[xy(0, row)];
      uint32_t carryover = fast_color_scale(cur, seepx);
      g_framebuffer[xy(0, row)] = fast_color_scale(cur, kKeep);
      for (unsigned x = 1; x < kCols; x++) {
        cur = g_framebuffer[xy(x, row)];
        uint32_t part = fast_color_scale(cur, seepx);
        cur = fast_color_scale(cur, kKeep);
        cur = color_add(cur, carryover);
        g_framebuffer[xy(x - 1, row)] = color_add(g_framebuffer[xy(x - 1, row)], part);
        g_framebuffer[xy(x, row)] = cur;
        carryover = part;
      }
    }
  }
  if (blur_y) {
    uint8_t seepy = blur_y >> 1;
    for (unsigned col = 0; col < kCols; col++) {
      uint32_t cur = g_framebuffer[xy(col, 0)];
      uint32_t carryover = fast_color_scale(cur, seepy);
      g_framebuffer[xy(col, 0)] = fast_color_scale(cur, kKeep);
      for (unsigned y = 1; y < kRows; y++) {
        cur = g_framebuffer[xy(col, y)];
        uint32_t part = fast_color_scale(cur, seepy);
        cur = fast_color_scale(cur, kKeep);
        cur = color_add(cur, carryover);
        g_framebuffer[xy(col, y - 1)] = color_add(g_framebuffer[xy(col, y - 1)], part);
        g_framebuffer[xy(col, y)] = cur;
        carryover = part;
      }
    }
  }
}

// ParticleSystem2D::fireParticleupdate() (private).
void fire_particle_update() {
  for (uint32_t i = 0; i < used_particles; i++) {
    if (particles[i].ttl > 0) {
      particles[i].ttl--;
      int32_t newY = particles[i].y + static_cast<int32_t>(particles[i].vy) + (particles[i].ttl >> 2);
      int32_t newX = particles[i].x + static_cast<int32_t>(particles[i].vx);
      particle_flags[i].out_of_bounds = false;
      if (newY < -kPHalfRadius) {
        particle_flags[i].out_of_bounds = true;
      } else if (newY > max_y + kPHalfRadius) {
        particles[i].ttl = 0;
      } else {
        if ((newX < 0) || (newX > max_x)) {
          if (g_settings.wrap_x) {
            newX = newX % (max_x + 1);
            if (newX < 0) newX += max_x + 1;
          } else if ((newX < -kPHalfRadius) || (newX > max_x + kPHalfRadius)) {
            particles[i].ttl = 0;
          }
        }
        particles[i].x = static_cast<int16_t>(newX);
      }
      particles[i].y = static_cast<int16_t>(newY);
    }
  }
}

// ParticleSystem2D::applyGravity() (private, all-particles overload).
void apply_gravity_all() {
  int32_t dv = calc_force_dv(g_gforce, g_gforce_counter);
  if (dv == 0) return;
  for (uint32_t i = 0; i < used_particles; i++) {
    particles[i].vy = static_cast<int8_t>(limit_speed(static_cast<int32_t>(particles[i].vy) - dv));
  }
}

// ParticleSystem2D::collideParticles() (private).
void collide_particles(Particle &p1, Particle &p2, int32_t dx, int32_t dy, uint32_t coll_dist_sq, int32_t massratio1,
                        int32_t massratio2) {
  int32_t distance_sq = dx * dx + dy * dy;
  int32_t rel_vx = static_cast<int32_t>(p2.vx) - p1.vx;
  int32_t rel_vy = static_cast<int32_t>(p2.vy) - p1.vy;

  if (distance_sq == 0) {
    dx = -1;
    if (rel_vx < 0)
      dx = 1;
    else if (rel_vx == 0)
      rel_vx = 1;
    dy = -1;
    if (rel_vy < 0)
      dy = 1;
    else if (rel_vy == 0)
      rel_vy = 1;
    distance_sq = 2;
  }

  int32_t dot_product = dx * rel_vx + dy * rel_vy;
  if (dot_product < 0) {
    int32_t surface_hardness =
        g_collision_hardness > (kPMinSurfaceHardness >> 1) ? g_collision_hardness : (kPMinSurfaceHardness >> 1);
    int32_t impulse = ((((-dot_product) << 15) / distance_sq) * surface_hardness) >> 8;
    int32_t ximpulse = (impulse * dx + ((dx >> 31) & 0x7FFF)) >> 15;
    int32_t yimpulse = (impulse * dy + ((dy >> 31) & 0x7FFF)) >> 15;

    if (massratio1) {
      int32_t vx1 = p1.vx - ((ximpulse * massratio1) >> 7);
      int32_t vy1 = p1.vy - ((yimpulse * massratio1) >> 7);
      int32_t vx2 = p2.vx + ((ximpulse * massratio2) >> 7);
      int32_t vy2 = p2.vy + ((yimpulse * massratio2) >> 7);
      p1.vx = static_cast<int8_t>(limit_speed(vx1));
      p1.vy = static_cast<int8_t>(limit_speed(vy1));
      p2.vx = static_cast<int8_t>(limit_speed(vx2));
      p2.vy = static_cast<int8_t>(limit_speed(vy2));
    } else {
      p1.vx = static_cast<int8_t>(p1.vx - ximpulse);
      p1.vy = static_cast<int8_t>(p1.vy - yimpulse);
      p2.vx = static_cast<int8_t>(p2.vx + ximpulse);
      p2.vy = static_cast<int8_t>(p2.vy + yimpulse);
    }
    if (g_collision_hardness < kPMinSurfaceHardness && (g_frame_counter & 0x07) == 0) {
      int32_t coeff = g_collision_hardness + (255 - kPMinSurfaceHardness);
      p1.vx = static_cast<int8_t>((static_cast<int32_t>(p1.vx) * coeff + ((static_cast<int32_t>(p1.vx) >> 31) & 0xFF)) >> 8);
      p1.vy = static_cast<int8_t>((static_cast<int32_t>(p1.vy) * coeff + ((static_cast<int32_t>(p1.vy) >> 31) & 0xFF)) >> 8);
      p2.vx = static_cast<int8_t>((static_cast<int32_t>(p2.vx) * coeff + ((static_cast<int32_t>(p2.vx) >> 31) & 0xFF)) >> 8);
      p2.vy = static_cast<int8_t>((static_cast<int32_t>(p2.vy) * coeff + ((static_cast<int32_t>(p2.vy) >> 31) & 0xFF)) >> 8);
    }
  }

  if (distance_sq < static_cast<int32_t>(coll_dist_sq) && (rel_vx * rel_vx + rel_vy * rel_vy < 50)) {
    bool fairlyrandom = (dot_product & 0x01) != 0;
    int32_t pushamount = 1 + ((static_cast<int32_t>(coll_dist_sq) - distance_sq) >> 13);
    int8_t pushx = static_cast<int8_t>(dx > 0 ? -pushamount : pushamount);
    int8_t pushy = static_cast<int8_t>(dy > 0 ? -pushamount : pushamount);

    if (g_collision_hardness < 5) {
      if (fairlyrandom) {
        p1.vx = 0;
        p1.vy = 0;
        p2.vx = 0;
        p2.vy = 0;
        p1.x = static_cast<int16_t>(p1.x + pushx);
        p1.y = static_cast<int16_t>(p1.y + pushy);
      }
    } else {
      if (fairlyrandom) {
        p1.vx = static_cast<int8_t>(p1.vx + pushx);
        p1.vy = static_cast<int8_t>(p1.vy + pushy);
      } else {
        p2.vx = static_cast<int8_t>(p2.vx - pushx);
        p2.vy = static_cast<int8_t>(p2.vy - pushy);
      }
    }
  }
}

// ParticleSystem2D::handleCollisions() (private).
void handle_collisions() {
  uint32_t coll_dist_sq = g_particle_hard_radius << 1;
  coll_dist_sq = coll_dist_sq * coll_dist_sq;
  int32_t bin_width = 6 * kPRadius;
  int32_t overlap = static_cast<int32_t>(g_particle_hard_radius) << 1;
  if (per_particle_size && adv_particles != nullptr) overlap = 512;

  uint32_t half_plus = (used_particles + 1) / 2;
  uint32_t max_bin_particles = half_plus > 50u ? half_plus : 50u;
  uint32_t num_bins = static_cast<uint32_t>((max_x + (bin_width - 1)) / bin_width);
  if (used_particles < max_bin_particles) {
    num_bins = 1;
    bin_width = max_x + 1;
  }

  // Fixed-size scratch space standing in for real WLED's stack VLA - see
  // this file's top comment.
  static uint16_t bin_indices[kMaxParticles];
  uint32_t bin_particle_count;
  uint32_t next_frame_start_idx = random16(static_cast<uint16_t>(used_particles));
  uint32_t pidx = g_collision_start_idx;

  for (uint32_t bin = 0; bin < num_bins; bin++) {
    bin_particle_count = 0;
    int32_t bin_start = static_cast<int32_t>(bin) * bin_width - overlap;
    int32_t bin_end = bin_start + bin_width + (overlap << 1);

    for (uint32_t i = 0; i < used_particles; i++) {
      if (particles[pidx].ttl > 0) {
        if (particles[pidx].x >= bin_start && particles[pidx].x <= bin_end) {
          if (!particle_flags[pidx].out_of_bounds && particle_flags[pidx].collide) {
            if (bin_particle_count >= max_bin_particles) {
              next_frame_start_idx = pidx;
              break;
            }
            bin_indices[bin_particle_count++] = static_cast<uint16_t>(pidx);
          }
        }
      }
      pidx++;
      if (pidx >= used_particles) pidx = 0;
    }

    int32_t massratio1 = 0;
    int32_t massratio2 = 0;
    for (uint32_t i = 0; i < bin_particle_count; i++) {
      uint32_t idx_i = bin_indices[i];
      for (uint32_t j = i + 1; j < bin_particle_count; j++) {
        uint32_t idx_j = bin_indices[j];
        uint32_t pair_coll_dist_sq = coll_dist_sq;
        if (per_particle_size && adv_particles != nullptr) {
          uint32_t d = static_cast<uint32_t>(kPMinHardRadius) * 2 +
                       (((static_cast<uint32_t>(adv_particles[idx_i].size) + adv_particles[idx_j].size) * 52) >> 6);
          pair_coll_dist_sq = d * d;
          uint32_t mass1 = static_cast<uint32_t>(kPRadius) + adv_particles[idx_i].size;
          uint32_t mass2 = static_cast<uint32_t>(kPRadius) + adv_particles[idx_j].size;
          mass1 *= mass1;
          mass2 *= mass2;
          uint32_t totalmass = mass1 + mass2;
          massratio1 = static_cast<int32_t>((mass2 << 8) / totalmass);
          massratio2 = static_cast<int32_t>((mass1 << 8) / totalmass);
        }
        int32_t dx = (particles[idx_j].x + particles[idx_j].vx) - (particles[idx_i].x + particles[idx_i].vx);
        if (dx * dx < static_cast<int32_t>(pair_coll_dist_sq)) {
          int32_t dy = (particles[idx_j].y + particles[idx_j].vy) - (particles[idx_i].y + particles[idx_i].vy);
          if (dy * dy < static_cast<int32_t>(pair_coll_dist_sq))
            collide_particles(particles[idx_i], particles[idx_j], dx, dy, pair_coll_dist_sq, massratio1, massratio2);
        }
      }
    }
  }
  g_collision_start_idx = static_cast<uint16_t>(next_frame_start_idx);
}

}  // namespace

// ---- direct state access definitions (real WLED's public data members) --
Particle *particles = nullptr;
ParticleFlags *particle_flags = nullptr;
Source *sources = nullptr;
AdvancedParticle *adv_particles = nullptr;
SizeControl *adv_size = nullptr;
int32_t max_x = 0;
int32_t max_y = 0;
uint32_t used_particles = 0;
uint32_t num_sources = 0;
bool per_particle_size = false;

// ---- settings ----

void set_used_particles(uint8_t percentage) {
  uint32_t v = (kMaxParticles * (static_cast<uint32_t>(percentage) + 1)) >> 8;
  used_particles = v > 1 ? v : 1;
}
void set_collision_hardness(uint8_t hardness) { g_collision_hardness = static_cast<int32_t>(hardness) + 1; }
void set_wall_hardness(uint8_t hardness) { g_wall_hardness = hardness; }
void set_wall_roughness(uint8_t roughness) { g_wall_roughness = roughness; }
void set_matrix_size(uint32_t x, uint32_t y) {
  max_x = static_cast<int32_t>(x) * kPRadius - 1;
  max_y = static_cast<int32_t>(y) * kPRadius - 1;
}
void set_wrap_x(bool enable) { g_settings.wrap_x = enable; }
void set_wrap_y(bool enable) { g_settings.wrap_y = enable; }
void set_bounce_x(bool enable) { g_settings.bounce_x = enable; }
void set_bounce_y(bool enable) { g_settings.bounce_y = enable; }
void set_kill_out_of_bounds(bool enable) { g_settings.kill_out_of_bounds = enable; }
void set_saturation(uint8_t sat) {
  for (uint32_t i = 0; i < used_particles; i++) particles[i].sat = sat;
}
void set_color_by_age(bool enable) { g_settings.color_by_age = enable; }
void set_motion_blur(uint8_t bluramount) { g_motion_blur = bluramount; }
void set_smear_blur(uint8_t bluramount) { g_smear_blur = bluramount; }
void set_particle_size(uint8_t size) {
  g_particle_size = size;
  g_particle_hard_radius = kPMinHardRadius;
  per_particle_size = false;
  if (g_particle_size > 1) {
    g_particle_hard_radius = static_cast<uint32_t>(kPMinHardRadius + ((static_cast<int32_t>(g_particle_size) * 52) >> 6));
  } else if (g_particle_size == 0) {
    g_particle_hard_radius = static_cast<uint32_t>(kPMinHardRadius >> 1);
  }
}
void set_gravity(int8_t force) {
  if (force) {
    g_gforce = force;
    g_settings.use_gravity = true;
  } else {
    g_settings.use_gravity = false;
  }
}
void enable_particle_collisions(bool enable, uint8_t hardness) {
  g_settings.use_collisions = enable;
  g_collision_hardness = static_cast<int32_t>(hardness) + 1;
}

// ---- lifecycle ----

void begin(bool advanced, bool size_control) {
  if (size_control) advanced = true;

  for (uint32_t i = 0; i < kMaxParticles; i++) {
    g_particles_storage[i] = Particle{};
    g_particles_storage[i].sat = 255;
    g_particle_flags_storage[i] = ParticleFlags{};
    g_adv_particles_storage[i] = AdvancedParticle{};
    g_adv_size_storage[i] = SizeControl{};
  }
  for (uint32_t i = 0; i < kMaxSources; i++) {
    g_sources_storage[i] = Source{};
    g_sources_storage[i].source.sat = 255;
    g_sources_storage[i].source.ttl = 1;
  }
  for (int i = 0; i < kPixelCount; i++) g_framebuffer[i] = 0;

  particles = g_particles_storage;
  particle_flags = g_particle_flags_storage;
  sources = g_sources_storage;
  adv_particles = advanced ? g_adv_particles_storage : nullptr;
  adv_size = size_control ? g_adv_size_storage : nullptr;

  used_particles = kMaxParticles;
  num_sources = kMaxSources;
  per_particle_size = advanced;

  g_settings = Settings2D{};
  g_emit_index = 0;
  g_collision_start_idx = 0;
  g_collision_hardness = 0;
  g_force_counter = 0;
  g_gforce_counter = 0;
  g_gforce = 0;
  g_fire_intensity = 0;
  g_frame_counter = 0;
  g_palette_id = 0;
  g_primary = Rgb{255, 0, 0};
  g_secondary = Rgb{0, 0, 0};
  g_tertiary = Rgb{0, 0, 0};

  set_matrix_size(GuDisplay::WIDTH, GuDisplay::HEIGHT);
  set_wall_hardness(255);
  set_wall_roughness(0);
  set_gravity(0);
  set_particle_size(1);
  set_motion_blur(0);
  set_smear_blur(0);
}

void update_system() {
  // Real WLED's updateSystem() re-reads the segment's current pixel size
  // every FX call, since a segment can be resized/reconfigured between
  // calls. This board is always exactly 32x32 (set once in begin()), so
  // there is nothing to refresh - kept as a no-op for call-site parity.
}

void update() {
  g_frame_counter++;
  if (g_settings.use_gravity) apply_gravity_all();

  if (adv_size != nullptr) {
    for (uint32_t i = 0; i < used_particles; i++) {
      if (!update_size(&adv_particles[i], &adv_size[i])) particles[i].ttl = 0;
    }
  }

  if (g_settings.use_collisions) handle_collisions();

  for (uint32_t i = 0; i < used_particles; i++) {
    particle_move_update(particles[i], particle_flags[i], nullptr, adv_particles ? &adv_particles[i] : nullptr);
  }
}

void update_fire(uint8_t intensity) {
  g_frame_counter++;
  fire_particle_update();
  g_fire_intensity = intensity > 0 ? intensity : 1;
}

void set_colors(uint8_t palette_id, Rgb primary, Rgb secondary, Rgb tertiary) {
  g_palette_id = palette_id;
  g_primary = primary;
  g_secondary = secondary;
  g_tertiary = tertiary;
}

void render(Frame frame) {
  if (g_motion_blur) {
    for (int i = 0; i < kPixelCount; i++) g_framebuffer[i] = fast_color_scale(g_framebuffer[i], g_motion_blur);
  } else {
    for (int i = 0; i < kPixelCount; i++) g_framebuffer[i] = 0;
  }

  for (uint32_t i = 0; i < used_particles; i++) {
    if (particles[i].ttl == 0 || particle_flags[i].out_of_bounds) continue;

    uint8_t brightness;
    Rgb base_rgb;
    if (g_fire_intensity) {
      uint32_t b = static_cast<uint32_t>(particles[i].ttl) * (3u + (g_fire_intensity >> 5)) + 5u;
      brightness = static_cast<uint8_t>(b > 255u ? 255u : b);
      base_rgb = color_from_palette(g_palette_id, brightness, g_primary, g_secondary, g_tertiary);
    } else {
      int b = particles[i].ttl << 1;
      brightness = static_cast<uint8_t>(b > 255 ? 255 : b);
      base_rgb = color_from_palette(g_palette_id, particles[i].hue, g_primary, g_secondary, g_tertiary);
      if (particles[i].sat < 255) base_rgb = apply_saturation(base_rgb, particles[i].sat);
    }
    render_particle(i, brightness, pack_color(base_rgb), g_settings.wrap_x, g_settings.wrap_y);
  }

  if (g_smear_blur) smear_blur_2d(g_smear_blur, g_smear_blur);

  for (int y = 0; y <= kMaxYPixel; y++) {
    for (int x = 0; x <= kMaxXPixel; x++) {
      frame[y][x] = unpack_color(g_framebuffer[static_cast<uint32_t>(y) * (static_cast<uint32_t>(kMaxXPixel) + 1) +
                                                static_cast<uint32_t>(x)]);
    }
  }
}

// ---- emitters ----

int32_t spray_emit(const Source &emitter) {
  bool success = false;
  for (uint32_t i = 0; i < used_particles; i++) {
    g_emit_index++;
    if (g_emit_index >= used_particles) g_emit_index = 0;
    if (particles[g_emit_index].ttl == 0) {
      success = true;
      int32_t dx = static_cast<int32_t>(random16(static_cast<uint16_t>(emitter.var << 1))) - emitter.var;
      int32_t dy = static_cast<int32_t>(random16(static_cast<uint16_t>(emitter.var << 1))) - emitter.var;
      if (emitter.var > 5) {
        while (dx * dx + dy * dy > emitter.var * emitter.var) {
          dx = static_cast<int32_t>(random16(static_cast<uint16_t>(emitter.var << 1))) - emitter.var;
          dy = static_cast<int32_t>(random16(static_cast<uint16_t>(emitter.var << 1))) - emitter.var;
        }
      }
      particles[g_emit_index].vx = static_cast<int8_t>(emitter.vx + dx);
      particles[g_emit_index].vy = static_cast<int8_t>(emitter.vy + dy);
      particles[g_emit_index].x = emitter.source.x;
      particles[g_emit_index].y = emitter.source.y;
      particles[g_emit_index].hue = emitter.source.hue;
      particles[g_emit_index].sat = emitter.source.sat;
      particle_flags[g_emit_index].collide = emitter.source_flags.collide;
      particles[g_emit_index].ttl = random16(emitter.min_life, emitter.max_life);
      if (adv_particles != nullptr) adv_particles[g_emit_index].size = emitter.size;
      break;
    }
  }
  return success ? static_cast<int32_t>(g_emit_index) : -1;
}

void flame_emit(const Source &emitter) {
  int32_t idx = spray_emit(emitter);
  // Preserves real WLED's own `emitIndex > 0` check verbatim (not `>= 0`) -
  // a particle landing at slot 0 silently skips the ttl bonus upstream too.
  if (idx > 0) particles[idx].ttl = static_cast<uint16_t>(particles[idx].ttl + emitter.source.ttl);
}

int32_t angle_emit(Source &emitter, uint16_t angle, int32_t speed) {
  emitter.vx = static_cast<int8_t>((static_cast<int32_t>(cos16(angle)) * speed) / 32600);
  emitter.vy = static_cast<int8_t>((static_cast<int32_t>(sin16(angle)) * speed) / 32600);
  return spray_emit(emitter);
}

// ---- physics ----

void apply_gravity(Particle &part) {
  uint8_t counter_backup = g_gforce_counter;
  int32_t dv = calc_force_dv(g_gforce, g_gforce_counter);
  g_gforce_counter = counter_backup;
  part.vy = static_cast<int8_t>(limit_speed(static_cast<int32_t>(part.vy) - dv));
}

void apply_force(Particle &part, int8_t xforce, int8_t yforce, uint8_t &counter) {
  uint8_t xcounter = counter & 0x0F;
  uint8_t ycounter = counter >> 4;
  int32_t dvx = calc_force_dv(xforce, xcounter);
  int32_t dvy = calc_force_dv(yforce, ycounter);
  counter = static_cast<uint8_t>(xcounter & 0x0F);
  counter = static_cast<uint8_t>(counter | ((ycounter << 4) & 0xF0));
  part.vx = static_cast<int8_t>(limit_speed(static_cast<int32_t>(part.vx) + dvx));
  part.vy = static_cast<int8_t>(limit_speed(static_cast<int32_t>(part.vy) + dvy));
}

void apply_force(uint32_t particle_index, int8_t xforce, int8_t yforce) {
  if (adv_particles == nullptr) return;
  apply_force(particles[particle_index], xforce, yforce, adv_particles[particle_index].force_counter);
}

void apply_force(int8_t xforce, int8_t yforce) {
  uint8_t tmp = g_force_counter;
  for (uint32_t i = 0; i < used_particles; i++) {
    tmp = g_force_counter;
    apply_force(particles[i], xforce, yforce, tmp);
  }
  g_force_counter = tmp;
}

void apply_angle_force(Particle &part, int8_t force, uint16_t angle, uint8_t &counter) {
  int8_t xforce = static_cast<int8_t>((static_cast<int32_t>(force) * cos16(angle)) / 32767);
  int8_t yforce = static_cast<int8_t>((static_cast<int32_t>(force) * sin16(angle)) / 32767);
  apply_force(part, xforce, yforce, counter);
}

void apply_angle_force(uint32_t particle_index, int8_t force, uint16_t angle) {
  if (adv_particles == nullptr) return;
  apply_angle_force(particles[particle_index], force, angle, adv_particles[particle_index].force_counter);
}

void apply_angle_force(int8_t force, uint16_t angle) {
  int8_t xforce = static_cast<int8_t>((static_cast<int32_t>(force) * cos16(angle)) / 32767);
  int8_t yforce = static_cast<int8_t>((static_cast<int32_t>(force) * sin16(angle)) / 32767);
  apply_force(xforce, yforce);
}

void apply_friction(Particle &part, int32_t coefficient) {
  int32_t friction = 256 - coefficient;
  part.vx = static_cast<int8_t>((static_cast<int32_t>(part.vx) * friction + ((static_cast<int32_t>(part.vx) >> 31) & 0xFF)) >> 8);
  part.vy = static_cast<int8_t>((static_cast<int32_t>(part.vy) * friction + ((static_cast<int32_t>(part.vy) >> 31) & 0xFF)) >> 8);
}

void apply_friction(int32_t coefficient) {
  int32_t friction = 256 - coefficient;
  for (uint32_t i = 0; i < used_particles; i++) {
    particles[i].vx = static_cast<int8_t>(
        (static_cast<int32_t>(particles[i].vx) * friction + ((static_cast<int32_t>(particles[i].vx) >> 31) & 0xFF)) >> 8);
    particles[i].vy = static_cast<int8_t>(
        (static_cast<int32_t>(particles[i].vy) * friction + ((static_cast<int32_t>(particles[i].vy) >> 31) & 0xFF)) >> 8);
  }
}

void point_attractor(uint32_t particle_index, Particle &attractor, uint8_t strength, bool swallow) {
  if (adv_particles == nullptr) return;
  int32_t dx = attractor.x - particles[particle_index].x;
  int32_t dy = attractor.y - particles[particle_index].y;
  int32_t distance_sq = dx * dx + dy * dy;
  if (distance_sq < 8192) {
    if (swallow) {
      if (particles[particle_index].ttl > 7) {
        particles[particle_index].ttl = static_cast<uint16_t>(particles[particle_index].ttl - 8);
      } else {
        particles[particle_index].ttl = 0;
        return;
      }
    }
    distance_sq = 2 * kPRadius * kPRadius;
  }
  int32_t force = (static_cast<int32_t>(strength) << 16) / distance_sq;
  int8_t xforce = static_cast<int8_t>((force * dx) / 1024);
  int8_t yforce = static_cast<int8_t>((force * dy) / 1024);
  apply_force(particle_index, xforce, yforce);
}

void particle_move_update(Particle &part, ParticleFlags &part_flags, Settings2D *options,
                           AdvancedParticle *advanced_properties) {
  if (options == nullptr) options = &g_settings;

  if (part.ttl > 0) {
    if (!part_flags.perpetual) part.ttl--;
    if (options->color_by_age) part.hue = static_cast<uint8_t>(part.ttl > 255 ? 255 : part.ttl);

    int32_t renderradius = kPHalfRadius - 1 + g_particle_size;
    int32_t newX = part.x + static_cast<int32_t>(part.vx);
    int32_t newY = part.y + static_cast<int32_t>(part.vy);
    part_flags.out_of_bounds = false;

    if (per_particle_size && advanced_properties != nullptr) {
      renderradius = kPHalfRadius - 1 + advanced_properties->size;
      if (advanced_properties->size > 0) {
        g_particle_hard_radius = static_cast<uint32_t>(kPMinHardRadius + ((advanced_properties->size * 52) >> 6));
      } else {
        g_particle_hard_radius = static_cast<uint32_t>(kPMinHardRadius >> 1);
      }
    }

    if (options->bounce_y) {
      if ((newY < static_cast<int32_t>(g_particle_hard_radius)) ||
          ((newY > (max_y - static_cast<int32_t>(g_particle_hard_radius))) && !options->use_gravity)) {
        bounce(part.vy, part.vx, newY, static_cast<uint32_t>(max_y));
      }
    }

    if (!check_bounds_and_wrap(newY, max_y, renderradius, options->wrap_y)) {
      part_flags.out_of_bounds = true;
      if (options->kill_out_of_bounds) {
        if (newY < 0)
          part.ttl = 0;
        else if (!options->use_gravity)
          part.ttl = 0;
      }
    }

    if (part.ttl) {
      if (options->bounce_x) {
        if ((newX < static_cast<int32_t>(g_particle_hard_radius)) ||
            (newX > (max_x - static_cast<int32_t>(g_particle_hard_radius)))) {
          bounce(part.vx, part.vy, newX, static_cast<uint32_t>(max_x));
        }
      } else if (!check_bounds_and_wrap(newX, max_x, renderradius, options->wrap_x)) {
        part_flags.out_of_bounds = true;
        if (options->kill_out_of_bounds) part.ttl = 0;
      }
    }

    part.x = static_cast<int16_t>(newX);
    part.y = static_cast<int16_t>(newY);
  }
}

}  // namespace ps2d
}  // namespace effects
