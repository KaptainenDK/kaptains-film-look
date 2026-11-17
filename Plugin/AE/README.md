# Kaptains Film Look - After Effects plug-in (milestone B: all effects)

All seven effects (Lens Distortion, Chromatic Aberration, Lens Blur, Vignette, Bloom, Halation, Grain), built on the host-independent core in `../../Kode/core`. Each effect has an On checkbox (Vignette and Grain on by default) and its own zone.
Parameters: Global & Zones (zone centre, roundness, input space, Mix, Show Zones) plus one group per effect. Changing the parameter list means: remove the effect from the layer and apply it again.

    sh build_ae.sh      # compile + PiPL + sign  -> build/KaptainsFilmLook.plugin
    sh install_ae.sh    # copy to /Library/Application Support/Adobe/Common/Plug-ins/7.0/MediaCore (sudo)

`tests/mock_host_test.cpp` tests the parameter/pixel glue (8/16/32-bit, premultiplied alpha, point parameter, grain per frame)
against a simulated host. It is not part of the plug-in.

## Build 8: Show Zones overlay in the comp window (After Effects only)

With the effect selected, the zone rings are drawn on top of the comp/layer window as an on-screen overlay: Start dashed, Full solid,
in the same colours as the burned-in rings, plus a white cross at the zone centre. Only effects that are On, have an effect amount
and a zone other than 0/0 get rings. The overlay is never part of the rendered picture, so it is safe for renders.
Premiere has no comp-window overlay, so there the **Show Zones** checkbox (burned in - turn it off before rendering) is still the way.
The parameter list is unchanged, so existing projects keep working. Needs `PF_OutFlag_CUSTOM_UI` (PiPL OutFlags 0x02008004).
The drawing itself could only be compiled and geometry-tested off-machine; check it visually in After Effects.

## Build 9: GPU skeleton (After Effects, Metal) - step 1

The effect now announces GPU support (`PF_OutFlag2_SUPPORTS_GPU_RENDER_F32`, PiPL OutFlags_2 0x0A001400) and handles
`PF_Cmd_GPU_DEVICE_SETUP / SETDOWN / SMART_RENDER_GPU`. When the project's Renderer is Metal, AE picks the GPU path in pre-render and
the frame goes: GPU buffer -> Metal copy kernel -> CPU core -> Metal copy kernel -> GPU buffer. The picture is the same as the CPU path
(tested pixel for pixel); this step only proves the plumbing. It is NOT faster yet. Later steps move the effects into Metal kernels one by one.
No GPU checkbox in the plug-in: with Renderer = Software Only, AE uses the CPU path. Premiere does not use this path.
Files: `KaptainFilmGPU.h` (interface), `KaptainFilmGPU.mm` (Metal, Objective-C++/ARC, the kernel is compiled from source at start-up),
`Tools/gpu_log.sh` shows the log lines the GPU path writes (device ready / first frame through Metal / compile errors).

## Build 10: GPU step 2a - Vignette fully on the GPU

When After Effects renders with Metal and the only active parts are Vignette (+ Input Space, Mix and the shared zone), one Metal kernel
(`KFVignetteKernel` in `kf_gpu_msl.h`) does the whole frame: decode -> vignette -> mix -> encode, no CPU stage. With any other effect on
(Lens, CA, Lens Blur, Bloom, Halation, Grain) or Show Zones burned in, the frame uses the step-1 path (GPU -> CPU core -> GPU, same picture).
The kernel text is tested for real: `tests/run_gpu_kernel_test.sh` compiles the shipped kernel as plain C++ and compares it with the CPU core
(difference ~1e-7). The host struct and the Metal struct come from one field list (`KF_VIG_FIELDS`), so they cannot drift apart.
Next: 2b Grain, 3 Lens Distortion + Chromatic Aberration, 4 Lens Blur / Bloom / Halation.

## Build 11: GPU step 2b - Grain on the GPU

