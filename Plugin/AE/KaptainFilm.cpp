// Kaptains Film Look - Copyright (C) 2026 Henrik Jonas Kaptain
// SPDX-License-Identifier: GPL-3.0-or-later (with an additional permission for the Adobe SDKs, see LICENSE-EXCEPTION)
// Kaptains Film Look - After Effects effect plug-in (milestone A: Vignette + Grain)
// The image processing lives in the host-independent core (Kode/core); this file only
// translates After Effects parameters and pixel buffers to and from it.
#include "KaptainFilm.h"

// Single translation unit with the core, so the build needs no extra source files.
#include "../../Kode/core/kf_core.cpp"

#include <cstdio>
#include <cstdarg>
#include <ctime>
#include <unistd.h>
#include <cmath>
#include <algorithm>
#include <adobesdk/DrawbotSuite.h>
#include "PrSDKAESupport.h"     // Premiere's pixel format suite (declaring 32-bit float BGRA is what makes Premiere offer the GPU path)
#include "KaptainFilmGPU.h"
#include "kf_gpu_plan.h"
#include <new>
#include <vector>

static PF_Err About(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], PF_LayerDef* output)
{
    snprintf(out_data->return_msg, sizeof(out_data->return_msg), "Kaptains Film Look v%d.%d (milestone B, multi-threaded)\rFilm look: lens, chromatic aberration, lens blur, vignette, bloom, halation, grain.",
             KF_MAJOR_VERSION, KF_MINOR_VERSION);
    return PF_Err_NONE;
}

static PF_Err GlobalSetup(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], PF_LayerDef* output)
{
    out_data->my_version = PF_VERSION(KF_MAJOR_VERSION, KF_MINOR_VERSION, KF_BUG_VERSION, KF_STAGE_VERSION, KF_BUILD_VERSION);
    // NON_PARAM_VARY: grain depends on the frame number, so AE must not reuse cached frames across time.
    // FLOAT_COLOR_AWARE (32 bpc) requires SUPPORTS_SMART_RENDER, so both are set. The classic PF_Cmd_RENDER path is
    // still implemented below (Premiere Pro may use it).
    out_data->out_flags = PF_OutFlag_DEEP_COLOR_AWARE | PF_OutFlag_NON_PARAM_VARY | PF_OutFlag_CUSTOM_UI;   // CUSTOM_UI: zone overlay in the comp window
    out_data->out_flags2 = PF_OutFlag2_FLOAT_COLOR_AWARE | PF_OutFlag2_SUPPORTS_SMART_RENDER | PF_OutFlag2_SUPPORTS_THREADED_RENDERING;
    if (in_data->appl_id != kAppID_Premiere) out_data->out_flags2 |= PF_OutFlag2_SUPPORTS_GPU_RENDER_F32;   // After Effects GPU path (Metal)
    else {
        // Premiere: say which pixel formats the effect works in. Without a 32-bit float format Premiere treats the effect as 8-bit
        // and never asks for the GPU filter (the Premiere GPU samples declare their float formats the same way).
        PF_PixelFormatSuite1* pfs = nullptr;
        if (in_data->pica_basicP->AcquireSuite(kPFPixelFormatSuite, kPFPixelFormatSuiteVersion1, (const void**)&pfs) == 0 && pfs) {
            pfs->ClearSupportedPixelFormats(in_data->effect_ref);
            pfs->AddSupportedPixelFormat(in_data->effect_ref, PrPixelFormat_BGRA_4444_32f);
            in_data->pica_basicP->ReleaseSuite(kPFPixelFormatSuite, kPFPixelFormatSuiteVersion1);
        }
    }
    return PF_Err_NONE;
}

