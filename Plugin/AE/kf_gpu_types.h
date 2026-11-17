// Kaptains Film Look - Copyright (C) 2026 Henrik Jonas Kaptain
// SPDX-License-Identifier: GPL-3.0-or-later (with an additional permission for the Adobe SDKs, see LICENSE-EXCEPTION)
// Kaptains Film Look - GPU types shared by every host (no Adobe SDK headers in here, so the Premiere and After Effects files can both use it).
#pragma once
// ---- steps 2a/2b: one frame kernel does Input Space decode -> Vignette (zone) -> Grain (zone) -> Mix -> encode, no CPU stage ----
// The field lists are the single source of truth: they make the C++ structs here AND the struct text prepended to the Metal source.
#include <vector>
#define KF_FRAME_FIELDS(X) \
    X(unsigned int, width) X(unsigned int, height) X(unsigned int, srcPitch) X(unsigned int, dstPitch) \
    X(int, inputSpace) /* 0 linear, 1 sRGB, 2 gamma 2.4 */ \
    X(int, applyVig) X(int, zoneEverywhere) \
    X(float, mix) X(float, amount) X(float, softness) X(float, zoneStart) X(float, zoneFull) \
    X(float, cx) X(float, cy) X(float, roundness) \
    X(int, applyGrain) X(int, gEverywhere) X(int, gColor) \
    X(float, gAmp0) X(float, gBias) X(float, gWs) X(float, gWi) X(float, gZoneStart) X(float, gZoneFull) \
    X(int, hasCur) /* 1: the picture comes from the linear working buffer `cur` (heavy chain), not from the source */

#define KF_NOISE_FIELDS(X) \
    X(unsigned int, width) X(unsigned int, height) X(unsigned int, radius) X(unsigned int, seed) \
    X(int, accum) X(float, weight) X(float, norm)

#define KF_FIELD_DECL(t, n) t n;
struct KFGpuFrameParams { KF_FRAME_FIELDS(KF_FIELD_DECL) };
struct KFGpuNoiseParams { KF_NOISE_FIELDS(KF_FIELD_DECL) };
#undef KF_FIELD_DECL

// One step of the grain noise build-up (see kf_gpu_plan.h). kernel: 0 direct noise, 1 horizontal blur of noise, 2 vertical blur.
struct KFGpuPass {
    int kernel;
    int layer;                 // which of the 4 noise-field buffers receives the result (V / direct pass) or reads (V pass)
    int hb_rows;               // rows of the horizontal-blur scratch buffer this pass needs (kernel 1)
    KFGpuNoiseParams np;
    std::vector<float> kw;     // gaussian weights (kernels 1 and 2)
};

// ---- step 3: the heavy effects (Lens, Chromatic Aberration, Lens Blur, Bloom, Halation) as a list of small kernel launches ----
// Working buffers are float4 R,G,B,A, STRAIGHT alpha, scene-linear, tightly packed (the same data layout the CPU core uses).
#define KF_OP_FIELDS(X) \
    X(unsigned int, w) X(unsigned int, h) X(unsigned int, sw) X(unsigned int, sh) X(unsigned int, srcPitch) X(unsigned int, dstPitch) \
    X(int, i[4]) X(float, f[16])
#define KF_FIELD_DECL(t, n) t n;
struct KFOpParams { KF_OP_FIELDS(KF_FIELD_DECL) };
#undef KF_FIELD_DECL

enum KFKernelId { KF_K_DECODE = 0, KF_K_GEO, KF_K_CADIFF, KF_K_CAADD,                      // group GEO
                  KF_K_DOWN, KF_K_UP, KF_K_GAUSSH, KF_K_GAUSSV, KF_K_DIR,                  // group BLUR
                  KF_K_UPADD, KF_K_LBSOFT, KF_K_LBMOVE, KF_K_MASK, KF_K_ADDGLOW, KF_K_VIG, // group EFF
                  KF_K_COUNT };
enum { KF_GRP_GEO = 1, KF_GRP_BLUR = 2, KF_GRP_EFF = 4, KF_GRP_ALL = 7 };

struct KFOp {
    int kernel;
    int in[4];                 // buffer ids, -1 = unused. 0 = the After Effects source, 2.. = working buffers
    int out;                   // working buffer id
    unsigned gw, gh;           // grid size
    KFOpParams P;
    std::vector<float> kw;     // gaussian weights (GAUSSH / GAUSSV)
};
struct KFBufDim { int w, h; };
struct KFPlan {
    std::vector<KFBufDim> bufs;   // index = buffer id; 0 and 1 (source / destination) are placeholders
    std::vector<KFOp> ops;
    int cur = -1;                 // working buffer that holds the finished linear picture
    unsigned need = 0;            // kernel groups the plan uses
};


// One parameter value as read from Premiere (see KaptainFilmPremiere.mm). type: 0 missing, 1 number (bool / float), 2 point (x, y, normalised 0..1), 3 integer (num = value, raw = the same bits; colours arrive this way)
struct KFPremValue { int type; double num; double x, y; unsigned long long raw; };

// Premiere bridge (implemented in KaptainFilm.cpp, which knows the parameter list). `v` = the effect's parameters in Premiere order
// (input frame excluded), `count` of them. Fills everything kf_gpu_exec needs. Returns false when this frame must not be done by the
// GPU path (Show Zones on, unknown parameter layout, a value of an unexpected type, missing kernel group): Premiere then uses the CPU path.
// use_plan = the heavy chain is needed (`plan` is filled), otherwise just the light frame kernel.
bool kf_prem_prepare(const KFPremValue* v, int count, int w, int h, int frame, unsigned groups,
                     KFGpuFrameParams& fp, std::vector<KFGpuPass>& passes, KFPlan& plan, bool& use_plan);

// ---- the Metal executor (KaptainFilmGPU.mm), host independent ----
// `data` = opaque per-device object (compiled pipelines). All pointers are id<MTLDevice>, id<MTLCommandQueue>, id<MTLBuffer> as void*.
void* kf_gpu_make_data(void* mtl_device);                 // nullptr when the basic kernels do not compile
void kf_gpu_free_data(void* data);
unsigned kf_gpu_data_groups(void* data);                  // KF_GRP_* bits
// src / dst: the host's pixel buffers (float4 B,G,R,A premultiplied, or half4 when `half_pixels`), pitches in pixels. src may equal dst.
// returns 0 = ok, 1 = out of memory, 2 = failed
int kf_gpu_exec(void* data, void* mtl_device, void* mtl_queue, void* src, void* dst, KFGpuFrameParams params,
                const std::vector<KFGpuPass>& passes, const KFPlan* plan, int half_pixels);
