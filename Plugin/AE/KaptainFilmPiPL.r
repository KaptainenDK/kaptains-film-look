/* Kaptains Film Look - Copyright (C) 2026 Henrik Jonas Kaptain */
/* SPDX-License-Identifier: GPL-3.0-or-later (with an additional permission for the Adobe SDKs, see LICENSE-EXCEPTION) */
#include "AEConfig.h"
#include "AE_EffectVers.h"

#ifndef AE_OS_WIN
	#include <AE_General.r>
#endif

resource 'PiPL' (16000) {
	{
		Kind { AEEffect },
		Name { "Kaptains Film Look" },
		Category { "Kaptain" },
#ifdef AE_OS_WIN
    #if defined(AE_PROC_INTELx64)
		CodeWin64X86 {"EffectMain"},
    #elif defined(AE_PROC_ARM64)
		CodeWinARM64 {"EffectMain"},
    #endif
#elif defined(AE_OS_MAC)
		CodeMacIntel64 {"EffectMain"},
		CodeMacARM64 {"EffectMain"},
#endif
		AE_PiPL_Version { 2, 0 },
		AE_Effect_Spec_Version { PF_PLUG_IN_VERSION, PF_PLUG_IN_SUBVERS },
		AE_Effect_Version { 32786 /* 0.1.0 develop build 18 */ },
		AE_Effect_Info_Flags { 0 },
		/* PF_OutFlag_DEEP_COLOR_AWARE | PF_OutFlag_NON_PARAM_VARY */
		AE_Effect_Global_OutFlags { 0x02008004 },
		/* PF_OutFlag2_FLOAT_COLOR_AWARE | PF_OutFlag2_SUPPORTS_SMART_RENDER | PF_OutFlag2_SUPPORTS_THREADED_RENDERING */
		AE_Effect_Global_OutFlags_2 { 0x0A001400 },
		AE_Effect_Match_Name { "KAPTAIN KaptainsFilmLook" },
		AE_Reserved_Info { 0 },
		AE_Effect_Support_URL { "https://kaptainen.dk" }
	}
};