static PF_Err ParamsSetup(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], PF_LayerDef* output)
{
    PF_ParamDef def;

#define KF_TOPIC(name, id) do { AEFX_CLR_STRUCT(def); PF_ADD_TOPIC(name, id); } while (0)
#define KF_END_TOPIC(id) do { AEFX_CLR_STRUCT(def); PF_END_TOPIC(id); } while (0)
#define KF_ON(id, dflt) do { AEFX_CLR_STRUCT(def); PF_ADD_CHECKBOXX("On", dflt, 0, id); } while (0)
#define KF_CHECK(name, dflt, id) do { AEFX_CLR_STRUCT(def); PF_ADD_CHECKBOXX(name, dflt, 0, id); } while (0)
#define KF_SLIDER(name, vmin, vmax, smin, smax, dflt, id) \
    do { AEFX_CLR_STRUCT(def); PF_ADD_FLOAT_SLIDERX(name, vmin, vmax, smin, smax, dflt, PF_Precision_HUNDREDTHS, 0, 0, id); } while (0)
#define KF_ZONE(zs_dflt, zf_dflt, id_s, id_f) \
    do { KF_SLIDER("Zone Start (0/0 = everywhere)", 0, 1.5, 0, 1.5, zs_dflt, id_s); KF_SLIDER("Zone Full", 0, 1.5, 0, 1.5, zf_dflt, id_f); } while (0)
#define KF_ANGLE(name, dflt, id) do { AEFX_CLR_STRUCT(def); PF_ADD_ANGLE(name, dflt, id); } while (0)
#define KF_COLOR(name, r, g, b, id) do { AEFX_CLR_STRUCT(def); PF_ADD_COLOR(name, r, g, b, id); } while (0)

    // Global + shared zone settings
    KF_TOPIC("Global & Zones (shared)", ID_ZONES_START);
    AEFX_CLR_STRUCT(def);
    PF_ADD_POINT("Zone Centre", 50, 50, FALSE, ID_ZONE_CENTER);
    KF_SLIDER("Zone Roundness (0 oval, 1 circle)", 0, 1, 0, 1, 1, ID_ZONE_ROUNDNESS);
    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Input Space", 3, 1, "sRGB|Rec.709 (gamma 2.4)|Linear", ID_INPUT_SPACE);
    KF_SLIDER("Mix (original to effect)", 0, 1, 0, 1, 1, ID_MIX);
    KF_CHECK("Show Zones (turn off before rendering!)", FALSE, ID_SHOW_ZONES);
    KF_END_TOPIC(ID_ZONES_END);

    // 1 Lens Distortion
    KF_TOPIC("1 Lens Distortion", ID_LENS_START);
    KF_ON(ID_LENS_ON, FALSE);
    KF_SLIDER("Amount (+ barrel, - pincushion)", -1, 1, -1, 1, 0.08, ID_LENS_AMOUNT);
    KF_CHECK("Auto Zoom (no empty edges)", TRUE, ID_LENS_AUTOZOOM);
    KF_SLIDER("Extra Zoom", 0.5, 3, 0.8, 1.5, 1.0, ID_LENS_ZOOM);
    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Edge", 3, 2, "Transparent|Clamp|Mirror", ID_LENS_EDGE);
    KF_ZONE(0.0, 0.0, ID_LENS_ZONE_START, ID_LENS_ZONE_FULL);
    KF_END_TOPIC(ID_LENS_END);

    // 2 Chromatic Aberration
    KF_TOPIC("2 Chromatic Aberration", ID_CA_START);
    KF_ON(ID_CA_ON, FALSE);
    KF_SLIDER("Amount", -5, 5, -3, 3, 0.8, ID_CA_AMOUNT);
    KF_SLIDER("Blur (softens the fringes)", 0, 1, 0, 1, 0.0, ID_CA_BLUR);
    KF_ZONE(0.0, 0.0, ID_CA_ZONE_START, ID_CA_ZONE_FULL);
    KF_END_TOPIC(ID_CA_END);

    // 3 Lens Blur
    KF_TOPIC("3 Lens Blur", ID_LB_START);
    KF_ON(ID_LB_ON, FALSE);
    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Type", 4, 1, "Soft|Radial|Swirl|Directional", ID_LB_TYPE);
    KF_SLIDER("Amount", 0, 10, 0, 4, 0.5, ID_LB_AMOUNT);
    KF_ANGLE("Angle (Directional only)", 90, ID_LB_ANGLE);
    KF_ZONE(0.5, 1.0, ID_LB_ZONE_START, ID_LB_ZONE_FULL);
    KF_END_TOPIC(ID_LB_END);

    // 4 Vignette
    KF_TOPIC("4 Vignette", ID_VIG_START);
    KF_ON(ID_VIG_ON, TRUE);
    KF_SLIDER("Amount (stops)", -4, 6, -4, 4, 1.0, ID_VIG_AMOUNT);
    KF_SLIDER("Softness", 0, 1, 0, 1, 0.5, ID_VIG_SOFTNESS);
    KF_SLIDER("Zone Start", 0, 1.5, 0, 1.5, 0.35, ID_VIG_ZONE_START);
    KF_SLIDER("Zone Full", 0, 1.5, 0, 1.5, 1.0, ID_VIG_ZONE_FULL);
    KF_END_TOPIC(ID_VIG_END);

    // 5 Bloom
    KF_TOPIC("5 Bloom", ID_BLOOM_START);
    KF_ON(ID_BLOOM_ON, FALSE);
    KF_SLIDER("Threshold (scene-linear)", 0, 4, 0, 2, 0.65, ID_BLOOM_THRESHOLD);
    KF_SLIDER("Intensity", 0, 5, 0, 2, 0.25, ID_BLOOM_INTENSITY);
    KF_SLIDER("Radius (% of diagonal)", 0.1, 20, 0.1, 6, 1.2, ID_BLOOM_RADIUS);
    KF_SLIDER("Blur", 0, 1, 0, 1, 0.0, ID_BLOOM_BLUR);
    KF_SLIDER("Stretch", 0, 1, 0, 1, 0.0, ID_BLOOM_STRETCH);
    KF_ANGLE("Stretch Angle", 0, ID_BLOOM_STRETCH_ANGLE);
    KF_COLOR("Tint", 255, 255, 255, ID_BLOOM_TINT);
    KF_ZONE(0.0, 0.0, ID_BLOOM_ZONE_START, ID_BLOOM_ZONE_FULL);
    KF_END_TOPIC(ID_BLOOM_END);

    // 6 Halation
    KF_TOPIC("6 Halation", ID_HAL_START);
    KF_ON(ID_HAL_ON, FALSE);
    KF_SLIDER("Threshold (scene-linear)", 0, 4, 0, 2, 0.8, ID_HAL_THRESHOLD);
    KF_SLIDER("Intensity", 0, 5, 0, 2, 0.35, ID_HAL_INTENSITY);
    KF_SLIDER("Radius (% of diagonal)", 0.1, 20, 0.1, 6, 0.7, ID_HAL_RADIUS);
    KF_SLIDER("Blur", 0, 1, 0, 1, 0.0, ID_HAL_BLUR);
    KF_SLIDER("Stretch", 0, 1, 0, 1, 0.0, ID_HAL_STRETCH);
    KF_ANGLE("Stretch Angle", 0, ID_HAL_STRETCH_ANGLE);
    KF_COLOR("Tint", 255, 77, 15, ID_HAL_TINT);
    KF_ZONE(0.0, 0.0, ID_HAL_ZONE_START, ID_HAL_ZONE_FULL);
    KF_END_TOPIC(ID_HAL_END);

    // 7 Grain
    KF_TOPIC("7 Grain", ID_GRAIN_START);
    KF_ON(ID_GRAIN_ON, TRUE);
    KF_SLIDER("Amount", 0, 4, 0, 2, 0.5, ID_GRAIN_AMOUNT);
    KF_SLIDER("Size", 0.1, 10, 0.1, 5, 1.0, ID_GRAIN_SIZE);
    KF_SLIDER("Roughness", 0, 1, 0, 1, 0.35, ID_GRAIN_ROUGHNESS);
    KF_SLIDER("Colour (0 mono, 1 colour)", 0, 1, 0, 1, 0.4, ID_GRAIN_COLOR);
    KF_SLIDER("Midtone Bias", 0, 1, 0, 1, 0.5, ID_GRAIN_MIDTONE);
    AEFX_CLR_STRUCT(def);
    PF_ADD_SLIDER("Seed", 1, 9999, 1, 999, 1, ID_GRAIN_SEED);
    KF_CHECK("Animate (new grain every frame)", TRUE, ID_GRAIN_ANIMATE);
    KF_ZONE(0.0, 0.0, ID_GRAIN_ZONE_START, ID_GRAIN_ZONE_FULL);
    KF_END_TOPIC(ID_GRAIN_END);

    // custom comp-window UI (the zone overlay); zero size = it only draws over the picture, no extra panel
    {
        PF_CustomUIInfo ci;
        AEFX_CLR_STRUCT(ci);
        ci.events = PF_CustomEFlag_LAYER | PF_CustomEFlag_COMP;
        ci.comp_ui_alignment = PF_UIAlignment_NONE;
        ci.layer_ui_alignment = PF_UIAlignment_NONE;
        ci.preview_ui_alignment = PF_UIAlignment_NONE;
        PF_Err e2 = (*(in_data->inter.register_ui))(in_data->effect_ref, &ci);
        if (e2) return e2;
    }

    out_data->num_params = KF_NUM_PARAMS;
    return PF_Err_NONE;
}

// ---- pixel conversion: AE (ARGB, premultiplied) <-> core (RGBA float, straight) ----
enum class Fmt { P8, P16, PF32, PREM32 };   // PREM32: Premiere's BGRA_4444_32f (floats in memory order B, G, R, A)
struct KFPremPixel { float b, g, r, a; };

template <typename PIX, typename GET>
static void to_core(const PF_EffectWorld* w, kf::Buf& b, GET get)
{
    for (int y = 0; y < b.h; ++y) {
        const PIX* row = (const PIX*)((const char*)w->data + (size_t)y * w->rowbytes);
        for (int x = 0; x < b.w; ++x) {
            float r, g, bl, a;
            get(row[x], r, g, bl, a);
            float* o = b.px(x, y);
            if (a > 0.f && a < 1.f) { r /= a; g /= a; bl /= a; }
            o[0] = r; o[1] = g; o[2] = bl; o[3] = a;
        }
    }
}

// Output pixel (x, y) takes core pixel (x + dx, y + dy); outside the core buffer it is transparent.
template <typename PIX, typename PUT>
static void from_core(const kf::Buf& b, PF_EffectWorld* w, int dx, int dy, PUT put)
{
    for (int y = 0; y < w->height; ++y) {
        PIX* row = (PIX*)((char*)w->data + (size_t)y * w->rowbytes);
        for (int x = 0; x < w->width; ++x) {
            float r = 0.f, g = 0.f, bl = 0.f, a = 0.f;
            const int sx = x + dx, sy = y + dy;
            if (sx >= 0 && sx < b.w && sy >= 0 && sy < b.h) {
                const float* p = b.px(sx, sy);
                a = p[3]; r = p[0] * a; g = p[1] * a; bl = p[2] * a;
            }
            put(row[x], r, g, bl, a);
        }
    }
}

static inline float clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }

