#!/bin/sh
# Kaptains Film Look - Copyright (C) 2026 Henrik Jonas Kaptain
# SPDX-License-Identifier: GPL-3.0-or-later (with an additional permission for the Adobe SDKs, see LICENSE-EXCEPTION)
# Build the Kaptains Film Look After Effects plug-in (macOS, Apple silicon).
# Usage:  sh build_ae.sh      (from this folder)
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
SDK="$ROOT/Adobe SDK/unpacked/AfterEffectsSDK/Examples"
if [ ! -d "$SDK/Headers" ]; then echo "After Effects SDK not found at: $SDK"; exit 1; fi
if [ ! -f "$ROOT/Kode/core/kf_core.cpp" ]; then echo "Core not found at: $ROOT/Kode/core"; exit 1; fi

# Build outside Documents: iCloud keeps adding a Finder xattr to folders there, which makes the signature invalid.
OUT="/tmp/kaptainsfilmlook_build/KaptainsFilmLook.plugin"
mkdir -p /tmp/kaptainsfilmlook_build
mkdir -p "$OUT/Contents/MacOS" "$OUT/Contents/Resources"
cp "$HERE/Info.plist" "$OUT/Contents/Info.plist"
printf 'eFKTFXTC' > "$OUT/Contents/PkgInfo"

PRSDK="$ROOT/Adobe SDK/unpacked/Premiere Pro 27.0 C++ SDK/Examples"
if [ ! -f "$PRSDK/Headers/PrSDKGPUFilter.h" ] || [ ! -f "$PRSDK/Projects/GPUVideoFilter/Utils/PrGPUFilterModule.h" ]; then
  echo "Premiere Pro SDK not found at: $PRSDK"; exit 1
fi
OBJ="/tmp/kaptainsfilmlook_build/obj"
rm -rf "$OBJ"; mkdir -p "$OBJ"
COMMON="-std=c++17 -O2 -arch arm64 -mmacosx-version-min=12.0 -fvisibility=hidden -DNDEBUG -Wno-unknown-pragmas"

echo "== 1/3 compiling (a minute or so)"
# After Effects part and the Metal side: After Effects headers only
clang++ $COMMON -c -I"$SDK/Headers" -I"$SDK/Util" -I"$SDK/Headers/SP" -I"$SDK/Resources" \
  "$HERE/KaptainFilm.cpp" -o "$OBJ/KaptainFilm.o"
clang++ $COMMON -c -fobjc-arc -I"$SDK/Headers" -I"$SDK/Util" -I"$SDK/Headers/SP" -I"$SDK/Resources" \
  "$HERE/KaptainFilmGPU.mm" -o "$OBJ/KaptainFilmGPU.o"
clang++ $COMMON -c -I"$SDK/Headers" -I"$SDK/Util" -I"$SDK/Headers/SP" -I"$SDK/Resources" \
  "$SDK/Util/AEGP_SuiteHandler.cpp" -o "$OBJ/AEGP_SuiteHandler.o"
clang++ $COMMON -c -I"$SDK/Headers" -I"$SDK/Util" -I"$SDK/Headers/SP" -I"$SDK/Resources" \
  "$SDK/Util/MissingSuiteError.cpp" -o "$OBJ/MissingSuiteError.o"
# Premiere GPU filter (exports xGPUFilterEntry): Premiere headers only - the two header sets must never meet in one file
clang++ $COMMON -c -DPRMAC_ENV -I"$PRSDK/Headers" -I"$PRSDK/Projects/GPUVideoFilter/Utils" -I"$HERE" \
  "$HERE/KaptainFilmPremiere.cpp" -o "$OBJ/KaptainFilmPremiere.o"
clang++ -arch arm64 -mmacosx-version-min=12.0 -bundle "$OBJ"/*.o \
  -framework CoreFoundation -framework Cocoa -framework Metal -framework Foundation \
  -o "$OUT/Contents/MacOS/KaptainsFilmLook"
if nm -gU "$OUT/Contents/MacOS/KaptainsFilmLook" | grep -q xGPUFilterEntry; then echo "Premiere GPU entry point: present"; else echo "WARNING: xGPUFilterEntry is missing in the binary - tell Claude."; fi

echo "== 2/3 compiling the PiPL resource (Rez)"
REZ="$(xcrun --find Rez 2>/dev/null || true)"
if [ -z "$REZ" ]; then
  for c in /Applications/Xcode.app/Contents/Developer/usr/bin/Rez /usr/bin/Rez /Library/Developer/CommandLineTools/usr/bin/Rez; do
    [ -x "$c" ] && REZ="$c" && break
  done
fi
if [ -z "$REZ" ]; then
  echo "STOP: the Rez tool was not found on this Mac. Tell Claude - the PiPL needs another route."
  exit 2
fi
echo "using $REZ"
"$REZ" "$HERE/KaptainFilmPiPL.r" -o "$OUT/Contents/Resources/KaptainsFilmLook.rsrc" -useDF -d __MACH__ \
  -i "$SDK/Headers" -i "$SDK/Resources" -i "$SDK/Util" -arch arm64

echo "== 3/3 signing (ad hoc)"
# Strip extended attributes / resource forks ("detritus") that make codesign refuse to seal the bundle.
xattr -cr "$OUT" 2>/dev/null || true
find "$OUT" -name '._*' -delete 2>/dev/null || true
dot_clean -m "$OUT" 2>/dev/null || true
codesign --force --deep --sign - "$OUT"
# The signature must be valid and must seal Contents/Resources + Info.plist, or Premiere Pro may refuse the plug-in.
codesign --verify --deep --strict --verbose=2 "$OUT" || { echo "STOP: the plug-in signature is NOT valid. Tell Claude."; exit 3; }
codesign -dvv "$OUT" 2>&1 | grep -E "Sealed Resources|Info.plist" || true
# keep a copy next to the sources for reference (the installer uses the /tmp one, which is the validly signed original)
mkdir -p "$HERE/build" && ditto --noextattr --noqtn --norsrc "$OUT" "$HERE/build/KaptainsFilmLook.plugin" 2>/dev/null || true
echo "OK: $OUT"
