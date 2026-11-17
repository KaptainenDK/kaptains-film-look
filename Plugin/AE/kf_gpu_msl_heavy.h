// Kaptains Film Look - Copyright (C) 2026 Henrik Jonas Kaptain
// SPDX-License-Identifier: GPL-3.0-or-later (with an additional permission for the Adobe SDKs, see LICENSE-EXCEPTION)
// Kaptains Film Look - Metal kernels for the heavy effects (text). Four raw strings: a shared prelude and three kernel groups.
// Each group is compiled as its own Metal library (prelude + group), so one group failing to compile cannot take the others down.
// struct KFOpParams is prepended from KF_OP_FIELDS at start-up. All kernels share one calling convention:
//   buffers 0..3 = inputs, 4 = output, 5 = KFOpParams, 6 = gaussian weights.  Working buffers: float4 R,G,B,A, straight alpha, linear.
// This text is also compiled as plain C++ by tests/gpu_kernel_test.cpp and compared with the CPU core.
#pragma once

static const char* kKFHeavyCommonMSL = R"MSL(
float kf_clamp01(float v) { return min(max(v, 0.0f), 1.0f); }

float kf_hdec(float v, int sp)
{
    if (sp == 1) {
        float s = v < 0.0f ? -1.0f : 1.0f;
        float a = fabs(v);
        return s * (a <= 0.04045f ? a / 12.92f : precise::pow((a + 0.055f) / 1.055f, 2.4f));
    }
    if (sp == 2) return v < 0.0f ? -precise::pow(-v, 2.4f) : precise::pow(v, 2.4f);
    return v;
}

// zone weight: ev != 0 means Start = Full = 0 (effect everywhere)
float kf_zw(float d, float zs, float zf, int ev)
{
    if (ev != 0) return 1.0f;
    float full = max(zf, zs + 0.002f);
    float t = kf_clamp01((d - zs) / (full - zs));
    return t * t * (3.0f - 2.0f * t);
}

// normalised distance from the zone centre (0 = centre, 1 = corner), circle <-> frame-shaped oval
float kf_zdist(float px, float py, float cx, float cy, float W, float H, float rnd)
{
    float hw = max(0.5f * W, 1.0f), hh = max(0.5f * H, 1.0f);
    float Ra = sqrt(hw * hw + hh * hh);
    float dx = px - cx, dy = py - cy;
    float dc = sqrt(dx * dx + dy * dy) / Ra;
    float ex = dx / hw, ey = dy / hh;
    float doval = sqrt(ex * ex + ey * ey) * 0.70710678f;
    return doval + (dc - doval) * rnd;
}

float4 kf_lerp4(float4 a, float4 b, float t) { return a + (b - a) * t; }

// bilinear, clamped edges, pixel centres at i + 0.5
float4 kf_bil(device const float4* b, int w, int h, float sx, float sy)
{
    float x = sx - 0.5f, y = sy - 0.5f;
    int x0 = (int)floor(x), y0 = (int)floor(y);
    float fx = x - (float)x0, fy = y - (float)y0;
    int x1 = min(max(x0 + 1, 0), w - 1), y1 = min(max(y0 + 1, 0), h - 1);
    x0 = min(max(x0, 0), w - 1); y0 = min(max(y0, 0), h - 1);
    float4 p00 = b[y0 * w + x0], p10 = b[y0 * w + x1], p01 = b[y1 * w + x0], p11 = b[y1 * w + x1];
    return kf_lerp4(kf_lerp4(p00, p10, fx), kf_lerp4(p01, p11, fx), fy);
}

int kf_mirror(int i, int n)
{
    int period = 2 * n;
    i = ((i % period) + period) % period;
    return i >= n ? period - 1 - i : i;
}

// edge modes: 0 transparent, 1 clamp, 2 mirror
float4 kf_fetch(device const float4* b, int w, int h, int x, int y, int mode)
{
    if (x < 0 || y < 0 || x >= w || y >= h) {
        if (mode == 0) return float4(0.0f, 0.0f, 0.0f, 0.0f);
        if (mode == 1) { x = x < 0 ? 0 : (x >= w ? w - 1 : x); y = y < 0 ? 0 : (y >= h ? h - 1 : y); }
        else { x = kf_mirror(x, w); y = kf_mirror(y, h); }
    }
    return b[y * w + x];
}

