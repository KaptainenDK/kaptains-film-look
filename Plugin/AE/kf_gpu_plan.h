// Kaptains Film Look - Copyright (C) 2026 Henrik Jonas Kaptain
// SPDX-License-Identifier: GPL-3.0-or-later (with an additional permission for the Adobe SDKs, see LICENSE-EXCEPTION)
// Kaptains Film Look - plan for the GPU grain noise (pure C++, no Adobe headers except via KaptainFilmGPU.h).
// Mirrors grain() / noise_field() in kf_core.cpp. Needs kf_core.cpp included before this file (it uses kf::gauss_kernel).
#pragma once
#include "KaptainFilmGPU.h"
#include <cmath>
#include <cstring>
#include <algorithm>

struct KFGrainField { float sigma; unsigned int seed; float weight; };
struct KFGrainPlan { int layers = 0; int nfields[4] = {0, 0, 0, 0}; KFGrainField f[4][2]; };

// Fills the grain fields of the frame params and the plan. Returns false when grain does nothing.
static inline bool kf_make_grain_plan(const kf::Params& p, int w, int h, KFGpuFrameParams& v, KFGrainPlan& plan)
{
    plan = KFGrainPlan();
    v.applyGrain = 0;
    if (!p.grain_enable || p.grain_amount == 0.f) return false;
    const float diag = std::hypot((float)w, (float)h);
    const float size_px = std::max(p.grain_size, 0.05f) * diag / 1600.f;
    const float sig_f = 0.45f * size_px, sig_c = sig_f * 2.5f;
    const float rough = std::min(std::max(p.grain_roughness, 0.f), 1.f);
    const float col = std::min(std::max(p.grain_color, 0.f), 1.f);
    const float bias = std::min(std::max(p.grain_midtone_bias, 0.f), 1.f);
    const unsigned int base_seed = (unsigned int)p.grain_seed * 7919u + (p.grain_animate ? (unsigned int)p.frame * 104729u : 0u);

    v.applyGrain = 1;
    v.gEverywhere = (p.grain_zone.start <= 0.f && p.grain_zone.full <= 0.f) ? 1 : 0;
    v.gZoneStart = p.grain_zone.start; v.gZoneFull = p.grain_zone.full;
    v.gAmp0 = p.grain_amount * 0.05f; v.gBias = bias;
    v.gColor = col > 0.001f ? 1 : 0;
    v.gWs = std::sqrt(1.f - col); v.gWi = std::sqrt(col);

    plan.layers = v.gColor ? 4 : 1;
    for (int L = 0; L < plan.layers; ++L) {
        const unsigned int id = (unsigned int)L;                      // layered(id): shared = 0, independent = 1 + channel
        KFGrainField* f = plan.f[L];
        if (rough > 0.001f) {
            f[0] = {sig_f, base_seed * 31u + id * 2u, std::sqrt(1.f - rough)};
            f[1] = {sig_c, base_seed * 31u + id * 2u + 1u, std::sqrt(rough)};
            plan.nfields[L] = 2;
        } else {
            f[0] = {sig_f, base_seed * 31u + id * 2u, 1.f};
            plan.nfields[L] = 1;
        }
    }
    return true;
}

static inline float kf_clamp01f(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }

// Expands the plan into kernel passes (direct noise, or horizontal + vertical blur), accumulating the fields of one layer.
static inline void kf_build_grain_passes(const KFGrainPlan& plan, int w, int h, std::vector<KFGpuPass>& out)
{
    out.clear();
    for (int L = 0; L < plan.layers; ++L)
        for (int k = 0; k < plan.nfields[L]; ++k) {
            const KFGrainField& f = plan.f[L][k];
            KFGpuNoiseParams np; memset(&np, 0, sizeof np);
            np.width = (unsigned)w; np.height = (unsigned)h; np.seed = f.seed; np.weight = f.weight; np.accum = k > 0 ? 1 : 0;
            if (f.sigma < 0.05f) {
                KFGpuPass d; d.kernel = 0; d.layer = L; d.hb_rows = 0; d.np = np; np.norm = 1.f; d.np.norm = 1.f;
                out.push_back(d);
                continue;
            }
            int r; std::vector<float> kw = kf::gauss_kernel(f.sigma, r);
            float sumsq = 0.f; for (float x : kw) sumsq += x * x;
            np.radius = (unsigned)r; np.norm = 1.f / sumsq;
            KFGpuPass a; a.kernel = 1; a.layer = L; a.hb_rows = h + 2 * r; a.np = np; a.kw = kw;
            KFGpuPass b = a; b.kernel = 2;
            out.push_back(a); out.push_back(b);
        }
}

