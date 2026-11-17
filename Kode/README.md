# Kaptains Film Look - core

The host independent C++ core of Kaptains Film Look, with its test harness. No Adobe SDK code here.
Licensed under GPL-3.0-or-later, see LICENSE and LICENSE-EXCEPTION in the repository root.

Fixed order:
1 Lens Distortion -> 2 Chromatic Aberration (resampled together with 1) -> 3 Lens Blur
-> 4 Vignette -> 5 Bloom -> 6 Halation -> 7 Grain.

- Every effect has a zone: Start (full effect begins to fade in) and Full (full effect). Start = Full = 0 means "everywhere".
- One shared, movable zone centre (zone_center_x/y) and one shared zone shape (zone_roundness: 0 = oval, 1 = circle).
- Lens Blur types: Soft, Radial, Swirl, Directional (with angle).
- Chromatic Aberration, Bloom and Halation have their own Blur; Bloom and Halation also Stretch + angle. Grain has no blur.
- show_zones draws a dashed Start ring and a solid Full ring per effect (own colour each) plus a centre cross. In the plugin this is a preview aid and must not end up in renders.
- All sizes are derived from the image diagonal, so the look is the same at any resolution, aspect ratio and orientation. Parameters are documented in core/kf_core.hpp.

Build the test CLI:   sh core/build.sh
Run the checks:       python3 -I tests/run_tests.py      (needs numpy, Pillow)
Contact sheet:        KF_IMAGES=<folder> python3 tools/test_effects.py NAME   (needs rawpy)
Try a RAF still:      KF_IMAGES=<folder> python3 tools/test_raf.py NAME   (needs rawpy and scipy, presets in tools/presets.json)
