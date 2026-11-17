// Kaptains Film Look - Copyright (C) 2026 Henrik Jonas Kaptain
// SPDX-License-Identifier: GPL-3.0-or-later (with an additional permission for the Adobe SDKs, see LICENSE-EXCEPTION)
// Kaptains Film Look - GPU glue (After Effects, Metal). The Metal side lives in KaptainFilmGPU.mm (macOS only).
// Step 1 ("skeleton"): the picture goes GPU -> Metal copy kernel -> CPU core -> Metal copy kernel -> GPU.
// Later steps replace the CPU stage effect by effect with Metal kernels.
#pragma once
#include "AEConfig.h"
#include "AE_Effect.h"
#include "AE_EffectCB.h"
#include "AE_EffectPixelFormat.h"
#include "AE_EffectCBSuites.h"
#include "AE_EffectGPUSuites.h"

// CPU stage: src / dst are tightly or loosely packed float4 pixels in B, G, R, A order (premultiplied), pitch in pixels.
typedef void (*KFGpuCpuStage)(const float* src, int sw, int sh, int src_pitch_px,
                              float* dst, int dw, int dh, int dst_pitch_px, void* user);

PF_Err kf_gpu_device_setup(PF_InData* in_data, PF_OutData* out_data, PF_GPUDeviceSetupExtra* extra);
PF_Err kf_gpu_device_setdown(PF_InData* in_data, PF_OutData* out_data, PF_GPUDeviceSetdownExtra* extra);
PF_Err kf_gpu_render(PF_InData* in_data, PF_OutData* out_data, PF_SmartRenderExtra* extra,
                     PF_EffectWorld* in_w, PF_EffectWorld* out_w, KFGpuCpuStage stage, void* user);

#include "kf_gpu_types.h"

// Which kernel groups compiled on this machine (KF_GRP_*). A missing group simply sends the frame down the older path.
unsigned kf_gpu_groups(PF_SmartRenderExtra* extra);

// `passes` build the noise fields (empty when Grain is off). Field buffer 0 = shared noise, 1..3 = independent R, G, B noise.
PF_Err kf_gpu_render_frame(PF_InData* in_data, PF_OutData* out_data, PF_SmartRenderExtra* extra,
                           PF_EffectWorld* in_w, PF_EffectWorld* out_w, KFGpuFrameParams params,
                           const std::vector<KFGpuPass>& passes, const KFPlan* plan = nullptr);
