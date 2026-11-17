#!/bin/sh
# Kaptains Film Look - Copyright (C) 2026 Henrik Jonas Kaptain
# SPDX-License-Identifier: GPL-3.0-or-later (with an additional permission for the Adobe SDKs, see LICENSE-EXCEPTION)
# Install the built plug-in where After Effects AND Premiere Pro look for shared plug-ins.
# Needs your Mac password (sudo). Copies over any older version without deleting anything.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="/tmp/kaptainsfilmlook_build/KaptainsFilmLook.plugin"
DEST="/Library/Application Support/Adobe/Common/Plug-ins/7.0/MediaCore"
[ -d "$SRC" ] || { echo "Build first: sh build_ae.sh"; exit 1; }
sudo mkdir -p "$DEST"
# An older copy (with a bad signature / Finder xattr) must not be merged into: move it aside, never delete.
if [ -e "$DEST/KaptainsFilmLook.plugin" ]; then
  mkdir -p "$HOME/Documents/_to_delete"
  sudo mv "$DEST/KaptainsFilmLook.plugin" "$HOME/Documents/_to_delete/KaptainsFilmLook.plugin.$(date +%Y%m%d-%H%M%S)"
fi
# The plug-in used to be called KaptainFilm.plugin: move that old copy aside too, or Premiere/AE would load both.
if [ -e "$DEST/KaptainFilm.plugin" ]; then
  mkdir -p "$HOME/Documents/_to_delete"
  sudo mv "$DEST/KaptainFilm.plugin" "$HOME/Documents/_to_delete/KaptainFilm.plugin.oldname.$(date +%Y%m%d-%H%M%S)"
fi
sudo ditto --noextattr --noqtn --norsrc "$SRC" "$DEST/KaptainsFilmLook.plugin"
sudo xattr -cr "$DEST/KaptainsFilmLook.plugin" 2>/dev/null || true
codesign --verify --deep --strict "$DEST/KaptainsFilmLook.plugin" && echo "Signature OK" || echo "WARNING: installed signature is not valid"
echo "Installed to: $DEST/KaptainsFilmLook.plugin"
echo "Now quit and restart After Effects. The effect is under Effect > Kaptain > Kaptains Film Look."