void kf_catmull(float t, thread float* w)
{
    float t2 = t * t, t3 = t2 * t;
    w[0] = -0.5f * t3 + t2 - 0.5f * t;
    w[1] = 1.5f * t3 - 2.5f * t2 + 1.0f;
    w[2] = -1.5f * t3 + 2.0f * t2 + 0.5f * t;
    w[3] = 0.5f * t3 - 0.5f * t2;
}

// Catmull-Rom bicubic, all four channels at one position (each channel is computed exactly like the single-channel core code)
float4 kf_bicubic(device const float4* b, int w, int h, float sx, float sy, int mode)
{
    float x = sx - 0.5f, y = sy - 0.5f;
    int ix = (int)floor(x), iy = (int)floor(y);
    float wx[4], wy[4];
    kf_catmull(x - (float)ix, wx);
    kf_catmull(y - (float)iy, wy);
    float4 acc = float4(0.0f, 0.0f, 0.0f, 0.0f);
    for (int j = 0; j < 4; ++j) {
        float4 row = float4(0.0f, 0.0f, 0.0f, 0.0f);
        for (int i = 0; i < 4; ++i) row = row + kf_fetch(b, w, h, ix - 1 + i, iy - 1 + j, mode) * wx[i];
        acc = acc + row * wy[j];
    }
    return acc;
}
)MSL";

// ---------------------------------------------------------------- group GEO: decode, lens + chromatic aberration
static const char* kKFGeoMSL = R"MSL(
// source (After Effects: B,G,R,A premultiplied, pitch in pixels) -> linear straight R,G,B,A.   i[0] = input space
kernel void KFHDecode(device const float4* in0 [[buffer(0)]], device const float4* in1 [[buffer(1)]],
                      device const float4* in2 [[buffer(2)]], device const float4* in3 [[buffer(3)]],
                      device float4* out [[buffer(4)]], constant KFOpParams& P [[buffer(5)]], constant float* kw [[buffer(6)]],
                      uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= P.w || gid.y >= P.h) return;
    float4 s = in0[gid.y * P.srcPitch + gid.x];
    float a = s.w;
    float r = s.z, g = s.y, b = s.x;
    if (a > 0.0f && a < 1.0f) { r /= a; g /= a; b /= a; }
    out[gid.y * P.w + gid.x] = float4(kf_hdec(r, P.i[0]), kf_hdec(g, P.i[0]), kf_hdec(b, P.i[0]), a);
}

// Lens distortion + chromatic aberration in one resample.
// f: 0 k0, 1 e0, 2 zoom, 3 cx, 4 cy, 5 roundness, 6/7 lens zone start/full, 8/9 CA zone start/full.   i: 0 edge mode, 1 lens zone everywhere, 2 CA zone everywhere
kernel void KFGeo(device const float4* in0 [[buffer(0)]], device const float4* in1 [[buffer(1)]],
                  device const float4* in2 [[buffer(2)]], device const float4* in3 [[buffer(3)]],
                  device float4* out [[buffer(4)]], constant KFOpParams& P [[buffer(5)]], constant float* kw [[buffer(6)]],
                  uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= P.w || gid.y >= P.h) return;
    int w = (int)P.w, h = (int)P.h;
    float W = (float)w, H = (float)h;
    float px = (float)gid.x + 0.5f, py = (float)gid.y + 0.5f;
    float cx = P.f[3], cy = P.f[4];
    float dx = px - cx, dy = py - cy;
    float d = kf_zdist(px, py, cx, cy, W, H, P.f[5]);
    float k = P.f[0] * kf_zw(d, P.f[6], P.f[7], P.i[1]);
    float e = P.f[1] * kf_zw(d, P.f[8], P.f[9], P.i[2]);
    float R = 0.5f * sqrt(W * W + H * H);
    float rz = sqrt(dx * dx + dy * dy) / R / P.f[2];
    float base = (1.0f + k * rz * rz) / P.f[2];
    float fr = base * (1.0f + e), fg = base * (1.0f + e * 0.0f), fb = base * (1.0f + e * -1.0f);
    float4 sr = kf_bicubic(in0, w, h, cx + dx * fr, cy + dy * fr, P.i[0]);     // red
    float4 sg = kf_bicubic(in0, w, h, cx + dx * fg, cy + dy * fg, P.i[0]);     // green and alpha
    float4 sb = kf_bicubic(in0, w, h, cx + dx * fb, cy + dy * fb, P.i[0]);     // blue
    float rr = sr.x < 0.0f ? 0.0f : sr.x;
    float gg = sg.y < 0.0f ? 0.0f : sg.y;
    float bb = sb.z < 0.0f ? 0.0f : sb.z;
    out[gid.y * P.w + gid.x] = float4(rr, gg, bb, kf_clamp01(sg.w));
}