// ============================================================================================================
// Step 3: the heavy effects as a list of kernel launches. Mirrors kf::process() in kf_core.cpp, stage by stage.
// ============================================================================================================
struct KFPlanBuilder {
    KFPlan& pl;
    std::vector<int> pool;                      // released working buffers, ready for reuse
    explicit KFPlanBuilder(KFPlan& p) : pl(p) { pl.bufs.assign(2, KFBufDim{0, 0}); }

    int alloc(int w, int h) {
        for (size_t i = 0; i < pool.size(); ++i) {
            const int id = pool[i];
            if (pl.bufs[id].w == w && pl.bufs[id].h == h) { pool.erase(pool.begin() + i); return id; }
        }
        pl.bufs.push_back(KFBufDim{w, h});
        return (int)pl.bufs.size() - 1;
    }
    void release(int id, int keep = -1) { if (id >= 2 && id != keep) pool.push_back(id); }
    KFOp& op(int kernel, int a, int b, int c, int d, int out) {
        pl.ops.emplace_back();
        KFOp& o = pl.ops.back();
        o.kernel = kernel; o.in[0] = a; o.in[1] = b; o.in[2] = c; o.in[3] = d; o.out = out;
        memset(&o.P, 0, sizeof o.P);
        o.P.w = o.gw = (unsigned)pl.bufs[out].w; o.P.h = o.gh = (unsigned)pl.bufs[out].h;
        if (a >= 2) { o.P.sw = (unsigned)pl.bufs[a].w; o.P.sh = (unsigned)pl.bufs[a].h; }
        if (kernel <= KF_K_CAADD) pl.need |= KF_GRP_GEO;
        else if (kernel <= KF_K_DIR) pl.need |= KF_GRP_BLUR;
        else pl.need |= KF_GRP_EFF;
        return o;
    }

    // separable gaussian (clamp edges) into a NEW buffer; the source is never modified
    int gauss_to(int src, float sigma) {
        const int w = pl.bufs[src].w, h = pl.bufs[src].h;
        int r; std::vector<float> k = kf::gauss_kernel(sigma, r);
        int t = alloc(w, h);
        { KFOp& o = op(KF_K_GAUSSH, src, -1, -1, -1, t); o.P.i[0] = r; o.kw = k; }
        int out = alloc(w, h);
        { KFOp& o = op(KF_K_GAUSSV, t, -1, -1, -1, out); o.P.i[0] = r; o.kw = k; }
        release(t);
        return out;
    }
    // gaussian for any sigma; big sigmas on a reduced copy (mirrors gauss_blur_big). Returns `src` itself when nothing is to be done.
    int gauss_big_to(int src, float sigma) {
        if (sigma <= 0.05f) return src;
        if (sigma <= 4.f) return gauss_to(src, sigma);
        const int w = pl.bufs[src].w, h = pl.bufs[src].h;
        int k = 0; float s = sigma;
        while (s > 3.f && k < 9) { s *= 0.5f; ++k; }
        int cur = src;
        for (int i = 0; i < k; ++i) cur = down_to(cur, src);
        int g = gauss_to(cur, s);
        release(cur, src);
        int res = alloc(w, h);
        op(KF_K_UP, g, -1, -1, -1, res);
        release(g);
        return res;
    }
    int down_to(int src, int keep) {
        const int w2 = std::max(1, (pl.bufs[src].w + 1) / 2), h2 = std::max(1, (pl.bufs[src].h + 1) / 2);
        int out = alloc(w2, h2);
        op(KF_K_DOWN, src, -1, -1, -1, out);
        release(src, keep);
        return out;
    }
    // weighted line of taps (mirrors directional_blur_buf)
    int dir_to(int src, float angle_deg, float length_px) {
        if (length_px < 0.5f) return src;
        const int w = pl.bufs[src].w, h = pl.bufs[src].h;
        int k = 0; float L = length_px;
        while (L > 36.f && k < 9) { L *= 0.5f; ++k; }
        int s = src;
        for (int i = 0; i < k; ++i) s = down_to(s, src);
        const float a = angle_deg * 3.14159265358979f / 180.f;
        int o = alloc(pl.bufs[s].w, pl.bufs[s].h);
        { KFOp& q = op(KF_K_DIR, s, -1, -1, -1, o);
          q.P.f[0] = std::cos(a); q.P.f[1] = std::sin(a); q.P.f[2] = L;
          q.P.i[0] = std::min(129, std::max(3, ((int)(L * 1.2f)) | 1)); }
        release(s, src);
        if (k == 0) return o;
        int res = alloc(w, h);
        op(KF_K_UP, o, -1, -1, -1, res);
        release(o);
        return res;
    }
};