Grain now runs in Metal too, so Vignette + Grain (+ Input Space, Mix, zones) is one GPU job; any other effect on (Lens, CA, Lens Blur, Bloom,
Halation) or burned-in Show Zones still uses the step-1 path (GPU -> CPU core -> GPU, same picture).
Noise is built exactly like the core: the same integer hash per pixel position (so seed / animate / frame give the same pattern), a gaussian blur
for Size (horizontal + vertical pass, padded so the statistics are exact at the borders), a coarse layer for Roughness and independent R/G/B layers for Color.
`kf_gpu_plan.h` turns the Grain settings into a list of kernel passes; the Metal host (`KaptainFilmGPU.mm`) and the test harness both run that same list.
`tests/run_gpu_kernel_test.sh` runs the shipped kernels as C++ against the CPU core (differences < 3e-7; mutation-checked: breaking a kernel makes it fail).
Next: 3 Lens Distortion + Chromatic Aberration, 4 Lens Blur / Bloom / Halation.

## Build 12: GPU step 3 - everything on the GPU (Lens, Chromatic Aberration, Lens Blur, Bloom, Halation)

All effects now run in Metal. Only the burned-in **Show Zones** rings (a preview aid, off for renders) and cropped/offset output areas still use
the older path (GPU -> CPU core -> GPU, same picture).
How it works: when Lens / CA / Lens Blur / Bloom / Halation actually change the picture (checkbox on and a non-zero value),
`kf_build_heavy_plan` (in `kf_gpu_plan.h`) turns the settings into a list of small kernel launches that mirrors `kf::process()` stage by stage:
decode -> lens + CA (bicubic, 3 edge modes, per-channel shift, auto-zoom worked out on the CPU) -> CA fringe blur -> lens blur (Soft / Radial / Swirl /
Directional) -> vignette -> bloom -> halation, in linear float4 working buffers; the frame kernel then adds Grain, Mix, encodes and writes the AE buffer.
Glow = highlight mask -> pyramid (down, gaussian, up-and-add) -> optional Blur and Stretch -> tinted add through the zone.
Working buffers are reused (a free list in the builder), so a 4K frame needs about 5 full-size float4 buffers rather than one per step.
The kernels are split in three libraries (lens/CA, blur, lens blur + vignette + glow) that compile separately: if one fails on a machine, only
frames that need it use the older path, and `gpu_log.sh` says which group is missing.
Tests: `tests/run_gpu_kernel_test.sh` now runs 20 heavy-effect cases (every lens blur type, all edge modes, CA blur incl. the reduced-size gaussian,
glow with blur/stretch/tint/zone, everything combined with Mix and Grain) through the real plan builder and the shipped kernels as C++ against the CPU core
(differences <= 6e-5, mostly ~1e-7). Mutation-checked: breaking a kernel line makes the tests fail. `tests/mock_host_test.cpp`: 83 checks.
Not testable outside a Mac: the Metal compile and run itself (the C++ run uses the same text, not the Metal compiler).

## Build 13: GPU in Premiere Pro (Metal)

The plug-in binary now also exports `xGPUFilterEntry`, Premiere's own GPU-filter entry point (file `KaptainFilmPremiere.cpp`, built with the
Premiere SDK headers only; it must never include After Effects headers). It uses the same Metal kernels and the same plan as After Effects:
the Metal side was split into `kf_gpu_make_data` (compile the pipelines for a device) and `kf_gpu_exec` (render one frame), which both hosts call.
Premiere hands over its frame buffer (32-bit float, or 16-bit float which is converted to float4 and back by two small kernels) and the effect
parameters; `kf_prem_prepare` (in `KaptainFilm.cpp`) turns those values into exactly the same frame parameters and plan as the AE path.
If anything is unexpected (Show Zones on, unknown parameter layout, a value of an unexpected type, missing kernel group) the GPU filter returns an
error and Premiere renders that frame with the normal CPU effect: the picture is the same, only slower.
`gpu_log.sh` shows (numbers only, macOS hides strings): parameter count, type and value of every parameter (first frame), and
"first frame rendered on the GPU (32-bit / 16-bit float)" once it works.
Guesses that only a real Premiere run can confirm: premultiplied alpha in Premiere's GPU frames; colour parameters arriving as an integer 0xAARRGGBB;
popup values 1-based; angles in degrees; parameter list with or without the group slots (both layouts are understood).
Tests: kernel test 40 checks (adds the half <-> float kernels), mock host test 95 checks (adds: the Premiere values give identical frame parameters and
plan to the AE path in both parameter layouts, light path, and 5 refusal cases). Not testable outside a Mac with Premiere: the Premiere
suites themselves and whether Premiere picks the GPU entry point.