// ---- parameter snapshot (plain data, so it can be carried from pre-render to render) ----
struct KFZone { float start, full; };
struct KFGlow {
    int on; float threshold, intensity, radius, blur, stretch, stretch_angle, tint[3]; KFZone zone;
};
struct KFInfo {
    int input_space;               // 1 sRGB, 2 Rec.709, 3 Linear (popup value)
    double px, py;                 // zone centre in layer pixels at the current resolution (AE scales point params itself)
    float roundness;               // shared zone roundness
    float mix;
    int show_zones;
    int lens_on; float lens_amount; int lens_autozoom; float lens_zoom; int lens_edge; KFZone lens_zone;
    int ca_on; float ca_amount, ca_blur; KFZone ca_zone;
    int lb_on; int lb_type; float lb_amount, lb_angle; KFZone lb_zone;
    int vig_on; float vig_amount, vig_softness; KFZone vig_zone;
    KFGlow bloom, hal;
    int grain_on; float g_amount, g_size, g_rough, g_color, g_mid; int g_seed, g_animate; KFZone g_zone;
    int frame;
};

// Every parameter the effect reads (group start/end markers are not read).
static const int kReadParams[] = {
    KF_ZONE_CENTER, KF_ZONE_ROUNDNESS, KF_INPUT_SPACE, KF_MIX, KF_SHOW_ZONES,
    KF_LENS_ON, KF_LENS_AMOUNT, KF_LENS_AUTOZOOM, KF_LENS_ZOOM, KF_LENS_EDGE, KF_LENS_ZONE_START, KF_LENS_ZONE_FULL,
    KF_CA_ON, KF_CA_AMOUNT, KF_CA_BLUR, KF_CA_ZONE_START, KF_CA_ZONE_FULL,
    KF_LB_ON, KF_LB_TYPE, KF_LB_AMOUNT, KF_LB_ANGLE, KF_LB_ZONE_START, KF_LB_ZONE_FULL,
    KF_VIG_ON, KF_VIG_AMOUNT, KF_VIG_SOFTNESS, KF_VIG_ZONE_START, KF_VIG_ZONE_FULL,
    KF_BLOOM_ON, KF_BLOOM_THRESHOLD, KF_BLOOM_INTENSITY, KF_BLOOM_RADIUS, KF_BLOOM_BLUR, KF_BLOOM_STRETCH,
    KF_BLOOM_STRETCH_ANGLE, KF_BLOOM_TINT, KF_BLOOM_ZONE_START, KF_BLOOM_ZONE_FULL,
    KF_HAL_ON, KF_HAL_THRESHOLD, KF_HAL_INTENSITY, KF_HAL_RADIUS, KF_HAL_BLUR, KF_HAL_STRETCH,
    KF_HAL_STRETCH_ANGLE, KF_HAL_TINT, KF_HAL_ZONE_START, KF_HAL_ZONE_FULL,
    KF_GRAIN_ON, KF_GRAIN_AMOUNT, KF_GRAIN_SIZE, KF_GRAIN_ROUGHNESS, KF_GRAIN_COLOR, KF_GRAIN_MIDTONE,
    KF_GRAIN_SEED, KF_GRAIN_ANIMATE, KF_GRAIN_ZONE_START, KF_GRAIN_ZONE_FULL};

static KFGlow glow_from(const PF_ParamDef* const* p, int on, int thr, int inten, int rad, int blur, int stretch, int ang, int tint, int zs, int zf)
{
    KFGlow g;
    g.on = p[on]->u.bd.value != 0;
    g.threshold = (float)p[thr]->u.fs_d.value;
    g.intensity = (float)p[inten]->u.fs_d.value;
    g.radius = (float)p[rad]->u.fs_d.value;
    g.blur = (float)p[blur]->u.fs_d.value;
    g.stretch = (float)p[stretch]->u.fs_d.value;
    g.stretch_angle = (float)(p[ang]->u.ad.value / 65536.0);
    g.tint[0] = p[tint]->u.cd.value.red / 255.f;
    g.tint[1] = p[tint]->u.cd.value.green / 255.f;
    g.tint[2] = p[tint]->u.cd.value.blue / 255.f;
    g.zone.start = (float)p[zs]->u.fs_d.value;
    g.zone.full = (float)p[zf]->u.fs_d.value;
    return g;
}

static KFInfo info_from(const PF_ParamDef* const* p, const PF_InData* in_data)
{
    KFInfo i;
    auto F = [&](int k) { return (float)p[k]->u.fs_d.value; };
    auto B = [&](int k) { return (int)(p[k]->u.bd.value != 0); };
    i.input_space = p[KF_INPUT_SPACE]->u.pd.value;
    i.px = p[KF_ZONE_CENTER]->u.td.x_value / 65536.0;
    i.py = p[KF_ZONE_CENTER]->u.td.y_value / 65536.0;
    i.roundness = F(KF_ZONE_ROUNDNESS);
    i.mix = F(KF_MIX);
    i.show_zones = B(KF_SHOW_ZONES);

    i.lens_on = B(KF_LENS_ON); i.lens_amount = F(KF_LENS_AMOUNT); i.lens_autozoom = B(KF_LENS_AUTOZOOM);
    i.lens_zoom = F(KF_LENS_ZOOM); i.lens_edge = p[KF_LENS_EDGE]->u.pd.value;
    i.lens_zone = {F(KF_LENS_ZONE_START), F(KF_LENS_ZONE_FULL)};

    i.ca_on = B(KF_CA_ON); i.ca_amount = F(KF_CA_AMOUNT); i.ca_blur = F(KF_CA_BLUR);
    i.ca_zone = {F(KF_CA_ZONE_START), F(KF_CA_ZONE_FULL)};

    i.lb_on = B(KF_LB_ON); i.lb_type = p[KF_LB_TYPE]->u.pd.value; i.lb_amount = F(KF_LB_AMOUNT);
    i.lb_angle = (float)(p[KF_LB_ANGLE]->u.ad.value / 65536.0);
    i.lb_zone = {F(KF_LB_ZONE_START), F(KF_LB_ZONE_FULL)};

    i.vig_on = B(KF_VIG_ON); i.vig_amount = F(KF_VIG_AMOUNT); i.vig_softness = F(KF_VIG_SOFTNESS);
    i.vig_zone = {F(KF_VIG_ZONE_START), F(KF_VIG_ZONE_FULL)};

    i.bloom = glow_from(p, KF_BLOOM_ON, KF_BLOOM_THRESHOLD, KF_BLOOM_INTENSITY, KF_BLOOM_RADIUS, KF_BLOOM_BLUR, KF_BLOOM_STRETCH,
                        KF_BLOOM_STRETCH_ANGLE, KF_BLOOM_TINT, KF_BLOOM_ZONE_START, KF_BLOOM_ZONE_FULL);
    i.hal = glow_from(p, KF_HAL_ON, KF_HAL_THRESHOLD, KF_HAL_INTENSITY, KF_HAL_RADIUS, KF_HAL_BLUR, KF_HAL_STRETCH,
                      KF_HAL_STRETCH_ANGLE, KF_HAL_TINT, KF_HAL_ZONE_START, KF_HAL_ZONE_FULL);

    i.grain_on = B(KF_GRAIN_ON); i.g_amount = F(KF_GRAIN_AMOUNT); i.g_size = F(KF_GRAIN_SIZE); i.g_rough = F(KF_GRAIN_ROUGHNESS);
    i.g_color = F(KF_GRAIN_COLOR); i.g_mid = F(KF_GRAIN_MIDTONE); i.g_seed = p[KF_GRAIN_SEED]->u.sd.value; i.g_animate = B(KF_GRAIN_ANIMATE);
    i.g_zone = {F(KF_GRAIN_ZONE_START), F(KF_GRAIN_ZONE_FULL)};
    i.frame = in_data->time_step > 0 ? (int)(in_data->current_time / in_data->time_step) : 0;
    return i;
}

static inline int popup_index(int v, int n) { v -= 1; return v < 0 ? 0 : (v >= n ? n - 1 : v); }

