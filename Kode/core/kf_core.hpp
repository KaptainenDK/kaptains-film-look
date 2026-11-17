// Kaptains Film Look - Copyright (C) 2026 Henrik Jonas Kaptain
// SPDX-License-Identifier: GPL-3.0-or-later (with an additional permission for the Adobe SDKs, see LICENSE-EXCEPTION)
// Kaptains Film Look - core image processing library (v2)
// Host-independent (no Adobe SDK code here). All sizes are derived from the image
// diagonal so the look is identical at any resolution, aspect ratio or orientation.
//
// Fixed processing order (follows the path of light):
//   1. Lens Distortion   (geometry: barrel / pincushion, auto-zoom, manual zoom, edge mode)
//   2. Chromatic Aberration (resampled together with 1 in a single pass; optional Blur)
//   3. Lens Blur         (Soft / Radial / Swirl / Directional, driven by a zone)
//   4. Vignette
//   5. Bloom             (optional Blur and Stretch)
//   6. Halation          (optional Blur and Stretch)
//   7. Grain
//
// Zones: every effect has a Start and a Full distance (0 = centre, 1 = corner of the active
// picture area). The effect is 0 inside Start, reaches full strength at Full, with a smooth
// transition in between. Start = Full = 0 means "full effect everywhere" (the default).
// All zones share one movable centre and one zone shape (circle <-> frame-shaped oval).
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace kf {

enum class InputSpace { Linear = 0, sRGB = 1, Rec709 = 2 };    // how the incoming pixels are encoded
enum class EdgeMode { Transparent = 0, Clamp = 1, Mirror = 2 };  // what is shown outside the frame
enum class LensBlurType { Soft = 0, Radial = 1, Swirl = 2, Directional = 3 };

// Interleaved float buffer. Images are RGBA (c = 4).
struct Buf {
    int w = 0, h = 0, c = 0;
    std::vector<float> d;
    Buf() {}
    Buf(int w_, int h_, int c_) : w(w_), h(h_), c(c_), d((size_t)w_ * h_ * c_, 0.f) {}
    float* px(int x, int y) { return &d[((size_t)y * w + x) * c]; }
    const float* px(int x, int y) const { return &d[((size_t)y * w + x) * c]; }
};

struct Rect { float x0 = 0.f, y0 = 0.f, x1 = 1.f, y1 = 1.f; }; // normalised 0..1
struct ZoneRange { float start = 0.f, full = 0.f; };            // 0 = centre, 1 = corner

struct Params {
    // ---- global ----
    InputSpace input_space = InputSpace::sRGB;
    float mix = 1.f;                 // 0..1  blend between original and processed
    Rect  active_area;               // picture area inside a letterboxed frame (default = full frame)
    float zone_center_x = 0.5f;      // 0..1 inside the active area (shared by all zones and the lens centre)
    float zone_center_y = 0.5f;
    float zone_roundness = 1.f;      // 0 = follows frame shape (oval), 1 = circular like a real lens
    bool  show_zones = false;        // draw zone rings into the image (preview aid, turn off for final renders)

    // ---- 1. Lens Distortion ----
    bool  lens_enable = true;
    float lens_amount = 0.f;         // -1..1   positive = barrel, negative = pincushion
    bool  lens_auto_zoom = true;     // zoom just enough so no empty edges appear
    float lens_zoom = 1.f;           // extra manual zoom multiplier (1 = none)
    EdgeMode lens_edge = EdgeMode::Clamp;
    ZoneRange lens_zone;

    // ---- 2. Chromatic Aberration ----
    bool  ca_enable = true;
    float ca_amount = 0.f;           // 1.0 = red/blue shifted 0.1% of half-diagonal at the corners
    float ca_blur = 0.f;             // 0..1  softens the colour fringes themselves (1 = 0.5% of the diagonal)
    ZoneRange ca_zone;

    // ---- 3. Lens Blur ----
    bool  lblur_enable = true;
    LensBlurType lblur_type = LensBlurType::Soft;
    float lblur_amount = 0.f;        // 1.0 = Soft: sigma 0.5% of diagonal; others: 1% of diagonal long
    float lblur_angle = 90.f;        // degrees, Directional only (0 = horizontal, 90 = vertical)
    ZoneRange lblur_zone = {0.5f, 1.0f};

    // ---- 4. Vignette ----
    bool  vig_enable = true;
    float vig_amount = 0.f;          // stops of darkening at full strength
    float vig_softness = 0.5f;       // 0..1   higher = broader, gentler falloff
    ZoneRange vig_zone = {0.35f, 1.0f};

    // ---- 5. Bloom ----
    bool  bloom_enable = true;
    float bloom_threshold = 0.6f;    // scene-linear level where glow starts (1.0 = white)
    float bloom_intensity = 0.f;     // 0 = off
    float bloom_radius = 1.5f;       // percent of image diagonal
    float bloom_blur = 0.f;          // 0..1  extra softness of the glow (1 = 1% of the diagonal)
    float bloom_stretch = 0.f;       // 0..1  stretches the glow along bloom_stretch_angle
    float bloom_stretch_angle = 0.f; // degrees (0 = horizontal, 90 = vertical)
    float bloom_tint[3] = {1.f, 1.f, 1.f};
    ZoneRange bloom_zone;

    // ---- 6. Halation ----
    bool  halation_enable = true;
    float halation_threshold = 0.8f;
    float halation_intensity = 0.f;
    float halation_radius = 0.8f;    // percent of image diagonal
    float halation_blur = 0.f;
    float halation_stretch = 0.f;
    float halation_stretch_angle = 0.f;
    float halation_tint[3] = {1.f, 0.30f, 0.06f}; // red/orange like the film base reflection
    ZoneRange halation_zone;

    // ---- 7. Grain (no blur: softness is set by size and roughness) ----
    bool  grain_enable = true;
    float grain_amount = 0.f;        // 1.0 = noise std-dev 0.05 in display-referred 0..1 (before response)
    float grain_size = 1.f;          // 1.0 = 1/1600 of the image diagonal (grain finer than ~1 pixel cannot be shown)
    float grain_roughness = 0.35f;   // 0 = fine only, 1 = coarse clumps
    float grain_color = 0.4f;        // 0 = monochrome, 1 = fully independent colour channels
    float grain_midtone_bias = 0.5f; // 0 = even over tonal range, 1 = strongest in midtones
    int   grain_seed = 1;
    int   frame = 0;                 // frame number; changes the grain pattern per frame
    bool  grain_animate = true;
    ZoneRange grain_zone;
};

// Process an RGBA float image. `out` is resized to match `in`.
void process(const Buf& in, Buf& out, const Params& p);

// Zoom that the auto-zoom option would pick for these settings (>= 1).
float auto_zoom_for(int w, int h, const Params& p);

} // namespace kf
