// Kaptains Film Look - Copyright (C) 2026 Henrik Jonas Kaptain
// SPDX-License-Identifier: GPL-3.0-or-later (with an additional permission for the Adobe SDKs, see LICENSE-EXCEPTION)
// Kaptains Film Look - Premiere Pro GPU filter (macOS / Metal).
// Premiere looks for the exported function xGPUFilterEntry in the same binary as the effect. This file only talks to Premiere:
// it reads the frame buffer and the effect parameters and hands them to the shared Metal executor (KaptainFilmGPU.mm) through
// kf_prem_prepare (KaptainFilm.cpp, which knows the parameter list). It must NOT include any After Effects header (they clash).
// Any error returned from here makes Premiere render the frame with the normal CPU effect instead.
// PrGPUFilterModule.h uses the transition suite only as an unused pointer, but does not declare it. Declaring it here (instead of including
// PrSDKAESupport.h) keeps every After Effects header out of this file.
struct PF_TransitionSuite;
#define kPFTransitionSuite "PF Transition Suite"
#define kPFTransitionSuiteVersion 2
#include "PrGPUFilterModule.h"
#include "kf_gpu_types.h"
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include <cstdarg>
#include <ctime>
#include <unistd.h>
#include <cstdlib>
#ifndef __APPLE__
#define getprogname() "linux-syntax-check"
#endif

// Every message goes to the macOS log AND to a plain text file (/tmp/kaptainsfilmlook_log.txt), so a missing log line can be told apart from a missing call.
static void kf_flog(const char* fmt, ...)
{
    FILE* f = std::fopen("/tmp/kaptainsfilmlook_log.txt", "a");
    if (!f) return;
    char buf[512]; va_list ap; va_start(ap, fmt); std::vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    std::fprintf(f, "%ld [%s %d] %s\n", (long)std::time(nullptr), getprogname(), (int)getpid(), buf);
    std::fclose(f);
}
#if defined(__APPLE__)
#include <os/log.h>
#define KF_PLOG(...) do { os_log(OS_LOG_DEFAULT, __VA_ARGS__); kf_flog(__VA_ARGS__); } while (0)
#define KF_PLOG_TEXT(t) do { os_log(OS_LOG_DEFAULT, "%{public}s", t); kf_flog("%s", t); } while (0)
#else
#define KF_PLOG(...) ((void)0)
#define KF_PLOG_TEXT(t) ((void)0)
#endif

// Runs the moment the binary is loaded into a process (Premiere, After Effects, or anything that scans plug-ins).
__attribute__((constructor)) static void kf_loaded() { KF_PLOG("KaptainsFilmLook Premiere: plug-in binary loaded (build 18)"); }

namespace {

constexpr int kMaxDevices = 12;
struct DeviceSlot { void* data = nullptr; bool tried = false; };
DeviceSlot g_slots[kMaxDevices];
std::mutex g_slot_mutex;

// One compiled set of Metal pipelines per Premiere GPU device, created on first use and freed in Shutdown.
void* data_for(unsigned device_index, void* mtl_device)
{
    if (device_index >= (unsigned)kMaxDevices) return nullptr;
    std::lock_guard<std::mutex> lock(g_slot_mutex);
    DeviceSlot& s = g_slots[device_index];
    if (!s.tried) {
        s.tried = true;
        s.data = kf_gpu_make_data(mtl_device);
        KF_PLOG("KaptainsFilmLook Premiere: Metal pipelines on device %u ready: %d", device_index, s.data ? 1 : 0);
    }
    return s.data;
}

// Reads one Premiere parameter into the plain value struct the bridge understands.
KFPremValue to_value(const PrParam& p)
{
    KFPremValue v; std::memset(&v, 0, sizeof v);
    switch (p.mType) {
        case kPrParamType_Int8:    v.type = 3; v.num = p.mInt8;  v.raw = (unsigned long long)(long long)p.mInt8;  break;
        case kPrParamType_Int16:   v.type = 3; v.num = p.mInt16; v.raw = (unsigned long long)(long long)p.mInt16; break;
        case kPrParamType_Int32:   v.type = 3; v.num = p.mInt32; v.raw = (unsigned long long)(unsigned int)p.mInt32; break;   // colours: 0xAARRGGBB
        case kPrParamType_Int64:   v.type = 3; v.num = (double)p.mInt64; v.raw = (unsigned long long)p.mInt64; break;
        case kPrParamType_Float32: v.type = 1; v.num = p.mFloat32; break;
        case kPrParamType_Float64: v.type = 1; v.num = p.mFloat64; break;
        case kPrParamType_Bool:    v.type = 1; v.num = p.mBool ? 1.0 : 0.0; break;
        case kPrParamType_Point:   v.type = 2; v.x = p.mPoint.x; v.y = p.mPoint.y; break;
        default: v.type = 0; break;                                    // Guid / memory pointer: not used by this effect
    }
    return v;
}

} // namespace