// cw/ch: size of the picture the core works on; ox/oy: where that picture starts in layer coordinates.
// The centre is normalised against the real buffer, not in_data->width (which can differ in SmartFX / at reduced resolution).
static kf::Params make_params(const KFInfo& i, int cw, int ch, double ox, double oy)
{
    kf::Params p;
    // popup is 1-based: sRGB, Rec.709, Linear (the core's enum order differs, so map explicitly)
    p.input_space = (i.input_space == 1) ? kf::InputSpace::sRGB : (i.input_space == 2) ? kf::InputSpace::Rec709 : kf::InputSpace::Linear;
    p.mix = i.mix;
    p.zone_center_x = (float)((i.px - ox) / (cw > 0 ? cw : 1));
    p.zone_center_y = (float)((i.py - oy) / (ch > 0 ? ch : 1));
    p.zone_roundness = i.roundness;
    p.show_zones = i.show_zones != 0;

    p.lens_enable = i.lens_on != 0;
    p.lens_amount = i.lens_amount;
    p.lens_auto_zoom = i.lens_autozoom != 0;
    p.lens_zoom = i.lens_zoom;
    { static const kf::EdgeMode em[3] = {kf::EdgeMode::Transparent, kf::EdgeMode::Clamp, kf::EdgeMode::Mirror};
      p.lens_edge = em[popup_index(i.lens_edge, 3)]; }
    p.lens_zone.start = i.lens_zone.start; p.lens_zone.full = i.lens_zone.full;

    p.ca_enable = i.ca_on != 0;
    p.ca_amount = i.ca_amount;
    p.ca_blur = i.ca_blur;
    p.ca_zone.start = i.ca_zone.start; p.ca_zone.full = i.ca_zone.full;

    p.lblur_enable = i.lb_on != 0;
    { static const kf::LensBlurType lt[4] = {kf::LensBlurType::Soft, kf::LensBlurType::Radial, kf::LensBlurType::Swirl, kf::LensBlurType::Directional};
      p.lblur_type = lt[popup_index(i.lb_type, 4)]; }
    p.lblur_amount = i.lb_amount;
    p.lblur_angle = i.lb_angle;
    p.lblur_zone.start = i.lb_zone.start; p.lblur_zone.full = i.lb_zone.full;

    p.vig_enable = i.vig_on != 0;
    p.vig_amount = i.vig_amount;
    p.vig_softness = i.vig_softness;
    p.vig_zone.start = i.vig_zone.start; p.vig_zone.full = i.vig_zone.full;

    p.bloom_enable = i.bloom.on != 0;
    p.bloom_threshold = i.bloom.threshold; p.bloom_intensity = i.bloom.intensity; p.bloom_radius = i.bloom.radius;
    p.bloom_blur = i.bloom.blur; p.bloom_stretch = i.bloom.stretch; p.bloom_stretch_angle = i.bloom.stretch_angle;
    for (int k = 0; k < 3; ++k) p.bloom_tint[k] = i.bloom.tint[k];
    p.bloom_zone.start = i.bloom.zone.start; p.bloom_zone.full = i.bloom.zone.full;

    p.halation_enable = i.hal.on != 0;
    p.halation_threshold = i.hal.threshold; p.halation_intensity = i.hal.intensity; p.halation_radius = i.hal.radius;
    p.halation_blur = i.hal.blur; p.halation_stretch = i.hal.stretch; p.halation_stretch_angle = i.hal.stretch_angle;
    for (int k = 0; k < 3; ++k) p.halation_tint[k] = i.hal.tint[k];
    p.halation_zone.start = i.hal.zone.start; p.halation_zone.full = i.hal.zone.full;

    p.grain_enable = i.grain_on != 0;
    p.grain_amount = i.g_amount;
    p.grain_size = i.g_size;
    p.grain_roughness = i.g_rough;
    p.grain_color = i.g_color;
    p.grain_midtone_bias = i.g_mid;
    p.grain_seed = i.g_seed;
    p.grain_animate = i.g_animate != 0;
    p.grain_zone.start = i.g_zone.start; p.grain_zone.full = i.g_zone.full;
    p.frame = i.frame;
    return p;
}

static Fmt detect_fmt(PF_InData* in_data, PF_EffectWorld* output)
{
    if (in_data->appl_id == kAppID_Premiere) {
        PF_PixelFormatSuite1* pfs = nullptr;
        if (in_data->pica_basicP->AcquireSuite(kPFPixelFormatSuite, kPFPixelFormatSuiteVersion1, (const void**)&pfs) == 0 && pfs) {
            PrPixelFormat prf = PrPixelFormat_Invalid;
            const PF_Err e = pfs->GetPixelFormat(output, &prf);
            in_data->pica_basicP->ReleaseSuite(kPFPixelFormatSuite, kPFPixelFormatSuiteVersion1);
            if (!e && prf == PrPixelFormat_BGRA_4444_32f) return Fmt::PREM32;
        }
    }
    PF_PixelFormat pf = PF_PixelFormat_ARGB32;
    PF_WorldSuite2* ws = nullptr;
    if (in_data->pica_basicP->AcquireSuite(kPFWorldSuite, kPFWorldSuiteVersion2, (const void**)&ws) == 0 && ws) {
        ws->PF_GetPixelFormat(output, &pf);
        in_data->pica_basicP->ReleaseSuite(kPFWorldSuite, kPFWorldSuiteVersion2);
    } else if (PF_WORLD_IS_DEEP(output)) {
        pf = PF_PixelFormat_ARGB64;
    }
    if (pf == PF_PixelFormat_ARGB128) return Fmt::PF32;
    if (pf == PF_PixelFormat_ARGB64) return Fmt::P16;
    return Fmt::P8;
}

// Core of both render paths. The first cw x ch pixels of `in` are processed as one picture;
// output pixel (x, y) takes core pixel (x + dx, y + dy).
static void run_core(PF_InData* in_data, const PF_EffectWorld* in, PF_EffectWorld* output, int cw, int ch, int dx, int dy, double in_ox, double in_oy, const KFInfo& info)
{
    if (cw <= 0 || ch <= 0) return;
    const Fmt fmt = detect_fmt(in_data, output);

    kf::Buf src(cw, ch, 4), dst;
    switch (fmt) {
        case Fmt::P8:
            to_core<PF_Pixel8>(in, src, [](const PF_Pixel8& p, float& r, float& g, float& b, float& a) {
                a = p.alpha / 255.f; r = p.red / 255.f; g = p.green / 255.f; b = p.blue / 255.f; });
            break;
        case Fmt::P16:
            to_core<PF_Pixel16>(in, src, [](const PF_Pixel16& p, float& r, float& g, float& b, float& a) {
                const float s = 1.f / 32768.f;
                a = p.alpha * s; r = p.red * s; g = p.green * s; b = p.blue * s; });
            break;
        case Fmt::PF32:
            to_core<PF_PixelFloat>(in, src, [](const PF_PixelFloat& p, float& r, float& g, float& b, float& a) {
                a = p.alpha; r = p.red; g = p.green; b = p.blue; });
            break;
        case Fmt::PREM32:
            to_core<KFPremPixel>(in, src, [](const KFPremPixel& p, float& r, float& g, float& b, float& a) {
                a = p.a; r = p.r; g = p.g; b = p.b; });
            break;
    }

    kf::process(src, dst, make_params(info, cw, ch, in_ox, in_oy));

    switch (fmt) {
        case Fmt::P8:
            from_core<PF_Pixel8>(dst, output, dx, dy, [](PF_Pixel8& o, float r, float g, float b, float a) {
                o.alpha = (A_u_char)(clamp01(a) * 255.f + 0.5f); o.red = (A_u_char)(clamp01(r) * 255.f + 0.5f);
                o.green = (A_u_char)(clamp01(g) * 255.f + 0.5f); o.blue = (A_u_char)(clamp01(b) * 255.f + 0.5f); });
            break;
        case Fmt::P16:
            from_core<PF_Pixel16>(dst, output, dx, dy, [](PF_Pixel16& o, float r, float g, float b, float a) {
                o.alpha = (A_u_short)(clamp01(a) * 32768.f + 0.5f); o.red = (A_u_short)(clamp01(r) * 32768.f + 0.5f);
                o.green = (A_u_short)(clamp01(g) * 32768.f + 0.5f); o.blue = (A_u_short)(clamp01(b) * 32768.f + 0.5f); });
            break;
        case Fmt::PF32:
            from_core<PF_PixelFloat>(dst, output, dx, dy, [](PF_PixelFloat& o, float r, float g, float b, float a) {
                o.alpha = a; o.red = r; o.green = g; o.blue = b; });
            break;
        case Fmt::PREM32:
            from_core<KFPremPixel>(dst, output, dx, dy, [](KFPremPixel& o, float r, float g, float b, float a) {
                o.a = a; o.r = r; o.g = g; o.b = b; });
            break;
    }
}

