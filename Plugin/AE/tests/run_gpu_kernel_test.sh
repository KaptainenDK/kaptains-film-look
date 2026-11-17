#!/bin/sh
# Kaptains Film Look - Copyright (C) 2026 Henrik Jonas Kaptain
# SPDX-License-Identifier: GPL-3.0-or-later (with an additional permission for the Adobe SDKs, see LICENSE-EXCEPTION)
# Builds + runs the kernel-vs-CPU-core test. Needs the AE SDK headers (set SDK) and, on Linux, the small Mac header shim (set SHIM).
# usage: SDK=/path/to/AfterEffectsSDK/Examples [SHIM="-include /tmp/fake/macfake.h -I/tmp/fake"] sh run_gpu_kernel_test.sh
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"; PL="$HERE/.."
OUT="${TMPDIR:-/tmp}/kf_gpu_test"; mkdir -p "$OUT"
python3 - "$PL/kf_gpu_msl.h" "$PL/kf_gpu_msl_heavy.h" "$OUT/kernel_cpp.inc" <<'PY'
import sys,re
body=""
for f in sys.argv[1:3]:
    src=open(f).read()
    for m in re.finditer(r'R"MSL\((.*?)\)MSL"', src, re.S): body+=m.group(1)+"\n"
open(sys.argv[3],'w').write("#define kernel\n#define device\n#define constant const\n#define thread\n"+body+"\n#undef kernel\n#undef device\n#undef constant\n#undef thread\n")
PY
cp "$HERE/gpu_kernel_test.cpp" "$OUT/gpu_kernel_test.cpp"
g++ -std=c++17 -O1 -pthread -fpermissive -w -D__MACH__ -DMAC_ENV $SHIM -I"$OUT" -I"$PL" -I"$SDK/Headers" -I"$SDK/Util" -I"$SDK/Headers/SP" -I"$SDK/Resources" \
  -DKF_CORE_PATH_OK "$OUT/gpu_kernel_test.cpp" -o "$OUT/gpu_kernel_test"
"$OUT/gpu_kernel_test"