class KaptainFilmGPUFilter : public PrGPUFilterBase
{
public:
    static prSuiteError Startup(piSuitesPtr, csSDK_int32 i) { KF_PLOG("KaptainsFilmLook Premiere: entry point called by Premiere (startup, index %d)", (int)i); return suiteError_NoError; }
    static prSuiteError Shutdown(piSuitesPtr, csSDK_int32)
    {
        std::lock_guard<std::mutex> lock(g_slot_mutex);
        for (DeviceSlot& s : g_slots) { if (s.data) kf_gpu_free_data(s.data); s = DeviceSlot(); }
        return suiteError_NoError;
    }
    // null name = the match name from this module's PiPL ("KAPTAIN KaptainsFilmLook")
    static PrSDKString MatchName(piSuitesPtr, csSDK_int32) { return PrSDKString(); }

    prSuiteError Initialize(PrGPUFilterInstance* inst) override
    {
        prSuiteError err = PrGPUFilterBase::Initialize(inst);
        KF_PLOG("KaptainsFilmLook Premiere: filter instance requested, device %u, framework %d, base init %d", mDeviceIndex, (int)mDeviceInfo.outDeviceFramework, (int)err);
        if (!PrSuiteErrorSucceeded(err)) return err;
        if (mDeviceInfo.outDeviceFramework != PrGPUDeviceFramework_Metal) return suiteError_Fail;       // only Metal is implemented
        if (!data_for(mDeviceIndex, mDeviceInfo.outDeviceHandle)) return suiteError_Fail;
        KF_PLOG("KaptainsFilmLook Premiere: filter instance created");
        return suiteError_NoError;
    }