## Build 14: Premiere now offers the GPU path (32-bit float declared)

Build 13 was loaded by Premiere but its GPU entry point was never called. Cause: in Premiere an effect has to declare the pixel formats it
works in (`PF_PixelFormatSuite`, in GLOBAL_SETUP); without a 32-bit float format Premiere treats it as an 8-bit effect and never asks for the GPU filter
(the Premiere GPU samples do the same). Now `GlobalSetup` declares `PrPixelFormat_BGRA_4444_32f` when the host is Premiere.
Consequence for the CPU path in Premiere: frames now arrive as BGRA float (memory order B, G, R, A), so `detect_fmt` asks Premiere for the real
format and `run_core` reads/writes it (`Fmt::PREM32`). Mock test: the Premiere BGRA result is bit-identical to the After Effects ARGB result (98 checks).
Diagnostics: the Premiere part also writes `/tmp/kaptainsfilmlook_log.txt` (shown by `gpu_log.sh`), so a missing macOS log line can be told from a missing call.
Unverified: that Premiere really calls the GPU filter now, and that Premiere's BGRA float frames are premultiplied like AE's.

## Build 16: Premiere status (honest summary) and clean-up

What was observed on a real Mac (Premiere Pro 2027, Metal): Premiere loads the plug-in, accepts the declared 32-bit float BGRA format and renders the
effect through the classic CPU path (commands sequence setup / frame setup / render / frame setdown, pixel format BGRA 32f). Premiere **never calls**
`xGPUFilterEntry`, so in Premiere the effect runs on the CPU (same picture as After Effects by the mock test, only slower). The GPU code for Premiere
(`KaptainFilmPremiere.cpp`) is in the binary and exported (`nm` confirms), but it has never run and is unverified. Why Premiere skips it is unknown.
Compared with Adobe's own GPU sample the PiPL flags and GlobalSetup are the same; the only visible difference is the declared format (Adobe: VUYA 32f).

Unverified in Premiere: premultiplied alpha and channel order of the CPU frames against After Effects (only the mock test covers it).

Build 16 removes the build-15 diagnostics (no more writing to `/tmp` per frame in Premiere). Version 16 (PiPL 32784). Tests: mock host 98, kernel 40.

After Effects: GPU (Metal) path finished and verified (build 12).

## Build 17: renamed to "Kaptains Film Look"

New name everywhere: effect name, About text, bundle `KaptainsFilmLook.plugin`, bundle id `dk.kaptainen.KaptainsFilmLook`, log tags, zip and installer names.
The match name changed to `KAPTAIN KaptainsFilmLook`: projects that used the old effect lose it and need the new one applied again.
The source files keep their old names (`KaptainFilm.cpp` etc.). The installers also move/remove an old `KaptainFilm.plugin` so the two never load together.

## Build 18: open source (GPL v3)

The source code is public on GitHub under GPL-3.0-or-later with an additional permission for the Adobe SDKs (`LICENSE`, `LICENSE-EXCEPTION`).
Every source file has a license header, the release zip now also contains both license files, and `READ ME FIRST.txt` says "Free and open source (GPL v3)".
The SDK folder was renamed from `Adobe SDK/udpakket` to `Adobe SDK/unpacked`. The core now includes `<cstddef>`, so it also builds on Linux.
No change to the effect itself or its parameters. Version 18 (PiPL 32786).