static inline int kf_edge_code(kf::EdgeMode m) { return m == kf::EdgeMode::Transparent ? 0 : (m == kf::EdgeMode::Clamp ? 1 : 2); }
static inline bool kf_zone_ev(const kf::ZoneRange& z) { return z.start <= 0.f && z.full <= 0.f; }

// Builds the launch list for lens, CA, lens blur, vignette, bloom and halation. Grain, Mix and the encode are done by the frame kernel
// afterwards (hasCur = 1). Fills `fp` (the frame kernel parameters) as well. Returns false when a needed kernel group is missing.
static inline bool kf_build_heavy_plan(const kf::Params& p, int w, int h, unsigned groups, KFPlan& pl)
{
    pl = KFPlan();
    KFPlanBuilder B(pl);
    const float fw = (float)w, fh = (float)h, diag = std::hypot(fw, fh);
    const float zcx = std::min(std::max(p.zone_center_x, 0.f), 1.f) * fw, zcy = std::min(std::max(p.zone_center_y, 0.f), 1.f) * fh;
    const float rnd = std::min(std::max(p.zone_roundness, 0.f), 1.f);

    int lin = B.alloc(w, h);
    { KFOp& o = B.op(KF_K_DECODE, 0, -1, -1, -1, lin);
      o.P.srcPitch = (unsigned)w;                                  // the host replaces this with the real source pitch
      o.P.i[0] = (p.input_space == kf::InputSpace::sRGB) ? 1 : (p.input_space == kf::InputSpace::Linear ? 0 : 2); }

    // 1 + 2: lens + chromatic aberration in one resample
    float z = 1.f;
    if (p.lens_enable || p.ca_enable) {
        if (p.lens_auto_zoom) z = kf::auto_zoom_for(w, h, p);
        if (p.lens_enable) z *= p.lens_zoom;
    }
    const bool geo = (p.lens_enable && p.lens_amount != 0.f) || (p.ca_enable && p.ca_amount != 0.f);
    auto geo_op = [&](int src, int dst, bool with_ca) {
        KFOp& o = B.op(KF_K_GEO, src, -1, -1, -1, dst);
        o.P.f[0] = p.lens_enable ? p.lens_amount * 0.30f : 0.f;
        o.P.f[1] = (with_ca && p.ca_enable) ? p.ca_amount * 0.001f : 0.f;
        o.P.f[2] = z; o.P.f[3] = zcx; o.P.f[4] = zcy; o.P.f[5] = rnd;
        o.P.f[6] = p.lens_zone.start; o.P.f[7] = p.lens_zone.full; o.P.f[8] = p.ca_zone.start; o.P.f[9] = p.ca_zone.full;
        o.P.i[0] = kf_edge_code(p.lens_edge); o.P.i[1] = kf_zone_ev(p.lens_zone) ? 1 : 0; o.P.i[2] = kf_zone_ev(p.ca_zone) ? 1 : 0;
    };
    int cur;
    if (!geo && z == 1.f) {
        cur = lin;
    } else if (p.ca_enable && p.ca_amount != 0.f && p.ca_blur > 0.f) {
        int Bn = B.alloc(w, h); geo_op(lin, Bn, false);
        int A = B.alloc(w, h);  geo_op(lin, A, true);
        B.release(lin);
        int D = B.alloc(w, h);  B.op(KF_K_CADIFF, Bn, A, -1, -1, D);
        B.release(A);
        int D2 = B.gauss_big_to(D, p.ca_blur * 0.002f * diag);
        B.release(D, D2);
        cur = B.alloc(w, h);    B.op(KF_K_CAADD, Bn, D2, -1, -1, cur);
        B.release(Bn); B.release(D2);
    } else {
        cur = B.alloc(w, h); geo_op(lin, cur, true);
        B.release(lin);
    }

    // 3: lens blur
    if (p.lblur_enable && p.lblur_amount > 0.f) {
        const kf::ZoneRange& zr = p.lblur_zone;
        if (p.lblur_type == kf::LensBlurType::Soft) {
            const float smax = p.lblur_amount * 0.005f * diag;
            int b1 = B.gauss_big_to(cur, smax / 3.f), b2 = B.gauss_big_to(cur, smax * 2.f / 3.f), b3 = B.gauss_big_to(cur, smax);
            int out = B.alloc(w, h);
            KFOp& o = B.op(KF_K_LBSOFT, cur, b1, b2, b3, out);
            o.P.f[0] = zcx; o.P.f[1] = zcy; o.P.f[2] = rnd; o.P.f[3] = zr.start; o.P.f[4] = zr.full; o.P.i[0] = kf_zone_ev(zr) ? 1 : 0;
            B.release(b1, cur); B.release(b2, cur); B.release(b3, cur);
            B.release(cur); cur = out;
        } else {
            const float ang = p.lblur_angle * 3.14159265358979f / 180.f;
            int out = B.alloc(w, h);
            KFOp& o = B.op(KF_K_LBMOVE, cur, -1, -1, -1, out);
            o.P.f[0] = p.lblur_amount * 0.01f * diag; o.P.f[1] = std::cos(ang); o.P.f[2] = std::sin(ang);
            o.P.f[3] = zcx; o.P.f[4] = zcy; o.P.f[5] = rnd; o.P.f[6] = zr.start; o.P.f[7] = zr.full;
            o.P.i[0] = p.lblur_type == kf::LensBlurType::Radial ? 1 : (p.lblur_type == kf::LensBlurType::Swirl ? 2 : 3);
            o.P.i[1] = kf_zone_ev(zr) ? 1 : 0;
            B.release(cur); cur = out;
        }
    }

    // 4: vignette (linear picture)
    if (p.vig_enable && p.vig_amount != 0.f) {
        int out = B.alloc(w, h);
        KFOp& o = B.op(KF_K_VIG, cur, -1, -1, -1, out);
        o.P.f[0] = p.vig_amount; o.P.f[1] = 1.f + 3.f * (1.f - std::min(std::max(p.vig_softness, 0.f), 1.f));
        o.P.f[2] = p.vig_zone.start; o.P.f[3] = p.vig_zone.full; o.P.f[4] = zcx; o.P.f[5] = zcy; o.P.f[6] = rnd;
        o.P.i[0] = kf_zone_ev(p.vig_zone) ? 1 : 0;
        B.release(cur); cur = out;
    }

    // 5 + 6: bloom and halation (mirror add_glow)
    auto glow_stage = [&](bool on, float thr, float inten, float radius_pct, const float tint[3], float blur, float stretch,
                          float stretch_angle, const kf::ZoneRange& zr) {
        if (!on || inten == 0.f) return;
        const float radius_px = std::max(radius_pct * 0.01f * diag, 1.5f);
        int levels = (int)std::ceil(std::log2(std::max(radius_px, 1.5f) / 1.5f));
        levels = std::min(std::max(levels, 0), 10);
        int mask = B.alloc(w, h);
        B.op(KF_K_MASK, cur, -1, -1, -1, mask).P.f[0] = thr;
        std::vector<int> raw(1, mask);
        for (int i = 0; i < levels; ++i) {
            if (std::min(pl.bufs[raw.back()].w, pl.bufs[raw.back()].h) < 4) break;
            raw.push_back(B.down_to(raw.back(), raw.back()));            // keep every level: they are blurred afterwards
        }
        const int n = (int)raw.size();
        std::vector<int> bl(n);
        for (int i = 0; i < n; ++i) bl[i] = B.gauss_to(raw[i], 1.2f);
        for (int i = 0; i < n; ++i) B.release(raw[i]);
        int S = bl[n - 1];
        for (int i = n - 2; i >= 0; --i) {
            int out = B.alloc(pl.bufs[bl[i]].w, pl.bufs[bl[i]].h);
            B.op(KF_K_UPADD, S, bl[i], -1, -1, out);
            B.release(S); B.release(bl[i]);
            S = out;
        }
        int g = S;
        if (blur > 0.f) { int g2 = B.gauss_big_to(g, blur * 0.01f * diag); if (g2 != g) { B.release(g); g = g2; } }
        if (stretch > 0.f) { int g2 = B.dir_to(g, stretch_angle, stretch * 6.f * radius_px); if (g2 != g) { B.release(g); g = g2; } }
        int out = B.alloc(w, h);
        KFOp& o = B.op(KF_K_ADDGLOW, cur, g, -1, -1, out);
        o.P.f[0] = inten / (float)n; o.P.f[1] = tint[0]; o.P.f[2] = tint[1]; o.P.f[3] = tint[2];
        o.P.f[4] = zr.start; o.P.f[5] = zr.full; o.P.f[6] = zcx; o.P.f[7] = zcy; o.P.f[8] = rnd;
        o.P.i[0] = kf_zone_ev(zr) ? 1 : 0;
        B.release(g); B.release(cur); cur = out;
    };
    glow_stage(p.bloom_enable, p.bloom_threshold, p.bloom_intensity, p.bloom_radius, p.bloom_tint, p.bloom_blur,
               p.bloom_stretch, p.bloom_stretch_angle, p.bloom_zone);
    glow_stage(p.halation_enable, p.halation_threshold, p.halation_intensity, p.halation_radius, p.halation_tint, p.halation_blur,
               p.halation_stretch, p.halation_stretch_angle, p.halation_zone);

    pl.cur = cur;
    return (pl.need & ~groups) == 0 && (groups & KF_GRP_GEO) != 0;
}

