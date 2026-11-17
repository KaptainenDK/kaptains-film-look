// Kaptains Film Look - Copyright (C) 2026 Henrik Jonas Kaptain
// SPDX-License-Identifier: GPL-3.0-or-later (with an additional permission for the Adobe SDKs, see LICENSE-EXCEPTION)
// Kaptains Film Look - Metal side of the After Effects GPU path (macOS only). Compiled as Objective-C++ with ARC.
#include "KaptainFilmGPU.h"
#include "kf_gpu_msl.h"
#include "kf_gpu_msl_heavy.h"
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include <new>
#include <cstring>
#include <algorithm>

// Struct text generated from the same field list as the C++ struct, so host and kernel can never disagree.
#define KF_FIELD_MSL(t, n) #t " " #n ";\n"
static NSString* kernel_source()
{
    NSString* head = @"#include <metal_stdlib>\nusing namespace metal;\n";
    NSString* f1 = [NSString stringWithUTF8String:KF_FRAME_FIELDS(KF_FIELD_MSL)];
    NSString* f2 = [NSString stringWithUTF8String:KF_NOISE_FIELDS(KF_FIELD_MSL)];
    return [NSString stringWithFormat:@"%@struct KFGpuFrameParams {\n%@};\nstruct KFGpuNoiseParams {\n%@};\n%s", head, f1, f2, kKFKernelMSL];
}

// Prelude for the heavy-effect libraries: struct text from the shared field list + the common helpers
static NSString* heavy_source(const char* group_text)
{
    NSString* head = @"#include <metal_stdlib>\nusing namespace metal;\n";
    NSString* f = [NSString stringWithUTF8String:KF_OP_FIELDS(KF_FIELD_MSL)];
    return [NSString stringWithFormat:@"%@struct KFOpParams {\n%@};\n%s\n%s", head, f, kKFHeavyCommonMSL, group_text];
}

struct KFCopyParams { uint32_t width, height, srcPitch, dstPitch; };   // same layout as the one in the Metal text

struct KFMetalData {
    id<MTLComputePipelineState> copy_pipeline;
    id<MTLComputePipelineState> frame_pipeline;
    id<MTLComputePipelineState> noise_direct, noise_h, noise_v;
    id<MTLComputePipelineState> h2f, f2h;               // Premiere 16-bit float frames <-> float4 working frames
    id<MTLComputePipelineState> heavy[KF_K_COUNT];     // nil when the group failed to compile
    unsigned groups;                                   // KF_GRP_* bits that are fully usable
};

static const struct { const char* name; unsigned group; } kHeavyKernels[KF_K_COUNT] = {
    {"KFHDecode", KF_GRP_GEO}, {"KFGeo", KF_GRP_GEO}, {"KFCaDiff", KF_GRP_GEO}, {"KFCaAdd", KF_GRP_GEO},
    {"KFDown", KF_GRP_BLUR}, {"KFUp", KF_GRP_BLUR}, {"KFGaussH", KF_GRP_BLUR}, {"KFGaussV", KF_GRP_BLUR}, {"KFDir", KF_GRP_BLUR},
    {"KFUpAdd", KF_GRP_EFF}, {"KFLbSoft", KF_GRP_EFF}, {"KFLbMove", KF_GRP_EFF}, {"KFMask", KF_GRP_EFF}, {"KFAddGlow", KF_GRP_EFF}, {"KFVig", KF_GRP_EFF}};

// Each group is its own library: a compile error in one cannot disable the others (or the older paths).
static void build_heavy_group(id<MTLDevice> device, KFMetalData* d, unsigned group, const char* text, const char* label)
{
    NSError* error = nil;
    id<MTLLibrary> lib = [device newLibraryWithSource:heavy_source(text) options:nil error:&error];
    if (!lib) { NSLog(@"KaptainsFilmLook GPU: kernel group %s did not compile: %@", label, error.localizedDescription); return; }
    bool all = true;
    for (int k = 0; k < KF_K_COUNT; ++k) {
        if (kHeavyKernels[k].group != group) continue;
        id<MTLFunction> fn = [lib newFunctionWithName:[NSString stringWithUTF8String:kHeavyKernels[k].name]];
        id<MTLComputePipelineState> ps = fn ? [device newComputePipelineStateWithFunction:fn error:&error] : nil;
        if (!ps) { NSLog(@"KaptainsFilmLook GPU: kernel %s failed: %@", kHeavyKernels[k].name, error.localizedDescription); all = false; continue; }
        d->heavy[k] = ps;
    }
    if (all) d->groups |= group;
}