// in0 = picture without CA, in1 = picture with CA  ->  (with - without), alpha 0
kernel void KFCaDiff(device const float4* in0 [[buffer(0)]], device const float4* in1 [[buffer(1)]],
                     device const float4* in2 [[buffer(2)]], device const float4* in3 [[buffer(3)]],
                     device float4* out [[buffer(4)]], constant KFOpParams& P [[buffer(5)]], constant float* kw [[buffer(6)]],
                     uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= P.w || gid.y >= P.h) return;
    uint i = gid.y * P.w + gid.x;
    float4 b = in0[i], a = in1[i];
    out[i] = float4(a.x - b.x, a.y - b.y, a.z - b.z, 0.0f);
}

// in0 = picture without CA, in1 = blurred fringe  ->  max(0, picture + fringe), alpha of the picture
kernel void KFCaAdd(device const float4* in0 [[buffer(0)]], device const float4* in1 [[buffer(1)]],
                    device const float4* in2 [[buffer(2)]], device const float4* in3 [[buffer(3)]],
                    device float4* out [[buffer(4)]], constant KFOpParams& P [[buffer(5)]], constant float* kw [[buffer(6)]],
                    uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= P.w || gid.y >= P.h) return;
    uint i = gid.y * P.w + gid.x;
    float4 b = in0[i], d = in1[i];
    out[i] = float4(max(0.0f, b.x + d.x), max(0.0f, b.y + d.y), max(0.0f, b.z + d.z), b.w);
}
)MSL";

// ---------------------------------------------------------------- group BLUR: pyramid steps, gaussian, directional
static const char* kKFBlurMSL = R"MSL(
// 4-tap binomial downsample by 2 (in0: sw x sh -> out: w x h)
kernel void KFDown(device const float4* in0 [[buffer(0)]], device const float4* in1 [[buffer(1)]],
                   device const float4* in2 [[buffer(2)]], device const float4* in3 [[buffer(3)]],
                   device float4* out [[buffer(4)]], constant KFOpParams& P [[buffer(5)]], constant float* kw [[buffer(6)]],
                   uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= P.w || gid.y >= P.h) return;
    int sw = (int)P.sw, sh = (int)P.sh;
    float k[4] = {0.125f, 0.375f, 0.375f, 0.125f};
    float4 acc = float4(0.0f, 0.0f, 0.0f, 0.0f);
    for (int j = 0; j < 4; ++j) {
        int yy = min(max(2 * (int)gid.y - 1 + j, 0), sh - 1);
        float4 row = float4(0.0f, 0.0f, 0.0f, 0.0f);
        for (int i = 0; i < 4; ++i) {
            int xx = min(max(2 * (int)gid.x - 1 + i, 0), sw - 1);
            row = row + in0[yy * sw + xx] * k[i];
        }
        acc = acc + row * k[j];
    }
    out[gid.y * P.w + gid.x] = acc;
}