// ---- classic render path (PF_Cmd_RENDER) ----
static PF_Err Render(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], PF_LayerDef* output)
{
    PF_EffectWorld* in = &params[KF_INPUT]->u.ld;
    const int w = std::min<int>(in->width, output->width), h = std::min<int>(in->height, output->height);
    if (w <= 0 || h <= 0) return PF_Err_NONE;
    const PF_ParamDef* ptrs[KF_NUM_PARAMS];
    for (int i = 0; i < KF_NUM_PARAMS; ++i) ptrs[i] = params[i];
    const KFInfo info = info_from(ptrs, in_data);
    run_core(in_data, in, output, w, h, 0, 0, 0.0, 0.0, info);
    return PF_Err_NONE;
}

// ---- SmartFX path (PF_Cmd_SMART_PRE_RENDER / PF_Cmd_SMART_RENDER) ----
// size at the current resolution (rounded up, like AE does); a missing / zero scale means 1:1
static int scaled_size(int v, const PF_RationalScale& sc)
{
    if (sc.num <= 0 || sc.den == 0) return v;
    return (int)(((long long)v * sc.num + sc.den - 1) / (long long)sc.den);
}

static void delete_info(void* p) { delete static_cast<KFInfo*>(p); }

static PF_Err PreRender(PF_InData* in_data, PF_OutData* out_data, PF_PreRenderExtra* ex)
{
    PF_Err err = PF_Err_NONE, err2 = PF_Err_NONE;

    // Check out every parameter here (not in render) so AE tracks them for caching.
    PF_ParamDef chk[KF_NUM_PARAMS];
    const PF_ParamDef* ptrs[KF_NUM_PARAMS];
    bool got[KF_NUM_PARAMS];
    for (int i = 0; i < KF_NUM_PARAMS; ++i) { AEFX_CLR_STRUCT(chk[i]); ptrs[i] = &chk[i]; got[i] = false; }
    for (int idx : kReadParams) {
        ERR(PF_CHECKOUT_PARAM(in_data, idx, in_data->current_time, in_data->time_step, in_data->time_scale, &chk[idx]));
        if (!err) got[idx] = true;
    }
    if (!err) {
        KFInfo* info = new (std::nothrow) KFInfo(info_from(ptrs, in_data));
        if (!info) {
            err = PF_Err_OUT_OF_MEMORY;
        } else {
            ex->output->pre_render_data = info;
            ex->output->delete_pre_render_data_func = delete_info;
        }
    }
    for (int idx : kReadParams) if (got[idx]) { ERR2(PF_CHECKIN_PARAM(in_data, &chk[idx])); }

    // GPU rendering is offered only when AE has picked Metal for this render (otherwise AE uses the CPU path)
    if (ex->input->what_gpu == PF_GPU_Framework_METAL) ex->output->flags |= PF_RenderOutputFlag_GPU_RENDER_POSSIBLE;

    if (!err) {
        // Zones, blur and grain need the whole picture, so always ask for the whole layer.
        // in_data->width/height are the FULL-resolution layer size, but the request rectangle is in the coordinates of
        // the current (reduced) resolution; asking for the full-res size at half resolution makes AE hand back a
        // padded, oversized buffer (that was the half-resolution bug).
        PF_RenderRequest req = ex->input->output_request;
        req.rect.left = 0; req.rect.top = 0;
        req.rect.right = scaled_size(in_data->width, in_data->downsample_x);
        req.rect.bottom = scaled_size(in_data->height, in_data->downsample_y);
        PF_CheckoutResult in_result;
        AEFX_CLR_STRUCT(in_result);
        ERR(ex->cb->checkout_layer(in_data->effect_ref, KF_INPUT, KF_INPUT, &req,
                                   in_data->current_time, in_data->time_step, in_data->time_scale, &in_result));
        if (!err) {
            ex->output->result_rect = in_result.result_rect;
            ex->output->max_result_rect = in_result.max_result_rect;
            ex->output->solid = FALSE;
            ex->output->flags |= PF_RenderOutputFlag_RETURNS_EXTRA_PIXELS;
        }
    }
    return err;
}

static PF_Err SmartRender(PF_InData* in_data, PF_OutData* out_data, PF_SmartRenderExtra* ex)
{
    PF_Err err = PF_Err_NONE, err2 = PF_Err_NONE;
    PF_EffectWorld* in_w = nullptr;
    PF_EffectWorld* out_w = nullptr;
    const KFInfo* info = static_cast<const KFInfo*>(ex->input->pre_render_data);
    if (!info) return PF_Err_INTERNAL_STRUCT_DAMAGED;

    ERR(ex->cb->checkout_layer_pixels(in_data->effect_ref, KF_INPUT, &in_w));
    ERR(ex->cb->checkout_output(in_data->effect_ref, &out_w));
    if (!err && in_w && out_w) {
        // origin_x/y: where each buffer sits in layer coordinates
        const int dx = (int)out_w->origin_x - (int)in_w->origin_x;
        const int dy = (int)out_w->origin_y - (int)in_w->origin_y;
        run_core(in_data, in_w, out_w, in_w->width, in_w->height, dx, dy, (double)in_w->origin_x, (double)in_w->origin_y, *info);
    }
    ERR2(ex->cb->checkin_layer_pixels(in_data->effect_ref, KF_INPUT));
    return err;
}


// ---- GPU path (After Effects, Metal): GPU worlds are float4 B,G,R,A, premultiplied ----
static inline void gpu_pix_to_core(const float* p, float* o)
{
    const float a = p[3];
    float r = p[2], g = p[1], b = p[0];
    if (a > 0.f && a < 1.f) { r /= a; g /= a; b /= a; }
    o[0] = r; o[1] = g; o[2] = b; o[3] = a;
}
static inline void core_to_gpu_pix(const float* c, float* p)
{
    const float a = c[3];
    p[0] = c[2] * a; p[1] = c[1] * a; p[2] = c[0] * a; p[3] = a;
}

// Everything is on the GPU now, except the burned-in Show Zones rings (a preview aid): those still use the CPU stage.
static bool gpu_full_ok(const KFInfo& i)
{
    return !i.show_zones;
}

static KFGpuFrameParams make_frame_params(const KFInfo& i, int w, int h, double ox, double oy, std::vector<KFGpuPass>* passes = nullptr)
{
    return kf_make_frame_params(make_params(i, w, h, ox, oy), w, h, passes);
}

struct KFGpuCtx { const KFInfo* info; int dx, dy; double ox, oy; };

