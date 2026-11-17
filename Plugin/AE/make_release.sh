#!/bin/sh
# Kaptains Film Look - Copyright (C) 2026 Henrik Jonas Kaptain
# SPDX-License-Identifier: GPL-3.0-or-later (with an additional permission for the Adobe SDKs, see LICENSE-EXCEPTION)
# Build a zip you can share: signed KaptainsFilmLook.plugin + double-click installer + read-me + license files.
# Usage (from this folder):  sh make_release.sh      -> ../../Release/KaptainsFilmLook-0.1.0-build<N>-mac-arm64.zip
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="$(sed -n 's/^#define KF_BUILD_VERSION[[:space:]]*\([0-9]*\).*/\1/p' "$HERE/KaptainFilm.h")"
NAME="KaptainsFilmLook-0.1.0-build$BUILD-mac-arm64"

echo "== building the plug-in first"
sh "$HERE/build_ae.sh"

OUTDIR="$ROOT/Release"; mkdir -p "$OUTDIR"
STAGE="/tmp/kaptainsfilmlook_release/$NAME.$(date +%H%M%S)"        # fresh folder in /tmp, nothing is overwritten or deleted
mkdir -p "$STAGE/$NAME"
ditto --noextattr --noqtn --norsrc /tmp/kaptainsfilmlook_build/KaptainsFilmLook.plugin "$STAGE/$NAME/KaptainsFilmLook.plugin"
cp "$HERE/release_template/Install Kaptains Film Look.command" "$STAGE/$NAME/"
cp "$HERE/release_template/READ ME FIRST.txt" "$STAGE/$NAME/"
cp "$HERE/release_template/LICENSE" "$HERE/release_template/LICENSE-EXCEPTION" "$STAGE/$NAME/"
chmod +x "$STAGE/$NAME/Install Kaptains Film Look.command"
xattr -cr "$STAGE" 2>/dev/null || true
codesign --verify --deep --strict "$STAGE/$NAME/KaptainsFilmLook.plugin"

ZIP="$OUTDIR/$NAME.zip"
if [ -e "$ZIP" ]; then mkdir -p "$HOME/Documents/_to_delete"; mv "$ZIP" "$HOME/Documents/_to_delete/$NAME.$(date +%Y%m%d-%H%M%S).zip"; fi
(cd "$STAGE" && ditto -c -k --norsrc --noextattr --keepParent "$NAME" "$ZIP")

echo "== checking the zip: unpack it somewhere else and verify"
CHK="/tmp/kaptainsfilmlook_release/check.$(date +%H%M%S)"; mkdir -p "$CHK"
ditto -x -k "$ZIP" "$CHK"
codesign --verify --deep --strict "$CHK/$NAME/KaptainsFilmLook.plugin" && echo "signature after unzip: OK"
[ -x "$CHK/$NAME/Install Kaptains Film Look.command" ] && echo "installer is executable: OK" || echo "WARNING: installer lost its executable bit"
echo "== done"; ls -l "$ZIP"; shasum -a 256 "$ZIP"
