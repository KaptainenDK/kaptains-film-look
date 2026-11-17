#!/bin/bash
# Kaptains Film Look - Copyright (C) 2026 Henrik Jonas Kaptain
# SPDX-License-Identifier: GPL-3.0-or-later (with an additional permission for the Adobe SDKs, see LICENSE-EXCEPTION)
# Kaptains Film Look installer (After Effects + Premiere Pro, macOS, Apple silicon).
# Double-click this file. It copies KaptainsFilmLook.plugin (next to this file) to the shared Adobe plug-in folder.
# Needs your Mac password once (the folder is system-wide). An older Kaptains Film Look install is simply replaced by this one.
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/KaptainsFilmLook.plugin"
DEST="${KF_DEST:-/Library/Application Support/Adobe/Common/Plug-ins/7.0/MediaCore}"
SUDO="${KF_SUDO-sudo}"

echo "Kaptains Film Look installer"
echo
if [ "$(uname -m)" != "arm64" ] && [ -z "$KF_ALLOW_ANY_ARCH" ]; then
  echo "This version only runs on Apple silicon Macs (M1 or newer). This Mac is: $(uname -m)."; read -r -p "Press Return to close."; exit 1
fi
if [ ! -d "$SRC" ]; then
  echo "KaptainsFilmLook.plugin was not found next to this installer ($HERE)."
  echo "Unzip the whole download first, then open this file from the unzipped folder."; read -r -p "Press Return to close."; exit 1
fi

set -e
# Files from the internet carry a 'quarantine' mark that makes macOS block the plug-in: remove it from the copy we install.
$SUDO mkdir -p "$DEST"
# Update = replace: remove the old Kaptains Film Look copy first so no stale files from an older version are left behind.
if [ -e "$DEST/KaptainsFilmLook.plugin" ]; then
  $SUDO rm -rf "$DEST/KaptainsFilmLook.plugin"
  echo "The older version was replaced."
fi
# Earlier versions were called KaptainFilm.plugin: remove that old copy too, or Premiere/AE would load both.
if [ -e "$DEST/KaptainFilm.plugin" ]; then
  $SUDO rm -rf "$DEST/KaptainFilm.plugin"
  echo "An earlier install under the old name was removed."
fi
$SUDO ditto --noextattr --noqtn --norsrc "$SRC" "$DEST/KaptainsFilmLook.plugin"
$SUDO xattr -cr "$DEST/KaptainsFilmLook.plugin" 2>/dev/null || true

if codesign --verify --deep --strict "$DEST/KaptainsFilmLook.plugin" 2>/dev/null; then echo "Signature check: OK"; else echo "WARNING: the signature check failed - the plug-in may not load. Re-download the zip."; fi
echo
echo "Installed to: $DEST/KaptainsFilmLook.plugin"
echo "Now quit and restart After Effects / Premiere Pro."
echo "After Effects: Effect > Kaptain > Kaptains Film Look.   Premiere Pro: Effects panel > Video Effects > Kaptain."
echo
read -r -p "Press Return to close."