// bilinear upsample (in0: sw x sh -> out: w x h)
kernel void KFUp(device const float4* in0 [[buffer(0)]], device const float4* in1 [[buffer(1)]],
                 device const float4* in2 [[buffer(2)]], device const float4* in3 [[buffer(3)]],
                 device float4* out [[buffer(4)]], constant KFOpParams& P [[buffer(5)]], constant float* kw [[buffer(6)]],
                 uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= P.w || gid.y >= P.h) return;
    float sx = (float)P.sw / (float)P.w, sy = (float)P.sh / (float)P.h;
    out[gid.y * P.w + gid.x] = kf_bil(in0, (int)P.sw, (int)P.sh, ((float)gid.x + 0.5f) * sx, ((float)gid.y + 0.5f) * sy);
}

// separable gaussian, clamp to edge.  i[0] = radius, kw = weights (2 radius + 1)
kernel void KFGaussH(device const float4* in0 [[buffer(0)]], device const float4* in1 [[buffer(1)]],
                     device const float4* in2 [[buffer(2)]], device const float4* in3 [[buffer(3)]],
                     device float4* out [[buffer(4)]], constant KFOpParams& P [[buffer(5)]], constant float* kw [[buffer(6)]],
                     uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= P.w || gid.y >= P.h) return;
    int r = P.i[0], w = (int)P.w;
    float4 acc = float4(0.0f, 0.0f, 0.0f, 0.0f);
    for (int i = -r; i <= r; ++i) {
        int xx = min(max((int)gid.x + i, 0), w - 1);
        acc = acc + in0[gid.y * P.w + (uint)xx] * kw[i + r];
    }
    out[gid.y * P.w + gid.x] = acc;
}

kernel void KFGaussV(device const float4* in0 [[buffer(0)]], device const float4* in1 [[buffer(1)]],
                     device const float4* in2 [[buffer(2)]], device const float4* in3 [[buffer(3)]],
                     device float4* out [[buffer(4)]], constant KFOpParams& P [[buffer(5)]], constant float* kw [[buffer(6)]],
                     uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= P.w || gid.y >= P.h) return;
    int r = P.i[0], h = (int)P.h;
    float4 acc = float4(0.0f, 0.0f, 0.0f, 0.0f);
    for (int j = -r; j <= r; ++j) {
        int yy = min(max((int)gid.y + j, 0), h - 1);
        acc = acc + in0[(uint)yy * P.w + gid.x] * kw[j + r];
    }
    out[gid.y * P.w + gid.x] = acc;
}

// weighted line of taps along a direction.  f: 0 cos, 1 sin, 2 length in pixels.  i[0] = taps
kernel void KFDir(device const float4* in0 [[buffer(0)]], device const float4* in1 [[buffer(1)]],
                  device const float4* in2 [[buffer(2)]], device const float4* in3 [[buffer(3)]],
                  device float4* out [[buffer(4)]], constant KFOpParams& P [[buffer(5)]], constant float* kw [[buffer(6)]],
                  uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= P.w || gid.y >= P.h) return;
    int N = P.i[0];
    float cs = P.f[0], sn = P.f[1], L = P.f[2];
    float4 acc = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float ws = 0.0f;
    for (int i = 0; i < N; ++i) {
        float t = ((float)i / (float)(N - 1)) * 2.0f - 1.0f;
        float wt = precise::exp(-2.0f * t * t);
        acc = acc + kf_bil(in0, (int)P.w, (int)P.h, (float)gid.x + 0.5f + cs * t * L * 0.5f, (float)gid.y + 0.5f + sn * t * L * 0.5f) * wt;
        ws += wt;
    }
    out[gid.y * P.w + gid.x] = acc / ws;
}
)MSL";