static void gpu_cpu_stage(const float* src, int sw, int sh, int sp, float* dst, int dw, int dh, int dp, void* user)
{
    const KFGpuCtx* c = static_cast<const KFGpuCtx*>(user);
    kf::Buf in(sw, sh, 4), out;
    for (int y = 0; y < sh; ++y)
        for (int x = 0; x < sw; ++x) gpu_pix_to_core(src + ((size_t)y * sp + x) * 4, in.px(x, y));
    kf::process(in, out, make_params(*c->info, sw, sh, c->ox, c->oy));
    for (int y = 0; y < dh; ++y)
        for (int x = 0; x < dw; ++x) {
            float* o = dst + ((size_t)y * dp + x) * 4;
            const int sx = x + c->dx, sy = y + c->dy;
            if (sx >= 0 && sx < out.w && sy >= 0 && sy < out.h) core_to_gpu_pix(out.px(sx, sy), o);
            else { o[0] = o[1] = o[2] = o[3] = 0.f; }
        }
}

static PF_Err SmartRenderGPU(PF_InData* in_data, PF_OutData* out_data, PF_SmartRenderExtra* ex)
{
    PF_Err err = PF_Err_NONE, err2 = PF_Err_NONE;
    PF_EffectWorld* in_w = nullptr;
    PF_EffectWorld* out_w = nullptr;
    const KFInfo* info = static_cast<const KFInfo*>(ex->input->pre_render_data);
    if (!info) return PF_Err_INTERNAL_STRUCT_DAMAGED;

    ERR(ex->cb->checkout_layer_pixels(in_data->effect_ref, KF_INPUT, &in_w));
    ERR(ex->cb->checkout_output(in_data->effect_ref, &out_w));
    if (!err && in_w && out_w) {
        PF_PixelFormat pf = PF_PixelFormat_INVALID;
        PF_WorldSuite2* ws = nullptr;
        if (in_data->pica_basicP->AcquireSuite(kPFWorldSuite, kPFWorldSuiteVersion2, (const void**)&ws) == 0 && ws) {
            ws->PF_GetPixelFormat(in_w, &pf);
            in_data->pica_basicP->ReleaseSuite(kPFWorldSuite, kPFWorldSuiteVersion2);
        }
        if (pf != PF_PixelFormat_GPU_BGRA128) {
            err = PF_Err_UNRECOGNIZED_PARAM_TYPE;
        } else {
            KFGpuCtx ctx;
            ctx.info = info;
            ctx.dx = (int)out_w->origin_x - (int)in_w->origin_x;
            ctx.dy = (int)out_w->origin_y - (int)in_w->origin_y;
            ctx.ox = (double)in_w->origin_x; ctx.oy = (double)in_w->origin_y;
            const bool same_area = ctx.dx == 0 && ctx.dy == 0 && in_w->width == out_w->width && in_w->height == out_w->height;
            bool done = false;
            if (same_area && gpu_full_ok(*info)) {
                std::vector<KFGpuPass> passes;
                KFGpuFrameParams fp = make_frame_params(*info, in_w->width, in_w->height, ctx.ox, ctx.oy, &passes);
                const kf::Params kp = make_params(*info, in_w->width, in_w->height, ctx.ox, ctx.oy);
                if (!kf_heavy_needed(kp)) {
                    err = kf_gpu_render_frame(in_data, out_data, ex, in_w, out_w, fp, passes);
                    done = true;
                } else {
                    // Lens, CA, Lens Blur, Bloom, Halation: a list of kernel launches; falls back to the CPU stage if a kernel group is missing
                    KFPlan plan;
                    if (kf_build_heavy_plan(kp, in_w->width, in_w->height, kf_gpu_groups(ex), plan)) {
                        fp.applyVig = 0;                       // vignette is part of the plan in this mode
                        fp.hasCur = 1;
                        err = kf_gpu_render_frame(in_data, out_data, ex, in_w, out_w, fp, passes, &plan);
                        done = true;
                    }
                }
            }
            if (!done) err = kf_gpu_render(in_data, out_data, ex, in_w, out_w, gpu_cpu_stage, &ctx);
        }
    }
    ERR2(ex->cb->checkin_layer_pixels(in_data->effect_ref, KF_INPUT));
    return err;
}


// ---- Premiere GPU bridge: the same KFInfo, built from the values Premiere hands to a GPU filter (no PF_ParamDef there) ----
static bool kf_is_group_marker(int k)
{
    static const int m[] = {KF_ZONES_START, KF_ZONES_END, KF_LENS_START, KF_LENS_END, KF_CA_START, KF_CA_END, KF_LB_START, KF_LB_END,
                            KF_VIG_START, KF_VIG_END, KF_BLOOM_START, KF_BLOOM_END, KF_HAL_START, KF_HAL_END, KF_GRAIN_START, KF_GRAIN_END};
    for (int x : m) if (x == k) return true;
    return false;
}

// Premiere order -> After Effects parameter index. Two layouts are understood: with the group start/end slots or without them.
static bool kf_prem_map(const KFPremValue* v, int count, KFPremValue* av)
{
    for (int k = 0; k < KF_NUM_PARAMS; ++k) av[k].type = 0;
    const int with_groups = KF_NUM_PARAMS - 1;
    int n_markers = 0; for (int k = 1; k < KF_NUM_PARAMS; ++k) if (kf_is_group_marker(k)) ++n_markers;
    if (count == with_groups) { for (int k = 1; k < KF_NUM_PARAMS; ++k) av[k] = v[k - 1]; return true; }
    if (count == with_groups - n_markers) {
        int j = 0;
        for (int k = 1; k < KF_NUM_PARAMS; ++k) if (!kf_is_group_marker(k)) av[k] = v[j++];
        return true;
    }
    return false;
}

struct KFPremReader {
    const KFPremValue* av; bool ok = true;
    double num(int k) { if (av[k].type != 1 && av[k].type != 3) { ok = false; return 0; } return av[k].num; }   // 1 float/bool, 3 integer (also carries .raw)
    float f(int k) { return (float)num(k); }
    int b(int k) { return num(k) != 0.0; }
    int pop(int k) { return (int)std::lround(num(k)); }                 // 1-based like the After Effects popup
    float angle(int k) { double d = num(k); return (float)(std::fabs(d) > 100000.0 ? d / 65536.0 : d); }   // degrees (fixed-point would be huge)
    void colour(int k, float* o) {                                      // raw integer 0xAARRGGBB
        if (av[k].type != 3) { ok = false; o[0] = o[1] = o[2] = 1.f; return; }
        const unsigned long long c = av[k].raw;
        o[0] = ((c >> 16) & 255) / 255.f; o[1] = ((c >> 8) & 255) / 255.f; o[2] = (c & 255) / 255.f;
    }
};