// AE does not provide an autorelease pool on its render threads.
#define KF_POOL @autoreleasepool

void* kf_gpu_make_data(void* mtl_device)
{
    void* result = nullptr;
    KF_POOL {
        id<MTLDevice> device = (__bridge id<MTLDevice>)mtl_device;
        if (!device) return nullptr;
        NSError* error = nil;
        id<MTLLibrary> lib = [device newLibraryWithSource:kernel_source() options:nil error:&error];
        if (!lib) { NSLog(@"KaptainsFilmLook GPU: Metal kernel did not compile: %@", error.localizedDescription); return nullptr; }
        auto make = [&](const char* name) -> id<MTLComputePipelineState> {
            id<MTLFunction> fn = [lib newFunctionWithName:[NSString stringWithUTF8String:name]];
            id<MTLComputePipelineState> ps = fn ? [device newComputePipelineStateWithFunction:fn error:&error] : nil;
            if (!ps) NSLog(@"KaptainsFilmLook GPU: kernel %s failed: %@", name, error.localizedDescription);
            return ps;
        };
        id<MTLComputePipelineState> pc = make("KFCopyKernel"), pf = make("KFFrameKernel");
        id<MTLComputePipelineState> nd = make("KFNoiseDirect"), nh = make("KFNoiseH"), nv = make("KFNoiseV");
        if (!pc || !pf || !nd || !nh || !nv) return nullptr;
        KFMetalData* d = new (std::nothrow) KFMetalData();
        if (!d) return nullptr;
        d->copy_pipeline = pc; d->frame_pipeline = pf;
        d->noise_direct = nd; d->noise_h = nh; d->noise_v = nv;
        d->h2f = make("KFHalfToFloat"); d->f2h = make("KFFloatToHalf");     // only Premiere 16-bit frames need these
        d->groups = 0;
        build_heavy_group(device, d, KF_GRP_GEO, kKFGeoMSL, "lens/CA");
        build_heavy_group(device, d, KF_GRP_BLUR, kKFBlurMSL, "blur");
        build_heavy_group(device, d, KF_GRP_EFF, kKFEffMSL, "lens blur/vignette/glow");
        // numbers, not words: the macOS log hides strings
        NSLog(@"KaptainsFilmLook GPU: heavy-effect kernel groups (1 = ready): lens/CA %d, blur %d, lens blur/glow %d",
              (d->groups & KF_GRP_GEO) ? 1 : 0, (d->groups & KF_GRP_BLUR) ? 1 : 0, (d->groups & KF_GRP_EFF) ? 1 : 0);
        NSLog(@"KaptainsFilmLook GPU: Metal device ready (%@)", device.name);
        result = d;
    }
    return result;
}

void kf_gpu_free_data(void* data)
{
    if (data) { KF_POOL { delete static_cast<KFMetalData*>(data); } }
}

unsigned kf_gpu_data_groups(void* data)
{
    return data ? static_cast<KFMetalData*>(data)->groups : 0;
}

PF_Err kf_gpu_device_setup(PF_InData* in_data, PF_OutData* out_data, PF_GPUDeviceSetupExtra* extra)
{
    if (!extra || !extra->input || !extra->output) return PF_Err_NONE;
    if (extra->input->what_gpu != PF_GPU_Framework_METAL) return PF_Err_NONE;   // only Metal is implemented
    PF_Err err = PF_Err_NONE;
    KF_POOL {
        PF_GPUDeviceSuite1* gs = nullptr;
        if (in_data->pica_basicP->AcquireSuite(kPFGPUDeviceSuite, kPFGPUDeviceSuiteVersion1, (const void**)&gs) != 0 || !gs)
            return PF_Err_UNRECOGNIZED_PARAM_TYPE;
        PF_GPUDeviceInfo info; memset(&info, 0, sizeof info);
        err = gs->GetDeviceInfo(in_data->effect_ref, extra->input->device_index, &info);
        in_data->pica_basicP->ReleaseSuite(kPFGPUDeviceSuite, kPFGPUDeviceSuiteVersion1);
        if (err) return err;
        if (!info.devicePV) return PF_Err_INTERNAL_STRUCT_DAMAGED;
        void* d = kf_gpu_make_data(info.devicePV);
        if (!d) return PF_Err_INTERNAL_STRUCT_DAMAGED;
        extra->output->gpu_data = d;
        out_data->out_flags2 = PF_OutFlag2_SUPPORTS_GPU_RENDER_F32;
    }
    return err;
}

