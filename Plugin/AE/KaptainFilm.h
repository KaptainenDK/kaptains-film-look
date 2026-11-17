// Kaptains Film Look - Copyright (C) 2026 Henrik Jonas Kaptain
// SPDX-License-Identifier: GPL-3.0-or-later (with an additional permission for the Adobe SDKs, see LICENSE-EXCEPTION)
// Kaptains Film Look - After Effects effect plug-in (milestone A: Vignette + Grain)
#pragma once
#ifndef KAPTAIN_FILM_H
#define KAPTAIN_FILM_H

typedef unsigned char u_char;
typedef unsigned short u_short;
typedef unsigned short u_int16;
typedef unsigned long u_long;
typedef short int int16;
#define PF_TABLE_BITS 12
#define PF_TABLE_SZ_16 4096

#define PF_DEEP_COLOR_AWARE 1

#include "AEConfig.h"

#ifdef AE_OS_WIN
typedef unsigned short PixelType;
#include <Windows.h>
#endif

#include "entry.h"
#include "AE_Effect.h"
#include "AE_EffectCB.h"
#include "AE_Macros.h"
#include "Param_Utils.h"
#include "AE_EffectCBSuites.h"
#include "String_Utils.h"
#include "AE_GeneralPlug.h"
#include "AEFX_ChannelDepthTpl.h"
#include "AEGP_SuiteHandler.h"

#define KF_MAJOR_VERSION 0
#define KF_MINOR_VERSION 1
#define KF_BUG_VERSION 0
#define KF_STAGE_VERSION PF_Stage_DEVELOP
#define KF_BUILD_VERSION 18

// Parameter list (UI order). Every group start/end counts as a parameter slot.
enum
{
    KF_INPUT = 0,

    KF_ZONES_START,
    KF_ZONE_CENTER,
    KF_ZONE_ROUNDNESS,
    KF_INPUT_SPACE,
    KF_MIX,
    KF_SHOW_ZONES,
    KF_ZONES_END,

    KF_LENS_START,
    KF_LENS_ON,
    KF_LENS_AMOUNT,
    KF_LENS_AUTOZOOM,
    KF_LENS_ZOOM,
    KF_LENS_EDGE,
    KF_LENS_ZONE_START,
    KF_LENS_ZONE_FULL,
    KF_LENS_END,

    KF_CA_START,
    KF_CA_ON,
    KF_CA_AMOUNT,
    KF_CA_BLUR,
    KF_CA_ZONE_START,
    KF_CA_ZONE_FULL,
    KF_CA_END,

    KF_LB_START,
    KF_LB_ON,
    KF_LB_TYPE,
    KF_LB_AMOUNT,
    KF_LB_ANGLE,
    KF_LB_ZONE_START,
    KF_LB_ZONE_FULL,
    KF_LB_END,

    KF_VIG_START,
    KF_VIG_ON,
    KF_VIG_AMOUNT,
    KF_VIG_SOFTNESS,
    KF_VIG_ZONE_START,
    KF_VIG_ZONE_FULL,
    KF_VIG_END,

    KF_BLOOM_START,
    KF_BLOOM_ON,
    KF_BLOOM_THRESHOLD,
    KF_BLOOM_INTENSITY,
    KF_BLOOM_RADIUS,
    KF_BLOOM_BLUR,
    KF_BLOOM_STRETCH,
    KF_BLOOM_STRETCH_ANGLE,
    KF_BLOOM_TINT,
    KF_BLOOM_ZONE_START,
    KF_BLOOM_ZONE_FULL,
    KF_BLOOM_END,

    KF_HAL_START,
    KF_HAL_ON,
    KF_HAL_THRESHOLD,
    KF_HAL_INTENSITY,
    KF_HAL_RADIUS,
    KF_HAL_BLUR,
    KF_HAL_STRETCH,
    KF_HAL_STRETCH_ANGLE,
    KF_HAL_TINT,
    KF_HAL_ZONE_START,
    KF_HAL_ZONE_FULL,
    KF_HAL_END,

    KF_GRAIN_START,
    KF_GRAIN_ON,
    KF_GRAIN_AMOUNT,
    KF_GRAIN_SIZE,
    KF_GRAIN_ROUGHNESS,
    KF_GRAIN_COLOR,
    KF_GRAIN_MIDTONE,
    KF_GRAIN_SEED,
    KF_GRAIN_ANIMATE,
    KF_GRAIN_ZONE_START,
    KF_GRAIN_ZONE_FULL,
    KF_GRAIN_END,

    KF_NUM_PARAMS
};

// Stable disk IDs (never renumber once released). Ids 1-6, 20-25 and 40-50 are from milestone A.
enum
{
    ID_ZONES_START = 1, ID_ZONE_CENTER, ID_ZONE_ROUNDNESS, ID_INPUT_SPACE, ID_SHOW_ZONES, ID_ZONES_END, ID_MIX,
    ID_VIG_START = 20, ID_VIG_AMOUNT, ID_VIG_SOFTNESS, ID_VIG_ZONE_START, ID_VIG_ZONE_FULL, ID_VIG_END, ID_VIG_ON,
    ID_GRAIN_START = 40, ID_GRAIN_AMOUNT, ID_GRAIN_SIZE, ID_GRAIN_ROUGHNESS, ID_GRAIN_COLOR, ID_GRAIN_MIDTONE,
    ID_GRAIN_SEED, ID_GRAIN_ANIMATE, ID_GRAIN_ZONE_START, ID_GRAIN_ZONE_FULL, ID_GRAIN_END, ID_GRAIN_ON,
    ID_LENS_START = 100, ID_LENS_ON, ID_LENS_AMOUNT, ID_LENS_AUTOZOOM, ID_LENS_ZOOM, ID_LENS_EDGE, ID_LENS_ZONE_START, ID_LENS_ZONE_FULL, ID_LENS_END,
    ID_CA_START = 120, ID_CA_ON, ID_CA_AMOUNT, ID_CA_BLUR, ID_CA_ZONE_START, ID_CA_ZONE_FULL, ID_CA_END,
    ID_LB_START = 140, ID_LB_ON, ID_LB_TYPE, ID_LB_AMOUNT, ID_LB_ANGLE, ID_LB_ZONE_START, ID_LB_ZONE_FULL, ID_LB_END,
    ID_BLOOM_START = 160, ID_BLOOM_ON, ID_BLOOM_THRESHOLD, ID_BLOOM_INTENSITY, ID_BLOOM_RADIUS, ID_BLOOM_BLUR, ID_BLOOM_STRETCH,
    ID_BLOOM_STRETCH_ANGLE, ID_BLOOM_TINT, ID_BLOOM_ZONE_START, ID_BLOOM_ZONE_FULL, ID_BLOOM_END,
    ID_HAL_START = 180, ID_HAL_ON, ID_HAL_THRESHOLD, ID_HAL_INTENSITY, ID_HAL_RADIUS, ID_HAL_BLUR, ID_HAL_STRETCH,
    ID_HAL_STRETCH_ANGLE, ID_HAL_TINT, ID_HAL_ZONE_START, ID_HAL_ZONE_FULL, ID_HAL_END
};

extern "C"
{
    DllExport PF_Err EffectMain(
        PF_Cmd cmd, PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], PF_LayerDef* output, void* extra);
}

#endif
