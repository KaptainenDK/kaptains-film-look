// Kaptains Film Look - Copyright (C) 2026 Henrik Jonas Kaptain
// SPDX-License-Identifier: GPL-3.0-or-later (with an additional permission for the Adobe SDKs, see LICENSE-EXCEPTION)
// Kaptains Film Look - core image processing library, v2 (implementation)
#include "kf_core.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

#include <atomic>
#include <cstdlib>
#include <thread>
#include <vector>

namespace kf {

// ------------------------------------------------------------------ threading
// Splits [0, n) over the CPU cores. Every iteration must write only its own output (no reductions), so the
// result is bit-identical to a single-threaded run. KF_THREADS=1 forces single-threaded (for tests).
static int thread_count() {
    static const int n = [] {
        const char* e = std::getenv("KF_THREADS");
        int v = e ? std::atoi(e) : 0;
        if (v <= 0) v = (int)std::thread::hardware_concurrency();
        if (v <= 0) v = 1;
        return v > 64 ? 64 : v;
    }();
    return n;
}

template <class F>
static void parallel_for(long n, F f) {
    const int nt = thread_count();
    if (nt <= 1 || n < 2) { for (long i = 0; i < n; ++i) f(i); return; }
    const long chunk = std::max(1L, n / ((long)nt * 8));    // small chunks: zone-dependent cost varies per row
    std::atomic<long> next(0);
    auto work = [&]() {
        for (;;) {
            long a = next.fetch_add(chunk);
            if (a >= n) break;
            long b = std::min(n, a + chunk);
            for (long i = a; i < b; ++i) f(i);
        }
    };
    const long chunks = (n + chunk - 1) / chunk;
    const int extra = (int)std::min<long>(nt - 1, chunks - 1);
    std::vector<std::thread> th;
    th.reserve(extra);
    for (int t = 0; t < extra; ++t) {
        try { th.emplace_back(work); } catch (...) { break; }   // if a thread cannot start, the others finish the work
    }
    work();
    for (auto& t : th) t.join();
}

// ------------------------------------------------------------------ helpers
static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
static const float kPi = 3.14159265358979f;

static inline float srgb_decode(float v) {
    float s = v < 0.f ? -1.f : 1.f, a = std::fabs(v);
    return s * (a <= 0.04045f ? a / 12.92f : std::pow((a + 0.055f) / 1.055f, 2.4f));
}
static inline float srgb_encode(float v) {
    float s = v < 0.f ? -1.f : 1.f, a = std::fabs(v);
    return s * (a <= 0.0031308f ? a * 12.92f : 1.055f * std::pow(a, 1.f / 2.4f) - 0.055f);
}
static inline float g24_decode(float v) { return v < 0.f ? -std::pow(-v, 2.4f) : std::pow(v, 2.4f); }
static inline float g24_encode(float v) { return v < 0.f ? -std::pow(-v, 1.f / 2.4f) : std::pow(v, 1.f / 2.4f); }

static void decode_to_linear(Buf& b, InputSpace sp) {
    if (sp == InputSpace::Linear) return;
    const long n = (long)b.w * b.h;
parallel_for(n, [&](long i) {
        float* p = &b.d[(size_t)i * b.c];
        for (int c = 0; c < 3; ++c) p[c] = (sp == InputSpace::sRGB) ? srgb_decode(p[c]) : g24_decode(p[c]);
    });
}
static void encode_from_linear(Buf& b, InputSpace sp) {
    if (sp == InputSpace::Linear) return;
    const long n = (long)b.w * b.h;
parallel_for(n, [&](long i) {
        float* p = &b.d[(size_t)i * b.c];
        for (int c = 0; c < 3; ++c) p[c] = (sp == InputSpace::sRGB) ? srgb_encode(p[c]) : g24_encode(p[c]);
    });
}

static inline int mirror_index(int i, int n) {
    int period = 2 * n;
    i = ((i % period) + period) % period;
    return i >= n ? period - 1 - i : i;
}

// Fetch one channel with edge handling. Returns 0 outside the frame in Transparent mode.
static inline float fetch_ch(const Buf& b, int x, int y, int ch, EdgeMode m) {
    if (x < 0 || y < 0 || x >= b.w || y >= b.h) {
        if (m == EdgeMode::Transparent) return 0.f;
        if (m == EdgeMode::Clamp) {
            x = x < 0 ? 0 : (x >= b.w ? b.w - 1 : x);
            y = y < 0 ? 0 : (y >= b.h ? b.h - 1 : y);
        } else {
            x = mirror_index(x, b.w);
            y = mirror_index(y, b.h);
        }
    }
    return b.d[((size_t)y * b.w + x) * b.c + ch];
}

static inline void catmull_rom(float t, float w[4]) {
    float t2 = t * t, t3 = t2 * t;
    w[0] = -0.5f * t3 + t2 - 0.5f * t;
    w[1] = 1.5f * t3 - 2.5f * t2 + 1.f;
    w[2] = -1.5f * t3 + 2.f * t2 + 0.5f * t;
    w[3] = 0.5f * t3 - 0.5f * t2;
}

// Bicubic (Catmull-Rom). sx, sy in pixel coordinates where pixel i has its centre at i + 0.5.
static float sample_bicubic(const Buf& b, float sx, float sy, int ch, EdgeMode m) {
    float x = sx - 0.5f, y = sy - 0.5f;
    int ix = (int)std::floor(x), iy = (int)std::floor(y);
    float wx[4], wy[4];
    catmull_rom(x - ix, wx);
    catmull_rom(y - iy, wy);
    float acc = 0.f;
    for (int j = 0; j < 4; ++j) {
        float row = 0.f;
        for (int i = 0; i < 4; ++i) row += wx[i] * fetch_ch(b, ix - 1 + i, iy - 1 + j, ch, m);
        acc += wy[j] * row;
    }
    return acc;
}

// Bilinear with clamped edges, all channels. Pixel centres at i + 0.5.
static inline void sample_bilinear(const Buf& b, float sx, float sy, float* out) {
    float x = sx - 0.5f, y = sy - 0.5f;
    int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
    float fx = x - x0, fy = y - y0;
    int x1 = std::min(std::max(x0 + 1, 0), b.w - 1), y1 = std::min(std::max(y0 + 1, 0), b.h - 1);
    x0 = std::min(std::max(x0, 0), b.w - 1); y0 = std::min(std::max(y0, 0), b.h - 1);
    const float* p00 = b.px(x0, y0); const float* p10 = b.px(x1, y0);
    const float* p01 = b.px(x0, y1); const float* p11 = b.px(x1, y1);
    for (int c = 0; c < b.c; ++c)
        out[c] = lerpf(lerpf(p00[c], p10[c], fx), lerpf(p01[c], p11[c], fx), fy);
}

// ------------------------------------------------------------------ zones
struct Zone {
    float cx, cy, hw, hh, Ra, round;
    // normalised distance from the zone centre: 0 = centre, 1 = corner of the active picture area
    float dist(float x, float y) const {
        float dx = x - cx, dy = y - cy;
        float dc = std::hypot(dx, dy) / Ra;
        float doval = std::hypot(dx / hw, dy / hh) * 0.70710678f;
        return lerpf(doval, dc, round);
    }
};

static Zone make_zone(int w, int h, const Params& p) {
    const float ax0 = p.active_area.x0 * w, ax1 = p.active_area.x1 * w;
    const float ay0 = p.active_area.y0 * h, ay1 = p.active_area.y1 * h;
    Zone z;
    z.cx = ax0 + clampf(p.zone_center_x, 0.f, 1.f) * (ax1 - ax0);
    z.cy = ay0 + clampf(p.zone_center_y, 0.f, 1.f) * (ay1 - ay0);
    z.hw = std::max(0.5f * (ax1 - ax0), 1.f);
    z.hh = std::max(0.5f * (ay1 - ay0), 1.f);
    z.Ra = std::hypot(z.hw, z.hh);
    z.round = clampf(p.zone_roundness, 0.f, 1.f);
    return z;
}

static inline bool zone_everywhere(const ZoneRange& z) { return z.start <= 0.f && z.full <= 0.f; }

// 0 inside Start, 1 beyond Full, smooth in between. Start = Full = 0 -> 1 everywhere.
static inline float zone_w(float d, const ZoneRange& z) {
    if (zone_everywhere(z)) return 1.f;
    float full = std::max(z.full, z.start + 0.002f);
    float t = clampf((d - z.start) / (full - z.start), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

// ------------------------------------------------------------------ geometry (1 + 2)
static const float kLensK = 0.30f;    // lens_amount = 1 -> k = 0.30
static const float kCaScale = 0.001f; // ca_amount = 1 -> 0.1% of half-diagonal at the corner

// Is every point on the output frame border mapped to a point inside the source?
static bool border_inside(int w, int h, float k0, float e0, float z, const Zone& zn, const Params& p) {
    const float R = 0.5f * std::hypot((float)w, (float)h);
    const int N = 512;
    for (int edge = 0; edge < 4; ++edge) {
        for (int i = 0; i <= N; ++i) {
            float t = (float)i / N, bx, by;
            switch (edge) {
                case 0: bx = t * w; by = 0.f; break;
                case 1: bx = t * w; by = (float)h; break;
                case 2: bx = 0.f; by = t * h; break;
                default: bx = (float)w; by = t * h; break;
            }
            float dx = bx - zn.cx, dy = by - zn.cy;
            float d = zn.dist(bx, by);
            float k = k0 * zone_w(d, p.lens_zone), e = e0 * zone_w(d, p.ca_zone);
            float rz = std::hypot(dx, dy) / R / z;
            float base = (1.f + k * rz * rz) / z;
            for (int s = -1; s <= 1; ++s) {
                float f = base * (1.f + e * (float)s);
                float sx = zn.cx + dx * f, sy = zn.cy + dy * f;
                if (sx < -1e-3f || sx > w + 1e-3f || sy < -1e-3f || sy > h + 1e-3f) return false;
            }
        }
    }
    return true;
}

float auto_zoom_for(int w, int h, const Params& p) {
    Zone zn = make_zone(w, h, p);
    float k0 = p.lens_enable ? p.lens_amount * kLensK : 0.f;
    float e0 = p.ca_enable ? p.ca_amount * kCaScale : 0.f;
    if (border_inside(w, h, k0, e0, 1.f, zn, p)) return 1.f;
    float lo = 1.f, hi = 8.f;
    if (!border_inside(w, h, k0, e0, hi, zn, p)) return hi;
    for (int it = 0; it < 40; ++it) {
        float mid = 0.5f * (lo + hi);
        if (border_inside(w, h, k0, e0, mid, zn, p)) hi = mid; else lo = mid;
    }
    return hi;
}

static void geometry_pass(const Buf& in, Buf& out, const Params& p, bool with_ca, float z, const Zone& zn) {
    const int w = in.w, h = in.h;
    const float k0 = p.lens_enable ? p.lens_amount * kLensK : 0.f;
    const float e0 = (with_ca && p.ca_enable) ? p.ca_amount * kCaScale : 0.f;
    out = Buf(w, h, 4);
    const float R = 0.5f * std::hypot((float)w, (float)h);
    const EdgeMode em = p.lens_edge;
    const float sgn[4] = {1.f, 0.f, -1.f, 0.f};  // R, G, B, A -> chromatic shift sign

parallel_for(h, [&](int y) {
        for (int x = 0; x < w; ++x) {
            float px = x + 0.5f, py = y + 0.5f;
            float dx = px - zn.cx, dy = py - zn.cy;
            float d = zn.dist(px, py);
            float k = k0 * zone_w(d, p.lens_zone), e = e0 * zone_w(d, p.ca_zone);
            float rz = std::hypot(dx, dy) / R / z;
            float base = (1.f + k * rz * rz) / z;
            float* o = out.px(x, y);
            for (int c = 0; c < 4; ++c) {
                float f = base * (1.f + e * sgn[c]);
                float v = sample_bicubic(in, zn.cx + dx * f, zn.cy + dy * f, c, em);
                o[c] = (c < 3) ? (v < 0.f ? 0.f : v) : clampf(v, 0.f, 1.f);
            }
        }
    });
}

// ------------------------------------------------------------------ blur utilities
static Buf downsample2(const Buf& s) {
    int w2 = std::max(1, (s.w + 1) / 2), h2 = std::max(1, (s.h + 1) / 2);
    Buf o(w2, h2, s.c);
    static const float k[4] = {0.125f, 0.375f, 0.375f, 0.125f};
parallel_for(h2, [&](int y) {
        for (int x = 0; x < w2; ++x) {
            float* op = o.px(x, y);
            for (int c = 0; c < s.c; ++c) {
                float acc = 0.f;
                for (int j = 0; j < 4; ++j) {
                    int yy = std::min(std::max(2 * y - 1 + j, 0), s.h - 1);
                    float row = 0.f;
                    for (int i = 0; i < 4; ++i) {
                        int xx = std::min(std::max(2 * x - 1 + i, 0), s.w - 1);
                        row += k[i] * s.d[((size_t)yy * s.w + xx) * s.c + c];
                    }
                    acc += k[j] * row;
                }
                op[c] = acc;
            }
        }
    });
    return o;
}

// bilinear upsample of `s` to (w, h)
static Buf upsample_to(const Buf& s, int w, int h) {
    Buf o(w, h, s.c);
    const float sx = (float)s.w / w, sy = (float)s.h / h;
parallel_for(h, [&](int y) { for (int x = 0; x < w; ++x) sample_bilinear(s, (x + 0.5f) * sx, (y + 0.5f) * sy, o.px(x, y)); });
    return o;
}

static std::vector<float> gauss_kernel(float sigma, int& radius) {
    radius = std::max(1, (int)std::ceil(3.f * sigma));
    std::vector<float> k(2 * radius + 1);
    float sum = 0.f;
    for (int i = -radius; i <= radius; ++i) { k[i + radius] = std::exp(-0.5f * i * i / (sigma * sigma)); sum += k[i + radius]; }
    for (auto& v : k) v /= sum;
    return k;
}

// separable gaussian on an interleaved buffer, clamp-to-edge
static void gauss_blur(Buf& b, float sigma) {
    if (sigma <= 0.f) return;
    int r; std::vector<float> k = gauss_kernel(sigma, r);
    Buf t(b.w, b.h, b.c);
parallel_for(b.h, [&](int y) { for (int x = 0; x < b.w; ++x)
            for (int c = 0; c < b.c; ++c) {
                float acc = 0.f;
                for (int i = -r; i <= r; ++i) {
                    int xx = std::min(std::max(x + i, 0), b.w - 1);
                    acc += k[i + r] * b.d[((size_t)y * b.w + xx) * b.c + c];
                }
                t.d[((size_t)y * b.w + x) * b.c + c] = acc;
            } });
parallel_for(b.h, [&](int y) { for (int x = 0; x < b.w; ++x)
            for (int c = 0; c < b.c; ++c) {
                float acc = 0.f;
                for (int j = -r; j <= r; ++j) {
                    int yy = std::min(std::max(y + j, 0), b.h - 1);
                    acc += k[j + r] * t.d[((size_t)yy * b.w + x) * b.c + c];
                }
                b.d[((size_t)y * b.w + x) * b.c + c] = acc;
            } });
}

// gaussian blur for any sigma: large sigmas are done on a reduced copy
static void gauss_blur_big(Buf& b, float sigma) {
    if (sigma <= 0.05f) return;
    if (sigma <= 4.f) { gauss_blur(b, sigma); return; }
    int k = 0; float s = sigma;
    while (s > 3.f && k < 9) { s *= 0.5f; ++k; }
    Buf small = b;
    for (int i = 0; i < k; ++i) small = downsample2(small);
    gauss_blur(small, s);
    b = upsample_to(small, b.w, b.h);
}

// weighted line of taps through every pixel along a direction; energy preserving.
// length_px = total length of the line (about 4 sigma of the gaussian weighting)
static void directional_blur_buf(Buf& b, float angle_deg, float length_px) {
    if (length_px < 0.5f) return;
    int k = 0; float L = length_px;
    while (L > 36.f && k < 9) { L *= 0.5f; ++k; }
    Buf s = b;
    for (int i = 0; i < k; ++i) s = downsample2(s);
    Buf o(s.w, s.h, s.c);
    const float a = angle_deg * kPi / 180.f, cs = std::cos(a), sn = std::sin(a);
    const int N = std::min(129, std::max(3, ((int)(L * 1.2f)) | 1));
parallel_for(s.h, [&](int y) { for (int x = 0; x < s.w; ++x) {
            float acc[4] = {0, 0, 0, 0}, tmp[4], ws = 0.f;
            for (int i = 0; i < N; ++i) {
                float t = ((float)i / (N - 1)) * 2.f - 1.f, wt = std::exp(-2.f * t * t);
                sample_bilinear(s, x + 0.5f + cs * t * L * 0.5f, y + 0.5f + sn * t * L * 0.5f, tmp);
                for (int c = 0; c < s.c; ++c) acc[c] += wt * tmp[c];
                ws += wt;
            }
            float* op = o.px(x, y);
            for (int c = 0; c < s.c; ++c) op[c] = acc[c] / ws;
        } });
    b = (k > 0) ? upsample_to(o, b.w, b.h) : std::move(o);
}

// ------------------------------------------------------------------ lens blur (3)
static void lens_blur(Buf& img, const Params& p, const Zone& zn) {
    if (!p.lblur_enable || p.lblur_amount <= 0.f) return;
    const int w = img.w, h = img.h;
    const float diag = std::hypot((float)w, (float)h);
    const Buf orig = img;

    if (p.lblur_type == LensBlurType::Soft) {
        // blend between a few pre-blurred copies according to the zone weight
        const float smax = p.lblur_amount * 0.005f * diag;
        Buf b1 = orig, b2 = orig, b3 = orig;
        gauss_blur_big(b1, smax / 3.f); gauss_blur_big(b2, smax * 2.f / 3.f); gauss_blur_big(b3, smax);
        const Buf* lv[4] = {&orig, &b1, &b2, &b3};
parallel_for(h, [&](int y) { for (int x = 0; x < w; ++x) {
                float f = zone_w(zn.dist(x + 0.5f, y + 0.5f), p.lblur_zone) * 3.f;
                int i = std::min((int)f, 2); float t = f - i;
                const float* a = lv[i]->px(x, y); const float* b = lv[i + 1]->px(x, y);
                float* o = img.px(x, y);
                for (int c = 0; c < 4; ++c) o[c] = lerpf(a[c], b[c], t);
            } });
        return;
    }

    const float Lmax = p.lblur_amount * 0.01f * diag;
    const float ang = p.lblur_angle * kPi / 180.f, dcs = std::cos(ang), dsn = std::sin(ang);
parallel_for(h, [&](int y) { for (int x = 0; x < w; ++x) {
            float px = x + 0.5f, py = y + 0.5f;
            float L = Lmax * zone_w(zn.dist(px, py), p.lblur_zone);
            if (L < 0.4f) continue;
            float dx = px - zn.cx, dy = py - zn.cy, r = std::hypot(dx, dy);
            float rx = r > 1e-3f ? dx / r : 1.f, ry = r > 1e-3f ? dy / r : 0.f;
            const int N = std::min(129, std::max(3, ((int)(L * 1.2f)) | 1));
            float acc[4] = {0, 0, 0, 0}, tmp[4], ws = 0.f;
            for (int i = 0; i < N; ++i) {
                float t = ((float)i / (N - 1)) * 2.f - 1.f, wt = std::exp(-2.f * t * t);
                float sx, sy;
                if (p.lblur_type == LensBlurType::Radial) {
                    sx = px + rx * t * L * 0.5f; sy = py + ry * t * L * 0.5f;
                } else if (p.lblur_type == LensBlurType::Swirl) {
                    float th = clampf(t * L * 0.5f / std::max(r, 1.f), -0.8f, 0.8f);
                    float c = std::cos(th), s = std::sin(th);
                    sx = zn.cx + dx * c - dy * s; sy = zn.cy + dx * s + dy * c;
                } else {
                    sx = px + dcs * t * L * 0.5f; sy = py + dsn * t * L * 0.5f;
                }
                sample_bilinear(orig, sx, sy, tmp);
                for (int c = 0; c < 4; ++c) acc[c] += wt * tmp[c];
                ws += wt;
            }
            float* o = img.px(x, y);
            for (int c = 0; c < 4; ++c) o[c] = acc[c] / ws;
        } });
}

// ------------------------------------------------------------------ vignette (4)
static void vignette(Buf& img, const Params& p, const Zone& zn) {
    if (p.vig_amount == 0.f) return;
    const int w = img.w, h = img.h;
    const float pw = 1.f + 3.f * (1.f - clampf(p.vig_softness, 0.f, 1.f));
    const bool everywhere = zone_everywhere(p.vig_zone);
    const float full = std::max(p.vig_zone.full, p.vig_zone.start + 0.002f);

parallel_for(h, [&](int y) { for (int x = 0; x < w; ++x) {
            float d = zn.dist(x + 0.5f, y + 0.5f);
            float t = everywhere ? 1.f : clampf((d - p.vig_zone.start) / (full - p.vig_zone.start), 0.f, 1.f);
            float g = std::exp2(-p.vig_amount * std::pow(t, pw));
            float* o = img.px(x, y);
            o[0] *= g; o[1] *= g; o[2] *= g;
        } });
}

// ------------------------------------------------------------------ glow (5 + 6)
// Soft-knee highlight extraction -> RGB (c = 3)
static Buf highlight_mask(const Buf& img, float threshold) {
    Buf m(img.w, img.h, 3);
    const float knee = 0.12f;
parallel_for(img.h, [&](int y) { for (int x = 0; x < img.w; ++x) {
            const float* ip = img.px(x, y);
            float* mp = m.px(x, y);
            for (int c = 0; c < 3; ++c) {
                float v = ip[c] - threshold;
                mp[c] = v <= -knee ? 0.f : (v < knee ? (v + knee) * (v + knee) / (4.f * knee) : v);
            }
        } });
    return m;
}

// Multi-scale glow: sum of gaussians whose widths double per level, up to `radius_px`.
static Buf glow(const Buf& mask, float radius_px) {
    int levels = (int)std::ceil(std::log2(std::max(radius_px, 1.5f) / 1.5f));
    levels = std::min(std::max(levels, 0), 10);
    std::vector<Buf> pyr;
    pyr.push_back(mask);
    for (int i = 0; i < levels; ++i) {
        if (std::min(pyr.back().w, pyr.back().h) < 4) break;
        pyr.push_back(downsample2(pyr.back()));
    }
    const int n = (int)pyr.size();
    for (auto& l : pyr) gauss_blur(l, 1.2f);
    std::vector<float> wgt(n, 1.f / n);  // equal energy per octave
    Buf acc = pyr[n - 1];
    for (auto& v : acc.d) v *= wgt[n - 1];
    for (int i = n - 2; i >= 0; --i) {
        Buf up = upsample_to(acc, pyr[i].w, pyr[i].h);
        parallel_for((long)up.d.size(), [&](long j) { up.d[j] += wgt[i] * pyr[i].d[j]; });
        acc = std::move(up);
    }
    return acc;
}

static void add_glow(Buf& img, float threshold, float intensity, float radius_pct, const float tint[3],
                     float blur, float stretch, float stretch_angle, const ZoneRange& zone, const Zone& zn) {
    if (intensity == 0.f) return;
    const float diag = std::hypot((float)img.w, (float)img.h);
    const float radius_px = std::max(radius_pct * 0.01f * diag, 1.5f);
    Buf g = glow(highlight_mask(img, threshold), radius_px);
    if (blur > 0.f) gauss_blur_big(g, blur * 0.01f * diag);
    if (stretch > 0.f) directional_blur_buf(g, stretch_angle, stretch * 6.f * radius_px);
parallel_for(img.h, [&](int y) { for (int x = 0; x < img.w; ++x) {
            float wz = zone_w(zn.dist(x + 0.5f, y + 0.5f), zone);
            float* ip = img.px(x, y);
            const float* gp = g.px(x, y);
            for (int c = 0; c < 3; ++c) ip[c] += gp[c] * intensity * tint[c] * wz;
        } });
}

// ------------------------------------------------------------------ grain (7)
static inline uint32_t hash32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
    return x;
}
static inline float gauss_noise(uint32_t x, uint32_t y, uint32_t seed) {
    uint32_t h1 = hash32(x * 0x9E3779B1u ^ hash32(y + seed * 0x85ebca6bu));
    uint32_t h2 = hash32(h1 + 0x68bc21ebu);
    float u1 = (float)(h1 >> 8) * (1.f / 16777216.f) + 1e-7f;
    float u2 = (float)(h2 >> 8) * (1.f / 16777216.f);
    return std::sqrt(-2.f * std::log(u1)) * std::cos(6.2831853f * u2);
}

// Unit-variance, gaussian-filtered noise field (w*h floats). Generated with padding so the
// statistics are exact all the way to the border. Pattern is tied to image pixel positions.
static std::vector<float> noise_field(int w, int h, uint32_t seed, float sigma) {
    std::vector<float> out((size_t)w * h);
    if (sigma < 0.05f) {
parallel_for(h, [&](int y) { for (int x = 0; x < w; ++x) out[(size_t)y * w + x] = gauss_noise(x, y, seed); });
        return out;
    }
    int r; std::vector<float> k = gauss_kernel(sigma, r);
    float sumsq = 0.f; for (float v : k) sumsq += v * v;
    const float norm = 1.f / sumsq;               // std after 2D separable blur = sumsq
    const int pw = w + 2 * r, ph = h + 2 * r;
    std::vector<float> pad((size_t)pw * ph), hb((size_t)w * ph);
parallel_for(ph, [&](int y) { for (int x = 0; x < pw; ++x)
            pad[(size_t)y * pw + x] = gauss_noise((uint32_t)(x - r + 100000), (uint32_t)(y - r + 100000), seed); });
parallel_for(ph, [&](int y) { for (int x = 0; x < w; ++x) {
            float acc = 0.f;
            for (int i = -r; i <= r; ++i) acc += k[i + r] * pad[(size_t)y * pw + (x + r + i)];
            hb[(size_t)y * w + x] = acc;
        } });
parallel_for(h, [&](int y) { for (int x = 0; x < w; ++x) {
            float acc = 0.f;
            for (int j = -r; j <= r; ++j) acc += k[j + r] * hb[(size_t)(y + r + j) * w + x];
            out[(size_t)y * w + x] = acc * norm;
        } });
    return out;
}

static void grain(Buf& img, const Params& p, const Zone& zn) {
    if (p.grain_amount == 0.f) return;
    const int w = img.w, h = img.h;
    const float diag = std::hypot((float)w, (float)h);
    const float size_px = std::max(p.grain_size, 0.05f) * diag / 1600.f;
    const float sig_f = 0.45f * size_px, sig_c = sig_f * 2.5f;
    const float rough = clampf(p.grain_roughness, 0.f, 1.f);
    const float col = clampf(p.grain_color, 0.f, 1.f);
    const float bias = clampf(p.grain_midtone_bias, 0.f, 1.f);
    const uint32_t base_seed = (uint32_t)p.grain_seed * 7919u + (p.grain_animate ? (uint32_t)p.frame * 104729u : 0u);
    const float amp0 = p.grain_amount * 0.05f;

    auto layered = [&](uint32_t id) {
        std::vector<float> a = noise_field(w, h, base_seed * 31u + id * 2u, sig_f);
        if (rough > 0.001f) {
            std::vector<float> b = noise_field(w, h, base_seed * 31u + id * 2u + 1u, sig_c);
            const float wa = std::sqrt(1.f - rough), wb = std::sqrt(rough);
parallel_for((long)a.size(), [&](long i) { a[i] = wa * a[i] + wb * b[i]; });
        }
        return a;
    };

    std::vector<float> shared = layered(0);
    // amplitude from the tonal range (before grain) and from the zone
    std::vector<float> amp((size_t)w * h);
parallel_for(h, [&](int y) { for (int x = 0; x < w; ++x) {
            const float* ip = img.px(x, y);
            float l = 0.2126f * ip[0] + 0.7152f * ip[1] + 0.0722f * ip[2];
            float lp = clampf(srgb_encode(std::max(l, 0.f)), 0.f, 1.f);
            amp[(size_t)y * w + x] = amp0 * lerpf(1.f, 4.f * lp * (1.f - lp), bias) * zone_w(zn.dist(x + 0.5f, y + 0.5f), p.grain_zone);
        } });
    const float ws = std::sqrt(1.f - col), wi = std::sqrt(col);
    for (int c = 0; c < 3; ++c) {
        std::vector<float> indep;
        if (col > 0.001f) indep = layered(1 + c);
parallel_for(h, [&](int y) { for (int x = 0; x < w; ++x) {
                size_t i = (size_t)y * w + x;
                float n = ws * shared[i] + (col > 0.001f ? wi * indep[i] : 0.f);
                float* ip = img.px(x, y);
                float pv = srgb_encode(ip[c]);
                ip[c] = std::max(0.f, srgb_decode(pv + n * amp[i]));   // grain never makes a pixel negative
            } });
    }
}

// ------------------------------------------------------------------ zone rings (preview aid)
struct RingSpec { bool active; ZoneRange z; float r, g, b; };

static void draw_zone_rings(Buf& img, const Params& p, const Zone& zn) {
    const RingSpec specs[7] = {
        {p.lens_enable && p.lens_amount != 0.f, p.lens_zone, 0.f, 0.9f, 1.f},          // cyan
        {p.ca_enable && p.ca_amount != 0.f, p.ca_zone, 1.f, 0.2f, 0.9f},               // magenta
        {p.lblur_enable && p.lblur_amount > 0.f, p.lblur_zone, 1.f, 0.6f, 0.f},        // orange
        {p.vig_enable && p.vig_amount != 0.f, p.vig_zone, 1.f, 1.f, 0.2f},             // yellow
        {p.bloom_enable && p.bloom_intensity > 0.f, p.bloom_zone, 0.5f, 0.75f, 1.f},   // light blue
        {p.halation_enable && p.halation_intensity > 0.f, p.halation_zone, 1.f, 0.2f, 0.2f}, // red
        {p.grain_enable && p.grain_amount > 0.f, p.grain_zone, 0.3f, 1.f, 0.3f},       // green
    };
    bool any = false;
    for (const auto& s : specs) if (s.active && !zone_everywhere(s.z)) any = true;
    if (!any) return;
    const int w = img.w, h = img.h;
    const float diag = std::hypot((float)w, (float)h);
    const float th = std::max(1.2f, diag * 0.0008f);
    const float arm = diag * 0.012f;

parallel_for(h, [&](int y) { for (int x = 0; x < w; ++x) {
            float px = x + 0.5f, py = y + 0.5f;
            float d = zn.dist(px, py);
            float* o = img.px(x, y);
            for (const auto& s : specs) {
                if (!s.active || zone_everywhere(s.z)) continue;
                for (int lvl = 0; lvl < 2; ++lvl) {
                    float level = lvl == 0 ? s.z.start : std::max(s.z.full, s.z.start);
                    if (level <= 0.f) continue;
                    float dist_px = std::fabs(d - level) * zn.Ra;
                    if (dist_px > th + 1.f) continue;
                    if (lvl == 0) {  // Start ring is dashed, Full ring is solid
                        float ang = std::atan2(py - zn.cy, px - zn.cx);
                        float ph = std::fmod((ang + kPi) * 48.f / (2.f * kPi), 1.f);
                        if (ph >= 0.5f) continue;
                    }
                    float a = clampf(th + 0.5f - dist_px, 0.f, 1.f) * 0.9f;
                    o[0] = lerpf(o[0], s.r, a); o[1] = lerpf(o[1], s.g, a); o[2] = lerpf(o[2], s.b, a);
                }
            }
            // small cross at the zone centre
            float dx = std::fabs(px - zn.cx), dy = std::fabs(py - zn.cy);
            if ((dx < th && dy < arm) || (dy < th && dx < arm)) { o[0] = o[1] = o[2] = 1.f; }
        } });
}

// ------------------------------------------------------------------ pipeline
void process(const Buf& in, Buf& out, const Params& p) {
    Buf lin = in;
    decode_to_linear(lin, p.input_space);
    const int w = lin.w, h = lin.h;
    const float diag = std::hypot((float)w, (float)h);
    const Zone zn = make_zone(w, h, p);

    // 1 + 2: geometry and chromatic aberration in one resample
    float z = 1.f;
    if (p.lens_enable || p.ca_enable) {
        if (p.lens_auto_zoom) z = auto_zoom_for(w, h, p);
        if (p.lens_enable) z *= p.lens_zoom;
    }
    const bool geo = (p.lens_enable && p.lens_amount != 0.f) || (p.ca_enable && p.ca_amount != 0.f);
    Buf cur;
    if (!geo && z == 1.f) {
        cur = lin;
    } else if (p.ca_enable && p.ca_amount != 0.f && p.ca_blur > 0.f) {
        // Soften only the fringe itself: blur (with CA - without CA) and add it back to the CA-free picture.
        Buf B, A;
        geometry_pass(lin, B, p, false, z, zn);
        geometry_pass(lin, A, p, true, z, zn);
        parallel_for((long)(A.d.size() / 4), [&](long q) {
            const size_t i = (size_t)q * 4;
            A.d[i] -= B.d[i]; A.d[i + 1] -= B.d[i + 1]; A.d[i + 2] -= B.d[i + 2]; A.d[i + 3] = 0.f;
        });
        gauss_blur_big(A, p.ca_blur * 0.002f * diag);
        cur = std::move(B);
        parallel_for((long)(cur.d.size() / 4), [&](long q) {
            const size_t i = (size_t)q * 4;
            for (int c = 0; c < 3; ++c) cur.d[i + c] = std::max(0.f, cur.d[i + c] + A.d[i + c]);
        });
    } else {
        geometry_pass(lin, cur, p, true, z, zn);
    }

    lens_blur(cur, p, zn);                                                       // 3
    if (p.vig_enable) vignette(cur, p, zn);                                      // 4
    if (p.bloom_enable)
        add_glow(cur, p.bloom_threshold, p.bloom_intensity, p.bloom_radius, p.bloom_tint,
                 p.bloom_blur, p.bloom_stretch, p.bloom_stretch_angle, p.bloom_zone, zn);              // 5
    if (p.halation_enable)
        add_glow(cur, p.halation_threshold, p.halation_intensity, p.halation_radius, p.halation_tint,
                 p.halation_blur, p.halation_stretch, p.halation_stretch_angle, p.halation_zone, zn);  // 6
    if (p.grain_enable) grain(cur, p, zn);                                       // 7

    if (p.mix < 1.f) {
        const float m = clampf(p.mix, 0.f, 1.f);
        parallel_for((long)cur.d.size(), [&](long i) { cur.d[i] = lerpf(lin.d[i], cur.d[i], m); });
    }
    encode_from_linear(cur, p.input_space);
    if (p.show_zones) draw_zone_rings(cur, p, zn);
    out = std::move(cur);
}

} // namespace kf