PF_Err kf_gpu_device_setdown(PF_InData*, PF_OutData*, PF_GPUDeviceSetdownExtra* extra)
{
    if (extra && extra->input && extra->input->what_gpu == PF_GPU_Framework_METAL && extra->input->gpu_data)
        kf_gpu_free_data(extra->input->gpu_data);
    return PF_Err_NONE;
}

unsigned kf_gpu_groups(PF_SmartRenderExtra* extra)
{
    if (!extra || !extra->input || extra->input->what_gpu != PF_GPU_Framework_METAL) return 0;
    const KFMetalData* md = static_cast<const KFMetalData*>(extra->input->gpu_data);
    return md ? md->groups : 0;
}

static PF_Err encode_copy(id<MTLCommandQueue> queue, id<MTLComputePipelineState> pso, id<MTLBuffer> src, id<MTLBuffer> dst,
                          const KFCopyParams& p)
{
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    [enc setComputePipelineState:pso];
    [enc setBuffer:src offset:0 atIndex:0];
    [enc setBuffer:dst offset:0 atIndex:1];
    [enc setBytes:&p length:sizeof p atIndex:2];
    const NSUInteger tw = pso.threadExecutionWidth;
    MTLSize group = MTLSizeMake(tw, 16, 1);
    MTLSize groups = MTLSizeMake((p.width + tw - 1) / tw, (p.height + 15) / 16, 1);
    [enc dispatchThreadgroups:groups threadsPerThreadgroup:group];
    [enc endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    return cb.error ? PF_Err_INTERNAL_STRUCT_DAMAGED : PF_Err_NONE;
}

PF_Err kf_gpu_render(PF_InData* in_data, PF_OutData* out_data, PF_SmartRenderExtra* extra,
                     PF_EffectWorld* in_w, PF_EffectWorld* out_w, KFGpuCpuStage stage, void* user)
{
    (void)out_data;
    if (!extra || !extra->input || !in_w || !out_w || !stage) return PF_Err_BAD_CALLBACK_PARAM;
    if (extra->input->what_gpu != PF_GPU_Framework_METAL) return PF_Err_UNRECOGNIZED_PARAM_TYPE;
    const KFMetalData* md = static_cast<const KFMetalData*>(extra->input->gpu_data);
    if (!md) return PF_Err_INTERNAL_STRUCT_DAMAGED;

    PF_Err err = PF_Err_NONE;
    static bool logged_first = false;
    if (!logged_first) { logged_first = true; NSLog(@"KaptainsFilmLook GPU: first frame rendered through the Metal path"); }
    KF_POOL {
        PF_GPUDeviceSuite1* gs = nullptr;
        if (in_data->pica_basicP->AcquireSuite(kPFGPUDeviceSuite, kPFGPUDeviceSuiteVersion1, (const void**)&gs) != 0 || !gs)
            return PF_Err_UNRECOGNIZED_PARAM_TYPE;
        PF_GPUDeviceInfo info; memset(&info, 0, sizeof info);
        void* src_mem = nullptr; void* dst_mem = nullptr;
        err = gs->GetDeviceInfo(in_data->effect_ref, extra->input->device_index, &info);
        if (!err) err = gs->GetGPUWorldData(in_data->effect_ref, in_w, &src_mem);
        if (!err) err = gs->GetGPUWorldData(in_data->effect_ref, out_w, &dst_mem);
        in_data->pica_basicP->ReleaseSuite(kPFGPUDeviceSuite, kPFGPUDeviceSuiteVersion1);
        if (err) return err;

        id<MTLDevice> device = (__bridge id<MTLDevice>)info.devicePV;
        id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)info.command_queuePV;
        id<MTLBuffer> src = (__bridge id<MTLBuffer>)src_mem;
        id<MTLBuffer> dst = (__bridge id<MTLBuffer>)dst_mem;
        if (!device || !queue || !src || !dst) return PF_Err_INTERNAL_STRUCT_DAMAGED;

        const int sw = in_w->width, sh = in_w->height, dw = out_w->width, dh = out_w->height;
        const int bpp = 16;                                         // float4
        const uint32_t src_pitch = (uint32_t)(in_w->rowbytes / bpp), dst_pitch = (uint32_t)(out_w->rowbytes / bpp);
        if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return PF_Err_NONE;

        // Shared (CPU-visible) staging buffers: the same code works for any storage mode AE uses for its own buffers.
        id<MTLBuffer> in_stage = [device newBufferWithLength:(NSUInteger)sw * sh * bpp options:MTLResourceStorageModeShared];
        id<MTLBuffer> out_stage = [device newBufferWithLength:(NSUInteger)dw * dh * bpp options:MTLResourceStorageModeShared];
        if (!in_stage || !out_stage) return PF_Err_OUT_OF_MEMORY;

        // 1. GPU -> staging (Metal kernel)
        KFCopyParams p1 = {(uint32_t)sw, (uint32_t)sh, src_pitch, (uint32_t)sw};
        err = encode_copy(queue, md->copy_pipeline, src, in_stage, p1);
        // 2. the CPU core works on the staging copy
        if (!err) stage((const float*)[in_stage contents], sw, sh, sw, (float*)[out_stage contents], dw, dh, dw, user);
        // 3. staging -> GPU (Metal kernel)
        KFCopyParams p2 = {(uint32_t)dw, (uint32_t)dh, (uint32_t)dw, dst_pitch};
        if (!err) err = encode_copy(queue, md->copy_pipeline, out_stage, dst, p2);
    }
    return err;
}

