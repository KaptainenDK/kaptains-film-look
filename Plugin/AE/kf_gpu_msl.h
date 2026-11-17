// Kaptains Film Look - Copyright (C) 2026 Henrik Jonas Kaptain
// SPDX-License-Identifier: GPL-3.0-or-later (with an additional permission for the Adobe SDKs, see LICENSE-EXCEPTION)
// Kaptains Film Look - Metal kernels (text). The struct KFGpuFrameParams is prepended from KF_FRAME_FIELDS at start-up.
// This text is also compiled as plain C++ by tests/gpu_kernel_test.cpp and compared with the CPU core.
#pragma once
static const char* kKFKernelMSL = R"MSL(
struct KFCopyParams { uint width; uint height; uint srcPitch; uint dstPitch; };
kernel void KFCopyKernel(device const float4* src [[buffer(0)]],
                         device float4* dst [[buffer(1)]],
                         constant KFCopyParams& p [[buffer(2)]],
                         uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= p.width || gid.y >= p.height) return;
    dst[gid.y * p.dstPitch + gid.x] = src[gid.y * p.srcPitch + gid.x];
}

// Premiere 16-bit float GPU frames <-> float4 working frames (B,G,R,A order is kept)
kernel void KFHalfToFloat(device const half4* src [[buffer(0)]],
                          device float4* dst [[buffer(1)]],
                          constant KFCopyParams& p [[buffer(2)]],
                          uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= p.width || gid.y >= p.height) return;
    half4 h = src[gid.y * p.srcPitch + gid.x];
    dst[gid.y * p.dstPitch + gid.x] = float4(h.x, h.y, h.z, h.w);
}
kernel void KFFloatToHalf(device const float4* src [[buffer(0)]],
                          device half4* dst [[buffer(1)]],
                          constant KFCopyParams& p [[buffer(2)]],
                          uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= p.width || gid.y >= p.height) return;
    float4 f = src[gid.y * p.srcPitch + gid.x];
    dst[gid.y * p.dstPitch + gid.x] = half4(f.x, f.y, f.z, f.w);
}

float kf_dec(float v, int sp)
{
    if (sp == 1) {
        float s = v < 0.0f ? -1.0f : 1.0f;
        float a = fabs(v);
        return s * (a <= 0.04045f ? a / 12.92f : precise::pow((a + 0.055f) / 1.055f, 2.4f));
    }
    if (sp == 2) return v < 0.0f ? -precise::pow(-v, 2.4f) : precise::pow(v, 2.4f);
    return v;
}

float kf_enc(float v, int sp)
{
    if (sp == 1) {
        float s = v < 0.0f ? -1.0f : 1.0f;
        float a = fabs(v);
        return s * (a <= 0.0031308f ? a * 12.92f : 1.055f * precise::pow(a, 1.0f / 2.4f) - 0.055f);
    }
    if (sp == 2) return v < 0.0f ? -precise::pow(-v, 1.0f / 2.4f) : precise::pow(v, 1.0f / 2.4f);
    return v;
}

// ---- grain noise (mirrors gauss_noise / noise_field in kf_core.cpp) ----
uint kf_hash32(uint x)
{
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
    return x;
}

float kf_gauss_noise(uint x, uint y, uint seed)
{
    uint h1 = kf_hash32(x * 0x9E3779B1u ^ kf_hash32(y + seed * 0x85ebca6bu));
    uint h2 = kf_hash32(h1 + 0x68bc21ebu);
    float u1 = (float)(h1 >> 8) * (1.0f / 16777216.0f) + 1e-7f;
    float u2 = (float)(h2 >> 8) * (1.0f / 16777216.0f);
    return precise::sqrt(-2.0f * precise::log(u1)) * precise::cos(6.2831853f * u2);
}

// sigma < 0.05: the raw noise field
kernel void KFNoiseDirect(device float* out [[buffer(0)]],
                          constant KFGpuNoiseParams& P [[buffer(2)]],
                          uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= P.width || gid.y >= P.height) return;
    float v = P.weight * kf_gauss_noise(gid.x, gid.y, P.seed);
    uint i = gid.y * P.width + gid.x;
    out[i] = (P.accum != 0 ? out[i] : 0.0f) + v;
}

// Horizontal gaussian over the padded noise; the padded noise is generated on the fly. Grid: width x (height + 2 radius).
kernel void KFNoiseH(device float* hb [[buffer(0)]],
                     constant float* kw [[buffer(1)]],
                     constant KFGpuNoiseParams& P [[buffer(2)]],
                     uint2 gid [[thread_position_in_grid]])
{
    int r = (int)P.radius;
    if (gid.x >= P.width || gid.y >= P.height + 2u * P.radius) return;
    uint ny = (uint)((int)gid.y - r + 100000);
    float acc = 0.0f;
    for (int i = -r; i <= r; ++i)
        acc += kw[i + r] * kf_gauss_noise((uint)((int)gid.x + i + 100000), ny, P.seed);
    hb[gid.y * P.width + gid.x] = acc;
}