// ---------------------------------------------------------------- group EFF: lens blur, vignette, glow pieces
static const char* kKFEffMSL = R"MSL(
// glow pyramid: out = bilinear(in0 (sw x sh)) + in1 (w x h)
kernel void KFUpAdd(device const float4* in0 [[buffer(0)]], device const float4* in1 [[buffer(1)]],
                    device const float4* in2 [[buffer(2)]], device const float4* in3 [[buffer(3)]],
                    device float4* out [[buffer(4)]], constant KFOpParams& P [[buffer(5)]], constant float* kw [[buffer(6)]],
                    uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= P.w || gid.y >= P.h) return;
    float sx = (float)P.sw / (float)P.w, sy = (float)P.sh / (float)P.h;
    uint i = gid.y * P.w + gid.x;
    out[i] = kf_bil(in0, (int)P.sw, (int)P.sh, ((float)gid.x + 0.5f) * sx, ((float)gid.y + 0.5f) * sy) + in1[i];
}

// Soft lens blur: blend between the sharp picture (in0) and three pre-blurred copies (in1..in3) by zone weight.
// f: 0 cx, 1 cy, 2 roundness, 3/4 zone start/full.  i[0] = zone everywhere
kernel void KFLbSoft(device const float4* in0 [[buffer(0)]], device const float4* in1 [[buffer(1)]],
                     device const float4* in2 [[buffer(2)]], device const float4* in3 [[buffer(3)]],
                     device float4* out [[buffer(4)]], constant KFOpParams& P [[buffer(5)]], constant float* kw [[buffer(6)]],
                     uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= P.w || gid.y >= P.h) return;
    uint i = gid.y * P.w + gid.x;
    float d = kf_zdist((float)gid.x + 0.5f, (float)gid.y + 0.5f, P.f[0], P.f[1], (float)P.w, (float)P.h, P.f[2]);
    float f = kf_zw(d, P.f[3], P.f[4], P.i[0]) * 3.0f;
    int lv = min((int)f, 2);
    float t = f - (float)lv;
    float4 a = in0[i], b = in1[i];
    if (lv == 1) { a = in1[i]; b = in2[i]; }
    else if (lv == 2) { a = in2[i]; b = in3[i]; }
    out[i] = kf_lerp4(a, b, t);
}

// Radial / Swirl / Directional lens blur, sampled from the sharp picture (in0).
// f: 0 Lmax, 1 cos(angle), 2 sin(angle), 3 cx, 4 cy, 5 roundness, 6/7 zone start/full.  i: 0 type (1 radial, 2 swirl, 3 directional), 1 zone everywhere
kernel void KFLbMove(device const float4* in0 [[buffer(0)]], device const float4* in1 [[buffer(1)]],
                     device const float4* in2 [[buffer(2)]], device const float4* in3 [[buffer(3)]],
                     device float4* out [[buffer(4)]], constant KFOpParams& P [[buffer(5)]], constant float* kw [[buffer(6)]],
                     uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= P.w || gid.y >= P.h) return;
    int w = (int)P.w, h = (int)P.h;
    uint idx = gid.y * P.w + gid.x;
    float px = (float)gid.x + 0.5f, py = (float)gid.y + 0.5f;
    float cx = P.f[3], cy = P.f[4];
    float L = P.f[0] * kf_zw(kf_zdist(px, py, cx, cy, (float)w, (float)h, P.f[5]), P.f[6], P.f[7], P.i[1]);
    if (L < 0.4f) { out[idx] = in0[idx]; return; }
    float dx = px - cx, dy = py - cy;
    float r = sqrt(dx * dx + dy * dy);
    float rx = r > 1e-3f ? dx / r : 1.0f, ry = r > 1e-3f ? dy / r : 0.0f;
    int N = min(129, max(3, ((int)(L * 1.2f)) | 1));
    float4 acc = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float ws = 0.0f;
    for (int i = 0; i < N; ++i) {
        float t = ((float)i / (float)(N - 1)) * 2.0f - 1.0f;
        float wt = precise::exp(-2.0f * t * t);
        float sx, sy;
        if (P.i[0] == 1) {
            sx = px + rx * t * L * 0.5f; sy = py + ry * t * L * 0.5f;
        } else if (P.i[0] == 2) {
            float th = min(max(t * L * 0.5f / max(r, 1.0f), -0.8f), 0.8f);
            float c = precise::cos(th), s = precise::sin(th);
            sx = cx + dx * c - dy * s; sy = cy + dx * s + dy * c;
        } else {
            sx = px + P.f[1] * t * L * 0.5f; sy = py + P.f[2] * t * L * 0.5f;
        }
        acc = acc + kf_bil(in0, w, h, sx, sy) * wt;
        ws += wt;
    }
    out[idx] = acc / ws;
}