// Steps 2a/2b: the whole frame on the GPU (no CPU stage). Only valid when input and output cover the same area.
static void dispatch2d(id<MTLComputeCommandEncoder> enc, id<MTLComputePipelineState> pso, NSUInteger w, NSUInteger h)
{
    const NSUInteger tw = pso.threadExecutionWidth;
    // heavy kernels use many registers: keep the threadgroup within what this pipeline allows
    NSUInteger th = pso.maxTotalThreadsPerThreadgroup / tw;
    th = th < 1 ? 1 : (th > 16 ? 16 : th);
    [enc dispatchThreadgroups:MTLSizeMake((w + tw - 1) / tw, (h + th - 1) / th, 1) threadsPerThreadgroup:MTLSizeMake(tw, th, 1)];
}

// The whole frame on the GPU. Shared by After Effects (kf_gpu_render_frame) and Premiere (KaptainFilmPremiere.mm).
int kf_gpu_exec(void* data, void* mtl_device, void* mtl_queue, void* src_mem, void* dst_mem, KFGpuFrameParams params,
                const std::vector<KFGpuPass>& passes, const KFPlan* plan, int half_pixels)
{
    const KFMetalData* md = static_cast<const KFMetalData*>(data);
    if (!md) return 2;
    static bool logged_first = false;
    if (!logged_first) { logged_first = true; NSLog(@"KaptainsFilmLook GPU: first frame rendered fully on the GPU (frame kernel)"); }
    static bool logged_heavy = false;
    if (plan && !logged_heavy) { logged_heavy = true; NSLog(@"KaptainsFilmLook GPU: first frame with heavy effects rendered fully on the GPU (%d launches)", (int)plan->ops.size()); }
    int rc = 0;
    KF_POOL {
        id<MTLDevice> device = (__bridge id<MTLDevice>)mtl_device;
        id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)mtl_queue;
        id<MTLBuffer> src = (__bridge id<MTLBuffer>)src_mem;
        id<MTLBuffer> dst = (__bridge id<MTLBuffer>)dst_mem;
        if (!device || !queue || !src || !dst) return 2;
        const NSUInteger W = params.width, H = params.height;
        if (W == 0 || H == 0) return 0;

        // Premiere 16-bit float frames: convert to a float4 working frame first and back at the end
        id<MTLBuffer> frame_in = src, frame_out = dst, tmp_in = nil, tmp_out = nil;
        const unsigned real_src_pitch = params.srcPitch, real_dst_pitch = params.dstPitch;
        if (half_pixels) {
            if (!md->h2f || !md->f2h) return 2;
            tmp_in = [device newBufferWithLength:W * H * 16 options:MTLResourceStorageModePrivate];
            tmp_out = [device newBufferWithLength:W * H * 16 options:MTLResourceStorageModePrivate];
            if (!tmp_in || !tmp_out) return 1;
            frame_in = tmp_in; frame_out = tmp_out;
            params.srcPitch = (unsigned)W; params.dstPitch = (unsigned)W;
        }

        // noise field buffers (GPU only): 0 = shared, 1..3 = independent R, G, B. Unused ones are bound to the source as a dummy.
        id<MTLBuffer> fields[4] = {frame_in, frame_in, frame_in, frame_in};
        int max_rows = 0;
        for (const KFGpuPass& p : passes) { if (p.kernel == 1) max_rows = std::max(max_rows, p.hb_rows); }
        for (const KFGpuPass& p : passes)
            if (fields[p.layer] == frame_in) fields[p.layer] = [device newBufferWithLength:W * H * sizeof(float) options:MTLResourceStorageModePrivate];
        id<MTLBuffer> hb = max_rows > 0 ? [device newBufferWithLength:W * (NSUInteger)max_rows * sizeof(float) options:MTLResourceStorageModePrivate] : nil;
        for (int k = 0; k < 4; ++k) if (!fields[k]) return 1;
        if (max_rows > 0 && !hb) return 1;

        // working buffers of the heavy chain (private GPU memory)
        std::vector<id<MTLBuffer>> work;
        if (plan) {
            work.assign(plan->bufs.size(), nil);
            for (size_t i = 2; i < plan->bufs.size(); ++i) {
                work[i] = [device newBufferWithLength:(NSUInteger)plan->bufs[i].w * plan->bufs[i].h * 16 options:MTLResourceStorageModePrivate];
                if (!work[i]) return 1;
            }
            for (const KFOp& o : plan->ops) if (!md->heavy[o.kernel]) return 2;
        }

        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
        if (half_pixels) {
            KFCopyParams cp = {(uint32_t)W, (uint32_t)H, real_src_pitch, (uint32_t)W};
            [enc setComputePipelineState:md->h2f];
            [enc setBuffer:src offset:0 atIndex:0]; [enc setBuffer:tmp_in offset:0 atIndex:1]; [enc setBytes:&cp length:sizeof cp atIndex:2];
            dispatch2d(enc, md->h2f, W, H);
        }
        if (plan) {
            for (const KFOp& o : plan->ops) {
                KFOpParams P = o.P;
                P.srcPitch = params.srcPitch;                                  // only the decode op reads the host source buffer
                id<MTLComputePipelineState> pso = md->heavy[o.kernel];
                [enc setComputePipelineState:pso];
                for (int k = 0; k < 4; ++k) {
                    id<MTLBuffer> b = o.in[k] >= 2 ? work[o.in[k]] : frame_in;   // unused slots get a harmless dummy
                    [enc setBuffer:b offset:0 atIndex:k];
                }
                [enc setBuffer:work[o.out] offset:0 atIndex:4];
                [enc setBytes:&P length:sizeof P atIndex:5];
                if (!o.kw.empty()) [enc setBytes:o.kw.data() length:o.kw.size() * sizeof(float) atIndex:6];
                else [enc setBuffer:frame_in offset:0 atIndex:6];
                dispatch2d(enc, pso, o.gw, o.gh);
            }
        }
        for (const KFGpuPass& p : passes) {
            KFGpuNoiseParams np = p.np;
            id<MTLBuffer> kwb = p.kw.empty() ? nil : [device newBufferWithBytes:p.kw.data() length:p.kw.size() * sizeof(float) options:MTLResourceStorageModeShared];
            if (p.kernel == 0) {
                [enc setComputePipelineState:md->noise_direct];
                [enc setBuffer:fields[p.layer] offset:0 atIndex:0];
                [enc setBytes:&np length:sizeof np atIndex:2];
                dispatch2d(enc, md->noise_direct, np.width, np.height);
            } else if (p.kernel == 1) {
                [enc setComputePipelineState:md->noise_h];
                [enc setBuffer:hb offset:0 atIndex:0];
                [enc setBuffer:kwb offset:0 atIndex:1];
                [enc setBytes:&np length:sizeof np atIndex:2];
                dispatch2d(enc, md->noise_h, np.width, (NSUInteger)p.hb_rows);
            } else {
                [enc setComputePipelineState:md->noise_v];
                [enc setBuffer:fields[p.layer] offset:0 atIndex:0];
                [enc setBuffer:hb offset:0 atIndex:1];
                [enc setBytes:&np length:sizeof np atIndex:2];
                [enc setBuffer:kwb offset:0 atIndex:3];
                dispatch2d(enc, md->noise_v, np.width, np.height);
            }
        }
        [enc setComputePipelineState:md->frame_pipeline];
        [enc setBuffer:frame_in offset:0 atIndex:0];
        [enc setBuffer:frame_out offset:0 atIndex:1];
        [enc setBytes:&params length:sizeof params atIndex:2];
        for (int k = 0; k < 4; ++k) [enc setBuffer:fields[k] offset:0 atIndex:3 + k];
        [enc setBuffer:(plan ? work[plan->cur] : frame_in) offset:0 atIndex:7];
        dispatch2d(enc, md->frame_pipeline, W, H);
        if (half_pixels) {
            KFCopyParams cp = {(uint32_t)W, (uint32_t)H, (uint32_t)W, real_dst_pitch};
            [enc setComputePipelineState:md->f2h];
            [enc setBuffer:tmp_out offset:0 atIndex:0]; [enc setBuffer:dst offset:0 atIndex:1]; [enc setBytes:&cp length:sizeof cp atIndex:2];
            dispatch2d(enc, md->f2h, W, H);
        }
        [enc endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        if (cb.error) { NSLog(@"KaptainsFilmLook GPU: frame failed: %@", cb.error.localizedDescription); rc = 2; }
    }
    return rc;
}