// Vertical gaussian over the horizontal result, scaled to unit variance, weighted and accumulated into the layer field.
kernel void KFNoiseV(device float* out [[buffer(0)]],
                     device const float* hb [[buffer(1)]],
                     constant KFGpuNoiseParams& P [[buffer(2)]],
                     constant float* kw [[buffer(3)]],
                     uint2 gid [[thread_position_in_grid]])
{
    int r = (int)P.radius;
    if (gid.x >= P.width || gid.y >= P.height) return;
    float acc = 0.0f;
    for (int j = -r; j <= r; ++j)
        acc += kw[j + r] * hb[(gid.y + (uint)(r + j)) * P.width + gid.x];
    uint i = gid.y * P.width + gid.x;
    out[i] = (P.accum != 0 ? out[i] : 0.0f) + P.weight * acc * P.norm;
}

// Pixels are float4 in B, G, R, A order, premultiplied (After Effects GPU worlds).
// cur (hasCur != 0): linear R,G,B,A straight-alpha picture after the heavy effects; the source is then only used for the Mix reference.
// fieldS = shared noise, fieldR / fieldG / fieldB = independent noise per channel (only read when gColor != 0).
kernel void KFFrameKernel(device const float4* src [[buffer(0)]],
                          device float4* dst [[buffer(1)]],
                          constant KFGpuFrameParams& P [[buffer(2)]],
                          device const float* fieldS [[buffer(3)]],
                          device const float* fieldR [[buffer(4)]],
                          device const float* fieldG [[buffer(5)]],
                          device const float* fieldB [[buffer(6)]],
                          device const float4* cur [[buffer(7)]],
                          uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= P.width || gid.y >= P.height) return;
    float4 s = src[gid.y * P.srcPitch + gid.x];
    if (P.applyVig == 0 && P.applyGrain == 0 && P.hasCur == 0) { dst[gid.y * P.dstPitch + gid.x] = s; return; }

    float a = s.w;
    float r = s.z, g = s.y, b = s.x;
    if (a > 0.0f && a < 1.0f) { r /= a; g /= a; b /= a; }              // straight colour, like the core
    float lr = kf_dec(r, P.inputSpace), lg = kf_dec(g, P.inputSpace), lb = kf_dec(b, P.inputSpace);
    float pr = lr, pg = lg, pb = lb;
    float ao = a;
    if (P.hasCur != 0) {                                                 // heavy chain: picture + alpha come from the linear buffer
        float4 c = cur[gid.y * P.width + gid.x];
        pr = c.x; pg = c.y; pb = c.z;
        ao = a + (c.w - a) * P.mix;
    }

    float hw = max(0.5f * (float)P.width, 1.0f), hh = max(0.5f * (float)P.height, 1.0f);
    float Ra = sqrt(hw * hw + hh * hh);
    float dx = ((float)gid.x + 0.5f) - P.cx, dy = ((float)gid.y + 0.5f) - P.cy;
    float dc = sqrt(dx * dx + dy * dy) / Ra;
    float ex = dx / hw, ey = dy / hh;
    float doval = sqrt(ex * ex + ey * ey) * 0.70710678f;
    float d = doval + (dc - doval) * P.roundness;

    if (P.applyVig != 0) {                                               // 4: vignette
        float full = max(P.zoneFull, P.zoneStart + 0.002f);
        float t = 1.0f;
        if (P.zoneEverywhere == 0) t = min(max((d - P.zoneStart) / (full - P.zoneStart), 0.0f), 1.0f);
        float pw = 1.0f + 3.0f * (1.0f - min(max(P.softness, 0.0f), 1.0f));
        float gain = exp2(-P.amount * precise::pow(t, pw));
        pr *= gain; pg *= gain; pb *= gain;
    }

    if (P.applyGrain != 0) {                                             // 7: grain, applied in sRGB-encoded values
        float l = 0.2126f * pr + 0.7152f * pg + 0.0722f * pb;
        float lp = min(max(kf_enc(max(l, 0.0f), 1), 0.0f), 1.0f);
        float zw = 1.0f;
        if (P.gEverywhere == 0) {
            float gfull = max(P.gZoneFull, P.gZoneStart + 0.002f);
            float gt = min(max((d - P.gZoneStart) / (gfull - P.gZoneStart), 0.0f), 1.0f);
            zw = gt * gt * (3.0f - 2.0f * gt);
        }
        float amp = P.gAmp0 * (1.0f + (4.0f * lp * (1.0f - lp) - 1.0f) * P.gBias) * zw;
        uint fi = gid.y * P.width + gid.x;
        float nS = fieldS[fi];
        float nr = P.gWs * nS, ng = nr, nb = nr;
        if (P.gColor != 0) { nr += P.gWi * fieldR[fi]; ng += P.gWi * fieldG[fi]; nb += P.gWi * fieldB[fi]; }
        pr = max(0.0f, kf_dec(kf_enc(pr, 1) + nr * amp, 1));
        pg = max(0.0f, kf_dec(kf_enc(pg, 1) + ng * amp, 1));
        pb = max(0.0f, kf_dec(kf_enc(pb, 1) + nb * amp, 1));
    }

    float m = P.mix;
    float orr = lr + (pr - lr) * m, og = lg + (pg - lg) * m, ob = lb + (pb - lb) * m;
    orr = kf_enc(orr, P.inputSpace); og = kf_enc(og, P.inputSpace); ob = kf_enc(ob, P.inputSpace);
    dst[gid.y * P.dstPitch + gid.x] = float4(ob * ao, og * ao, orr * ao, ao);
}
)MSL";