// True when any of Lens, CA, Lens Blur, Bloom or Halation would change the picture (checkbox on AND a non-neutral value).
static inline bool kf_heavy_needed(const kf::Params& p)
{
    return (p.lens_enable && (p.lens_amount != 0.f || p.lens_zoom != 1.f)) || (p.ca_enable && p.ca_amount != 0.f) ||
           (p.lblur_enable && p.lblur_amount > 0.f) || (p.bloom_enable && p.bloom_intensity != 0.f) ||
           (p.halation_enable && p.halation_intensity != 0.f);
}

// Frame-kernel parameters (Input Space, vignette for the light path, zone centre, Mix, grain) from the core parameters.
static inline KFGpuFrameParams kf_make_frame_params(const kf::Params& p, int w, int h, std::vector<KFGpuPass>* passes = nullptr)
{
    KFGpuFrameParams v;
    memset(&v, 0, sizeof v);
    v.width = (unsigned)w; v.height = (unsigned)h; v.srcPitch = v.dstPitch = (unsigned)w;
    v.inputSpace = (p.input_space == kf::InputSpace::sRGB) ? 1 : (p.input_space == kf::InputSpace::Linear ? 0 : 2);
    v.applyVig = (p.vig_enable && p.vig_amount != 0.f) ? 1 : 0;
    v.zoneEverywhere = (p.vig_zone.start <= 0.f && p.vig_zone.full <= 0.f) ? 1 : 0;
    v.mix = kf_clamp01f(p.mix);
    v.amount = p.vig_amount; v.softness = p.vig_softness;
    v.zoneStart = p.vig_zone.start; v.zoneFull = p.vig_zone.full;
    v.cx = kf_clamp01f(p.zone_center_x) * (float)w; v.cy = kf_clamp01f(p.zone_center_y) * (float)h;
    v.roundness = kf_clamp01f(p.zone_roundness);
    KFGrainPlan plan;
    if (kf_make_grain_plan(p, w, h, v, plan) && passes) kf_build_grain_passes(plan, w, h, *passes);
    return v;
}