PF_Err kf_gpu_render_frame(PF_InData* in_data, PF_OutData*, PF_SmartRenderExtra* extra,
                           PF_EffectWorld* in_w, PF_EffectWorld* out_w, KFGpuFrameParams params,
                           const std::vector<KFGpuPass>& passes, const KFPlan* plan)
{
    if (!extra || !extra->input || !in_w || !out_w) return PF_Err_BAD_CALLBACK_PARAM;
    if (extra->input->what_gpu != PF_GPU_Framework_METAL) return PF_Err_UNRECOGNIZED_PARAM_TYPE;
    void* data = const_cast<void*>(extra->input->gpu_data);
    if (!data) return PF_Err_INTERNAL_STRUCT_DAMAGED;
    PF_GPUDeviceSuite1* gs = nullptr;
    if (in_data->pica_basicP->AcquireSuite(kPFGPUDeviceSuite, kPFGPUDeviceSuiteVersion1, (const void**)&gs) != 0 || !gs)
        return PF_Err_UNRECOGNIZED_PARAM_TYPE;
    PF_GPUDeviceInfo info; memset(&info, 0, sizeof info);
    void* src_mem = nullptr; void* dst_mem = nullptr;
    PF_Err err = gs->GetDeviceInfo(in_data->effect_ref, extra->input->device_index, &info);
    if (!err) err = gs->GetGPUWorldData(in_data->effect_ref, in_w, &src_mem);
    if (!err) err = gs->GetGPUWorldData(in_data->effect_ref, out_w, &dst_mem);
    in_data->pica_basicP->ReleaseSuite(kPFGPUDeviceSuite, kPFGPUDeviceSuiteVersion1);
    if (err) return err;
    params.width = (unsigned)out_w->width; params.height = (unsigned)out_w->height;
    params.srcPitch = (unsigned)(in_w->rowbytes / 16); params.dstPitch = (unsigned)(out_w->rowbytes / 16);
    const int rc = kf_gpu_exec(data, info.devicePV, info.command_queuePV, src_mem, dst_mem, params, passes, plan, 0);
    return rc == 0 ? PF_Err_NONE : (rc == 1 ? PF_Err_OUT_OF_MEMORY : PF_Err_INTERNAL_STRUCT_DAMAGED);
}