// soft-knee highlight extraction (RGB, alpha 0).  f[0] = threshold
kernel void KFMask(device const float4* in0 [[buffer(0)]], device const float4* in1 [[buffer(1)]],
                   device const float4* in2 [[buffer(2)]], device const float4* in3 [[buffer(3)]],
                   device float4* out [[buffer(4)]], constant KFOpParams& P [[buffer(5)]], constant float* kw [[buffer(6)]],
                   uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= P.w || gid.y >= P.h) return;
    uint i = gid.y * P.w + gid.x;
    float4 s = in0[i];
    float knee = 0.12f;
    float v0 = s.x - P.f[0], v1 = s.y - P.f[0], v2 = s.z - P.f[0];
    float m0 = v0 <= -knee ? 0.0f : (v0 < knee ? (v0 + knee) * (v0 + knee) / (4.0f * knee) : v0);
    float m1 = v1 <= -knee ? 0.0f : (v1 < knee ? (v1 + knee) * (v1 + knee) / (4.0f * knee) : v1);
    float m2 = v2 <= -knee ? 0.0f : (v2 < knee ? (v2 + knee) * (v2 + knee) / (4.0f * knee) : v2);
    out[i] = float4(m0, m1, m2, 0.0f);
}

// picture (in0) + glow (in1) * intensity * tint * zone weight.   f: 0 intensity (already times the pyramid scale), 1..3 tint, 4/5 zone start/full, 6 cx, 7 cy, 8 roundness.  i[0] = zone everywhere
kernel void KFAddGlow(device const float4* in0 [[buffer(0)]], device const float4* in1 [[buffer(1)]],
                      device const float4* in2 [[buffer(2)]], device const float4* in3 [[buffer(3)]],
                      device float4* out [[buffer(4)]], constant KFOpParams& P [[buffer(5)]], constant float* kw [[buffer(6)]],
                      uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= P.w || gid.y >= P.h) return;
    uint i = gid.y * P.w + gid.x;
    float wz = kf_zw(kf_zdist((float)gid.x + 0.5f, (float)gid.y + 0.5f, P.f[6], P.f[7], (float)P.w, (float)P.h, P.f[8]), P.f[4], P.f[5], P.i[0]);
    float4 c = in0[i], g = in1[i];
    out[i] = float4(c.x + g.x * P.f[0] * P.f[1] * wz, c.y + g.y * P.f[0] * P.f[2] * wz, c.z + g.z * P.f[0] * P.f[3] * wz, c.w);
}

// vignette on the linear picture.  f: 0 amount, 1 power, 2/3 zone start/full, 4 cx, 5 cy, 6 roundness.  i[0] = zone everywhere
kernel void KFVig(device const float4* in0 [[buffer(0)]], device const float4* in1 [[buffer(1)]],
                  device const float4* in2 [[buffer(2)]], device const float4* in3 [[buffer(3)]],
                  device float4* out [[buffer(4)]], constant KFOpParams& P [[buffer(5)]], constant float* kw [[buffer(6)]],
                  uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= P.w || gid.y >= P.h) return;
    uint i = gid.y * P.w + gid.x;
    float d = kf_zdist((float)gid.x + 0.5f, (float)gid.y + 0.5f, P.f[4], P.f[5], (float)P.w, (float)P.h, P.f[6]);
    float full = max(P.f[3], P.f[2] + 0.002f);
    float t = P.i[0] != 0 ? 1.0f : kf_clamp01((d - P.f[2]) / (full - P.f[2]));
    float g = exp2(-P.f[0] * precise::pow(t, P.f[1]));
    float4 c = in0[i];
    out[i] = float4(c.x * g, c.y * g, c.z * g, c.w);
}
)MSL";