    prSuiteError Render(const PrGPUFilterRenderParams* rp, const PPixHand* frames, csSDK_size_t frame_count, PPixHand* out_frame) override
    {
        (void)frames; (void)frame_count;
        KF_PLOG("KaptainsFilmLook Premiere: render called");
        if (!rp || !out_frame || !*out_frame) return fail(1);
        void* data = data_for(mDeviceIndex, mDeviceInfo.outDeviceHandle);
        if (!data) return fail(2);

        void* gpu_pixels = nullptr;
        if (mGPUDeviceSuite->GetGPUPPixData(*out_frame, &gpu_pixels) != suiteError_NoError || !gpu_pixels) return fail(3);
        PrPixelFormat fmt = PrPixelFormat_Invalid;
        mPPixSuite->GetPixelFormat(*out_frame, &fmt);
        int bpp = 0;
        if (fmt == PrPixelFormat_GPU_BGRA_4444_32f) bpp = 16;
        else if (fmt == PrPixelFormat_GPU_BGRA_4444_16f) bpp = 8;
        else return fail(4);
        prRect bounds; std::memset(&bounds, 0, sizeof bounds);
        mPPixSuite->GetBounds(*out_frame, &bounds);
        csSDK_int32 row_bytes = 0;
        mPPixSuite->GetRowBytes(*out_frame, &row_bytes);
        const int w = bounds.right - bounds.left, h = bounds.bottom - bounds.top;
        if (w <= 0 || h <= 0 || row_bytes <= 0 || row_bytes % bpp != 0) return fail(5);

        // all effect parameters at this clip time (the input frame is not counted: GetParam above already shifts by one)
        csSDK_int32 count = 0;
        if (mVideoSegmentSuite->GetParamCount(mNodeID, &count) != suiteError_NoError || count <= 0 || count > 256) return fail(6);
        std::vector<KFPremValue> vals((size_t)count);
        for (csSDK_int32 i = 0; i < count; ++i) {
            PrParam p = {};
            if (mVideoSegmentSuite->GetParam(mNodeID, i, rp->inClipTime, &p) != suiteError_NoError) return fail(7);
            vals[(size_t)i] = to_value(p);
        }
        log_params_once(count, vals);

        const int frame = rp->inRenderTicksPerFrame > 0 ? (int)(rp->inClipTime / rp->inRenderTicksPerFrame) : 0;
        KFGpuFrameParams fp; std::memset(&fp, 0, sizeof fp);
        std::vector<KFGpuPass> passes; KFPlan plan; bool use_plan = false;
        if (!kf_prem_prepare(vals.data(), (int)count, w, h, frame, kf_gpu_data_groups(data), fp, passes, plan, use_plan)) {
            return fail(8);
        }
        fp.srcPitch = fp.dstPitch = (unsigned)(row_bytes / bpp);
        const int rc = kf_gpu_exec(data, mDeviceInfo.outDeviceHandle, mDeviceInfo.outCommandQueueHandle,
                                   gpu_pixels, gpu_pixels, fp, passes, use_plan ? &plan : nullptr, bpp == 8 ? 1 : 0);   // in place
        if (rc != 0) return fail(9 + rc);
        log_once(&done_logged_, bpp == 8 ? "KaptainsFilmLook Premiere: first frame rendered on the GPU (16-bit float)"
                                         : "KaptainsFilmLook Premiere: first frame rendered on the GPU (32-bit float)");
        return suiteError_NoError;
    }

private:
    // first failure of each kind is logged with a number: 1 bad args, 2 no pipelines, 3 no GPU pointer, 4 pixel format, 5 row bytes, 6 param count,
    // 7 param read, 8 parameters not usable (see the param lines), 10/11 Metal out of memory / failed
    prSuiteError fail(int code) { if (code < 32 && !(failed_ & (1u << code))) { failed_ |= 1u << code; KF_PLOG("KaptainsFilmLook Premiere: frame handed back to the CPU effect, reason %d", code); } return suiteError_Fail; }
    unsigned failed_ = 0;
    // Numbers only (the macOS log hides strings): how many parameters, and type + value of every one, once per instance.
    void log_params_once(csSDK_int32 count, const std::vector<KFPremValue>& v)
    {
        if (params_logged_) return;
        params_logged_ = true;
        KF_PLOG("KaptainsFilmLook Premiere: parameter count %d", (int)count);
        for (csSDK_int32 i = 0; i < count; ++i)
            KF_PLOG("KaptainsFilmLook Premiere: param %d type %d value %f x %f y %f raw %llu", (int)i, v[(size_t)i].type, v[(size_t)i].num,
                    v[(size_t)i].x, v[(size_t)i].y, v[(size_t)i].raw);
    }
    static void log_once(bool* flag, const char* text) { if (!*flag) { *flag = true; KF_PLOG_TEXT(text); } }

    bool params_logged_ = false, declined_logged_ = false, done_logged_ = false;
};

DECLARE_GPUFILTER_ENTRY(PrGPUFilterModule<KaptainFilmGPUFilter>)