static bool kf_info_from_values(const KFPremValue* av, int w, int h, int frame, KFInfo& i)
{
    KFPremReader R{av};
    auto glow = [&](int on, int thr, int inten, int rad, int blur, int stretch, int ang, int tint, int zs, int zf) {
        KFGlow g;
        g.on = R.b(on); g.threshold = R.f(thr); g.intensity = R.f(inten); g.radius = R.f(rad); g.blur = R.f(blur);
        g.stretch = R.f(stretch); g.stretch_angle = R.angle(ang); R.colour(tint, g.tint);
        g.zone.start = R.f(zs); g.zone.full = R.f(zf);
        return g;
    };
    i.input_space = R.pop(KF_INPUT_SPACE);
    if (av[KF_ZONE_CENTER].type != 2) R.ok = false;
    i.px = av[KF_ZONE_CENTER].x * w; i.py = av[KF_ZONE_CENTER].y * h;      // Premiere points are 0..1
    i.roundness = R.f(KF_ZONE_ROUNDNESS); i.mix = R.f(KF_MIX); i.show_zones = R.b(KF_SHOW_ZONES);
    i.lens_on = R.b(KF_LENS_ON); i.lens_amount = R.f(KF_LENS_AMOUNT); i.lens_autozoom = R.b(KF_LENS_AUTOZOOM);
    i.lens_zoom = R.f(KF_LENS_ZOOM); i.lens_edge = R.pop(KF_LENS_EDGE); i.lens_zone = {R.f(KF_LENS_ZONE_START), R.f(KF_LENS_ZONE_FULL)};
    i.ca_on = R.b(KF_CA_ON); i.ca_amount = R.f(KF_CA_AMOUNT); i.ca_blur = R.f(KF_CA_BLUR); i.ca_zone = {R.f(KF_CA_ZONE_START), R.f(KF_CA_ZONE_FULL)};
    i.lb_on = R.b(KF_LB_ON); i.lb_type = R.pop(KF_LB_TYPE); i.lb_amount = R.f(KF_LB_AMOUNT); i.lb_angle = R.angle(KF_LB_ANGLE);
    i.lb_zone = {R.f(KF_LB_ZONE_START), R.f(KF_LB_ZONE_FULL)};
    i.vig_on = R.b(KF_VIG_ON); i.vig_amount = R.f(KF_VIG_AMOUNT); i.vig_softness = R.f(KF_VIG_SOFTNESS);
    i.vig_zone = {R.f(KF_VIG_ZONE_START), R.f(KF_VIG_ZONE_FULL)};
    i.bloom = glow(KF_BLOOM_ON, KF_BLOOM_THRESHOLD, KF_BLOOM_INTENSITY, KF_BLOOM_RADIUS, KF_BLOOM_BLUR, KF_BLOOM_STRETCH,
                   KF_BLOOM_STRETCH_ANGLE, KF_BLOOM_TINT, KF_BLOOM_ZONE_START, KF_BLOOM_ZONE_FULL);
    i.hal = glow(KF_HAL_ON, KF_HAL_THRESHOLD, KF_HAL_INTENSITY, KF_HAL_RADIUS, KF_HAL_BLUR, KF_HAL_STRETCH,
                 KF_HAL_STRETCH_ANGLE, KF_HAL_TINT, KF_HAL_ZONE_START, KF_HAL_ZONE_FULL);
    i.grain_on = R.b(KF_GRAIN_ON); i.g_amount = R.f(KF_GRAIN_AMOUNT); i.g_size = R.f(KF_GRAIN_SIZE); i.g_rough = R.f(KF_GRAIN_ROUGHNESS);
    i.g_color = R.f(KF_GRAIN_COLOR); i.g_mid = R.f(KF_GRAIN_MIDTONE); i.g_seed = (int)std::lround(R.num(KF_GRAIN_SEED)); i.g_animate = R.b(KF_GRAIN_ANIMATE);
    i.g_zone = {R.f(KF_GRAIN_ZONE_START), R.f(KF_GRAIN_ZONE_FULL)};
    i.frame = frame;
    return R.ok;
}

bool kf_prem_prepare(const KFPremValue* v, int count, int w, int h, int frame, unsigned groups,
                     KFGpuFrameParams& fp, std::vector<KFGpuPass>& passes, KFPlan& plan, bool& use_plan)
{
    use_plan = false;
    if (!v || w <= 0 || h <= 0) return false;
    KFPremValue av[KF_NUM_PARAMS];
    if (!kf_prem_map(v, count, av)) return false;
    KFInfo info;
    if (!kf_info_from_values(av, w, h, frame, info)) return false;
    if (!gpu_full_ok(info)) return false;                                   // Show Zones burns rings in on the CPU path
    fp = make_frame_params(info, w, h, 0.0, 0.0, &passes);
    const kf::Params kp = make_params(info, w, h, 0.0, 0.0);
    if (!kf_heavy_needed(kp)) return true;
    if (!kf_build_heavy_plan(kp, w, h, groups, plan)) return false;         // a kernel group is missing: CPU path
    fp.applyVig = 0; fp.hasCur = 1;
    use_plan = true;
    return true;
}

// ---- zone overlay in the comp window (After Effects only; Premiere has no comp-window overlay, use the Show Zones checkbox) ----
// Geometry mirrors the core: d = lerp(oval, circle, roundness); along direction (cos t, sin t) the distance grows linearly
// with the radius, so the point on the ring of level L is simply r = L / k(t).
struct KFRingGeom { double cx, cy, hw, hh, round; };

static void kf_ring_point(const KFRingGeom& g, double theta, double level, double* x, double* y)
{
    const double c = std::cos(theta), s = std::sin(theta);
    const double Ra = std::hypot(g.hw, g.hh);
    const double k_oval = std::hypot(c / g.hw, s / g.hh) * 0.70710678118654752;
    const double k = k_oval * (1.0 - g.round) + (1.0 / Ra) * g.round;
    const double r = level / (k > 1e-12 ? k : 1e-12);
    *x = g.cx + r * c;
    *y = g.cy + r * s;
}

struct KFDrawbot {
    DRAWBOT_DrawbotSuiteCurrent* drawbot_suiteP;
    DRAWBOT_SupplierSuiteCurrent* supplier_suiteP;
    DRAWBOT_SurfaceSuiteCurrent* surface_suiteP;
    DRAWBOT_PathSuiteCurrent* path_suiteP;
};

struct KFOverlayRing { bool active; KFZone z; float r, g, b; };

// Same colours and "is this effect doing anything" rules as the burned-in rings in the core.
static int kf_overlay_rings(const KFInfo& i, KFOverlayRing out[7])
{
    const KFOverlayRing specs[7] = {
        {i.lens_on && i.lens_amount != 0.f, i.lens_zone, 0.f, 0.9f, 1.f},
        {i.ca_on && i.ca_amount != 0.f, i.ca_zone, 1.f, 0.2f, 0.9f},
        {i.lb_on && i.lb_amount > 0.f, i.lb_zone, 1.f, 0.6f, 0.f},
        {i.vig_on && i.vig_amount != 0.f, i.vig_zone, 1.f, 1.f, 0.2f},
        {i.bloom.on && i.bloom.intensity > 0.f, i.bloom.zone, 0.5f, 0.75f, 1.f},
        {i.hal.on && i.hal.intensity > 0.f, i.hal.zone, 1.f, 0.2f, 0.2f},
        {i.grain_on && i.g_amount > 0.f, i.g_zone, 0.3f, 1.f, 0.3f},
    };
    int n = 0;
    for (const auto& s : specs)
        if (s.active && !(s.z.start <= 0.f && s.z.full <= 0.f)) out[n++] = s;
    return n;
}

static const int kRingSegments = 96;

// Draws one ring as a polyline in frame (screen) coordinates; dashed = every second group of segments is skipped.
static PF_Err kf_stroke_ring(PF_InData* in_data, PF_EventExtra* ev, KFDrawbot& dbs, DRAWBOT_SupplierRef supplier,
                             DRAWBOT_SurfaceRef surface, const KFRingGeom& g, double level, bool dashed,
                             const DRAWBOT_ColorRGBA& col, float width)
{
    PF_Err err = PF_Err_NONE;
    DRAWBOT_PenP pen(dbs.supplier_suiteP, supplier, &col, width);
    DRAWBOT_PathP path(dbs.supplier_suiteP, supplier);
    const bool comp = (PF_Window_COMP == (*ev->contextH)->w_type);
    bool pen_down = false;
    for (int k = 0; k <= kRingSegments; ++k) {
        const bool visible = !dashed || ((k / 2) % 2 == 0) || k == kRingSegments;
        double x, y;
        kf_ring_point(g, 2.0 * 3.14159265358979323846 * k / kRingSegments, level, &x, &y);
        PF_FixedPoint fp;
        fp.x = (PF_Fixed)(x * 65536.0);
        fp.y = (PF_Fixed)(y * 65536.0);
        if (comp) ev->cbs.layer_to_comp(ev->cbs.refcon, ev->contextH, in_data->current_time, in_data->time_scale, &fp);
        ev->cbs.source_to_frame(ev->cbs.refcon, ev->contextH, &fp);
        const float fx = (float)(fp.x / 65536.0), fy = (float)(fp.y / 65536.0);
        if (!visible) { pen_down = false; continue; }
        if (!pen_down) { ERR(dbs.path_suiteP->MoveTo(path, fx, fy)); pen_down = true; }
        else { ERR(dbs.path_suiteP->LineTo(path, fx, fy)); }
    }
    ERR(dbs.surface_suiteP->StrokePath(surface, pen, path));
    return err;
}

static PF_Err DrawOverlay(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], PF_EventExtra* ev)
{
    PF_Err err = PF_Err_NONE;
    const int wt = (*ev->contextH)->w_type;
    if (wt != PF_Window_COMP && wt != PF_Window_LAYER) return err;
    if (in_data->width <= 0 || in_data->height <= 0) return err;

    const KFInfo info = info_from(params, in_data);
    KFOverlayRing rings[7];
    const int n = kf_overlay_rings(info, rings);
    if (n == 0) return err;                       // nothing to show: no zones set

    KFRingGeom g;
    g.cx = info.px; g.cy = info.py;
    g.hw = std::max(0.5 * in_data->width, 1.0); g.hh = std::max(0.5 * in_data->height, 1.0);
    g.round = std::min(1.0, std::max(0.0, (double)info.roundness));

    AEGP_SuiteHandler suites(in_data->pica_basicP);
    DRAWBOT_DrawRef drawing_ref = NULL;
    ERR(suites.EffectCustomUISuite1()->PF_GetDrawingReference(ev->contextH, &drawing_ref));
    if (err || !drawing_ref) return err;

    KFDrawbot dbs;
    dbs.drawbot_suiteP = suites.DrawbotSuiteCurrent();
    dbs.supplier_suiteP = suites.SupplierSuiteCurrent();
    dbs.surface_suiteP = suites.SurfaceSuiteCurrent();
    dbs.path_suiteP = suites.PathSuiteCurrent();
    DRAWBOT_SupplierRef supplier = NULL;
    DRAWBOT_SurfaceRef surface = NULL;
    ERR(dbs.drawbot_suiteP->GetSupplier(drawing_ref, &supplier));
    ERR(dbs.drawbot_suiteP->GetSurface(drawing_ref, &surface));
    if (!err) {
        try {
            for (int k = 0; k < n; ++k) {
                DRAWBOT_ColorRGBA col; col.red = rings[k].r; col.green = rings[k].g; col.blue = rings[k].b; col.alpha = 0.95f;
                const float s = rings[k].z.start, f = std::max(rings[k].z.full, rings[k].z.start);
                if (s > 0.f) ERR(kf_stroke_ring(in_data, ev, dbs, supplier, surface, g, s, true, col, 1.5f));
                if (f > 0.f) ERR(kf_stroke_ring(in_data, ev, dbs, supplier, surface, g, f, false, col, 1.5f));
            }
            // centre cross (white)
            DRAWBOT_ColorRGBA white; white.red = white.green = white.blue = white.alpha = 1.f;
            const double arm = 0.012 * std::hypot((double)in_data->width, (double)in_data->height);
            DRAWBOT_PenP pen(dbs.supplier_suiteP, supplier, &white, 1.5f);
            DRAWBOT_PathP path(dbs.supplier_suiteP, supplier);
            const bool comp = (wt == PF_Window_COMP);
            const double pts[4][2] = {{g.cx - arm, g.cy}, {g.cx + arm, g.cy}, {g.cx, g.cy - arm}, {g.cx, g.cy + arm}};
            for (int k = 0; k < 4; ++k) {
                PF_FixedPoint fp; fp.x = (PF_Fixed)(pts[k][0] * 65536.0); fp.y = (PF_Fixed)(pts[k][1] * 65536.0);
                if (comp) ev->cbs.layer_to_comp(ev->cbs.refcon, ev->contextH, in_data->current_time, in_data->time_scale, &fp);
                ev->cbs.source_to_frame(ev->cbs.refcon, ev->contextH, &fp);
                if (k % 2 == 0) ERR(dbs.path_suiteP->MoveTo(path, (float)(fp.x / 65536.0), (float)(fp.y / 65536.0)));
                else ERR(dbs.path_suiteP->LineTo(path, (float)(fp.x / 65536.0), (float)(fp.y / 65536.0)));
            }
            ERR(dbs.surface_suiteP->StrokePath(surface, pen, path));
        } catch (...) { err = PF_Err_INTERNAL_STRUCT_DAMAGED; }
    }
    return err;
}

static PF_Err HandleEvent(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], PF_LayerDef* output, PF_EventExtra* ev)
{
    PF_Err err = PF_Err_NONE;
    if (!ev) return err;
    if (ev->e_type == PF_Event_DRAW && in_data->appl_id != kAppID_Premiere) {
        err = DrawOverlay(in_data, out_data, params, ev);
        ev->evt_out_flags = PF_EO_HANDLED_EVENT;
    }
    return err;
}

extern "C" DllExport PF_Err PluginDataEntryFunction2(
    PF_PluginDataPtr inPtr, PF_PluginDataCB2 inPluginDataCallBackPtr, SPBasicSuite* inSPBasicSuitePtr,
    const char* inHostName, const char* inHostVersion)
{
    PF_Err result = PF_Err_INVALID_CALLBACK;
    result = PF_REGISTER_EFFECT_EXT2(
        inPtr, inPluginDataCallBackPtr,
        "Kaptains Film Look",          // Name
        "KAPTAIN KaptainsFilmLook",   // Match name (unique, never change)
        "Kaptain",               // Category
        AE_RESERVED_INFO,
        "EffectMain",
        "https://kaptainen.dk");
    return result;
}

PF_Err EffectMain(PF_Cmd cmd, PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], PF_LayerDef* output, void* extra)
{
    PF_Err err = PF_Err_NONE;
    try {
        switch (cmd) {
            case PF_Cmd_ABOUT: err = About(in_data, out_data, params, output); break;
            case PF_Cmd_GLOBAL_SETUP: err = GlobalSetup(in_data, out_data, params, output); break;
            case PF_Cmd_PARAMS_SETUP: err = ParamsSetup(in_data, out_data, params, output); break;
            case PF_Cmd_RENDER: err = Render(in_data, out_data, params, output); break;
            case PF_Cmd_SMART_PRE_RENDER: err = PreRender(in_data, out_data, (PF_PreRenderExtra*)extra); break;
            case PF_Cmd_EVENT: err = HandleEvent(in_data, out_data, params, output, (PF_EventExtra*)extra); break;
            case PF_Cmd_SMART_RENDER: err = SmartRender(in_data, out_data, (PF_SmartRenderExtra*)extra); break;
            case PF_Cmd_SMART_RENDER_GPU: err = SmartRenderGPU(in_data, out_data, (PF_SmartRenderExtra*)extra); break;
            case PF_Cmd_GPU_DEVICE_SETUP: err = kf_gpu_device_setup(in_data, out_data, (PF_GPUDeviceSetupExtra*)extra); break;
            case PF_Cmd_GPU_DEVICE_SETDOWN: err = kf_gpu_device_setdown(in_data, out_data, (PF_GPUDeviceSetdownExtra*)extra); break;
        }
    } catch (PF_Err& thrown_err) {
        err = thrown_err;
    } catch (...) {
        err = PF_Err_INTERNAL_STRUCT_DAMAGED;
    }
    return err;
}
