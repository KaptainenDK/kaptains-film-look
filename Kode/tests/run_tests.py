#!/usr/bin/env python3
# Kaptains Film Look - Copyright (C) 2026 Henrik Jonas Kaptain
# SPDX-License-Identifier: GPL-3.0-or-later (with an additional permission for the Adobe SDKs, see LICENSE-EXCEPTION)
"""Automatic checks for the Kaptains Film Look core. Run: python3 -I run_tests.py"""
import math
import os
import sys
import time

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
import kfio  # noqa: E402

RESULTS = []


def check(name, ok, detail=""):
    RESULTS.append(ok)
    print(("PASS  " if ok else "FAIL  ") + name + (f"   [{detail}]" if detail else ""))


def smooth(x):
    x = np.clip(x, 0, 1)
    return x * x * (3 - 2 * x)


def scene(w, h):
    """Resolution-independent linear test scene (coordinates in units of the image diagonal)."""
    diag = math.hypot(w, h)
    ys, xs = np.mgrid[0:h, 0:w].astype(np.float32)
    u = (xs + 0.5 - w / 2) / diag
    v = (ys + 0.5 - h / 2) / diag
    px = 1.0 / diag  # one pixel in diag units
    img = np.zeros((h, w, 3), np.float32)
    img += (0.05 + 0.10 * (u + 0.5))[..., None]
    img[..., 2] += 0.05 * (v + 0.5)

    def disc(cx, cy, r, col):
        d = np.hypot(u - cx, v - cy)
        a = 1 - smooth((d - r) / (1.5 * px) + 0.5)
        for c in range(3):
            img[..., c] = img[..., c] * (1 - a) + col[c] * a

    disc(-0.25, -0.10, 0.025, (4.0, 4.0, 4.0))
    disc(0.28, 0.12, 0.018, (3.0, 2.5, 1.5))
    disc(0.05, 0.20, 0.012, (2.0, 2.0, 2.2))
    # grid lines (for distortion) and a coloured edge (for chromatic aberration)
    for k in range(-4, 5):
        for coord in (u, v):
            a = 1 - smooth((np.abs(coord - k * 0.1) - 0.0025) / (1.5 * px) + 0.5)
            img += 0.5 * a[..., None]
    edge = smooth((u - 0.3) / (1.5 * px) + 0.5)
    img[..., 0] += 0.4 * edge * (v > 0.1)
    return kfio.to_rgba(np.clip(img, 0, None))


def run(img, **kw):
    kw.setdefault("input_space", 0)  # tests are done in linear light
    return kfio.run(img, **kw)


def down(img_rgba, w, h):
    return kfio.resize(img_rgba[..., :3], w, h)


# ----------------------------------------------------------------------------------
print("== basics ==")
img = scene(800, 450)
out, _ = run(img)
check("identity with default parameters (all amounts 0)", np.abs(out - img).max() < 1e-6)
out, _ = run(img, lens_amount=0.5, ca_amount=3, vig_amount=1, bloom_intensity=0.3, halation_intensity=0.3, grain_amount=1, mix=0)
check("mix=0 returns the original untouched", np.abs(out - img).max() < 1e-6)
out, _ = run(img, vig_amount=1, grain_amount=1)
check("alpha channel passes through unchanged", np.abs(out[..., 3] - 1).max() < 1e-6)

# ----------------------------------------------------------------------------------
print("== lens distortion / auto-zoom / edge modes ==")
flat = kfio.to_rgba(np.full((450, 800, 3), 0.4, np.float32))
for amt in (0.3, 0.8, 1.0, -0.5, -1.0):
    out, z = run(flat, lens_amount=amt, ca_amount=2, lens_auto_zoom=1, lens_edge=0)
    check(f"auto-zoom leaves no empty edges (amount {amt:+.1f}, transparent edge mode)",
          out[..., 3].min() > 0.995, f"min alpha {out[..., 3].min():.4f}, zoom {z:.3f}")
out, z = run(flat, lens_amount=0.8, lens_auto_zoom=0, lens_edge=0)
check("with auto-zoom off barrel distortion shows empty corners in transparent mode", out[..., 3].min() < 0.5,
      f"min alpha {out[..., 3].min():.3f}")
for em, nm in ((1, "clamp"), (2, "mirror")):
    out, _ = run(flat, lens_amount=0.8, lens_auto_zoom=0, lens_edge=em)
    check(f"edge mode {nm} fills the corners with picture (no black)", abs(out[0, 0, 0] - 0.4) < 1e-3, f"corner {out[0,0,0]:.3f}")
out, z = run(flat, lens_amount=-1.0, lens_auto_zoom=1)
check("pincushion needs no zoom (zoom = 1.000)", abs(z - 1) < 1e-6, f"zoom {z:.5f}")

# a single dot: direction and manual zoom
dot = np.zeros((600, 1000, 4), np.float32); dot[..., 3] = 1
cy, cx = 300, 500
py, px_ = 300 + 100, 500 + 300  # dot at (+300, +100) from the centre
dot[py - 2:py + 3, px_ - 2:px_ + 3, :3] = 1.0
def centroid(a):
    m = a[..., 0]; ys, xs = np.mgrid[0:m.shape[0], 0:m.shape[1]]
    s = m.sum(); return (xs * m).sum() / s, (ys * m).sum() / s
out, _ = run(dot, lens_amount=0.5, lens_auto_zoom=0, lens_edge=0)
x1, y1 = centroid(out)
r0, r1 = math.hypot(300, 100), math.hypot(x1 - 500 + 0.5 - 0.5, y1 - 300)
check("barrel (+) pulls content towards the centre", r1 < r0 - 1, f"radius {r0:.1f} -> {r1:.1f}")
out, _ = run(dot, lens_amount=-0.5, lens_auto_zoom=0, lens_edge=0)
x1, y1 = centroid(out); r1 = math.hypot(x1 - 500, y1 - 300)
check("pincushion (-) pushes content away from the centre", r1 > r0 + 1, f"radius {r0:.1f} -> {r1:.1f}")
dot_z = np.zeros((600, 1000, 4), np.float32); dot_z[..., 3] = 1
dot_z[300 + 50 - 2:300 + 50 + 3, 500 + 150 - 2:500 + 150 + 3, :3] = 1.0   # dot at (+150, +50)
rz0 = math.hypot(150, 50)
out, _ = run(dot_z, lens_zoom=2.0, lens_amount=0.0001, lens_auto_zoom=0, lens_edge=0)
x1, y1 = centroid(out); rz1 = math.hypot(x1 - 500, y1 - 300)
check("manual zoom 2.0 doubles the distance from the centre", abs(rz1 - 2 * rz0) < 1.0, f"radius {rz0:.1f} -> {rz1:.1f}")

# ----------------------------------------------------------------------------------
print("== chromatic aberration ==")
dot2 = np.zeros((600, 1000, 4), np.float32); dot2[..., 3] = 1
dot2[300 - 3:300 + 4, 500 + 400 - 3:500 + 400 + 4, :3] = 1.0  # white dot 400 px right of the centre
out, _ = run(dot2, ca_amount=10, lens_auto_zoom=0, lens_edge=0)
cr = centroid(np.stack([out[..., 0]] * 4, -1))[0]
cg = centroid(np.stack([out[..., 1]] * 4, -1))[0]
cb = centroid(np.stack([out[..., 2]] * 4, -1))[0]
expect = 400 * 0.001 * 10
check("green channel stays in place", abs(cg - (500 + 400)) < 0.2, f"green x {cg:.2f}")
check("red and blue shift in opposite directions by the expected amount",
      abs((cr - cg) + expect) < 0.4 and abs((cb - cg) - expect) < 0.4 or abs((cr - cg) - expect) < 0.4 and abs((cb - cg) + expect) < 0.4,
      f"red {cr - cg:+.2f}px blue {cb - cg:+.2f}px expected ±{expect:.2f}")
out, _ = run(dot2.copy(), ca_amount=10)
dot_c = np.zeros((600, 1000, 4), np.float32); dot_c[..., 3] = 1; dot_c[297:304, 497:504, :3] = 1.0
outc, _ = run(dot_c, ca_amount=10, lens_auto_zoom=0, lens_edge=0)
check("no colour fringing at the exact centre", abs(centroid(np.stack([outc[..., 0]] * 4, -1))[0] - centroid(np.stack([outc[..., 2]] * 4, -1))[0]) < 0.05)

# ----------------------------------------------------------------------------------
print("== vignette ==")
grey = kfio.to_rgba(np.full((540, 960, 3), 0.5, np.float32))
out, _ = run(grey, vig_amount=1.5, vig_zone_start=0.3, vig_softness=0.3, zone_roundness=1)
corner = out[0, 0, 0] / 0.5
check("centre untouched, corner darkened by the requested stops", abs(out[270, 480, 0] - 0.5) < 1e-4 and abs(corner - 2 ** -1.5) < 0.02,
      f"corner gain {corner:.3f} vs {2 ** -1.5:.3f}")
# rotation invariance with a circular vignette (same diagonal, same look)
g1 = kfio.to_rgba(np.full((540, 960, 3), 0.5, np.float32)); g2 = kfio.to_rgba(np.full((960, 540, 3), 0.5, np.float32))
o1, _ = run(g1, vig_amount=1.5, zone_roundness=1); o2, _ = run(g2, vig_amount=1.5, zone_roundness=1)
check("circular vignette is identical for landscape and portrait (rotated 90°)", np.abs(np.rot90(o2[..., :3]) - o1[..., :3]).max() < 1e-4,
      f"max diff {np.abs(np.rot90(o2[..., :3]) - o1[..., :3]).max():.2e}")
# letterbox: vignette measured from the active picture area only
lb = np.zeros((540, 960, 4), np.float32); lb[..., 3] = 1
y0, y1 = int(540 * 0.125), int(540 * 0.875); lb[y0:y1, :, :3] = 0.5
out, _ = run(lb, vig_amount=1.5, zone_roundness=0, vig_zone_start=0.3, active_area=(0, 0.125, 1, 0.875))
c_active = out[y0, 0, 0] / 0.5
out_full, _ = run(lb, vig_amount=1.5, zone_roundness=0, vig_zone_start=0.3)
c_full = out_full[y0, 0, 0] / 0.5
check("letterbox: vignette uses the active picture area (corner of the picture is fully darkened)",
      abs(c_active - 2 ** -1.5) < 0.03 and c_full > c_active + 0.05, f"active {c_active:.3f}, full frame {c_full:.3f}")
out_oval, _ = run(g2, vig_amount=1.5, zone_roundness=0)
check("oval mode follows the frame shape (edge middles darker than for circular)", out_oval[270, 0, 0] < o2[270, 0, 0] - 0.01,
      f"oval {out_oval[480,0,0]:.3f} vs circular {o2[480,0,0]:.3f}")

# ----------------------------------------------------------------------------------
print("== bloom / halation ==")
dsc = np.zeros((600, 900, 4), np.float32); dsc[..., 3] = 1
yy, xx = np.mgrid[0:600, 0:900]; m = (np.hypot(xx - 450, yy - 300) < 12)
dsc[m, :3] = 3.0
out, _ = run(dsc, halation_intensity=0.5, halation_threshold=0.8, halation_radius=2.0)
added = (out[..., :3] - dsc[..., :3]).sum(axis=(0, 1))
src = np.maximum(dsc[..., 0] - 0.8, 0).sum()
exp = np.array([1.0, 0.30, 0.06]) * 0.5 * src
check("halation adds red-orange glow with the requested tint and energy",
      np.allclose(added, exp, rtol=0.06), f"added {np.round(added)} expected {np.round(exp)}")
out, _ = run(dsc, bloom_intensity=0.4, bloom_threshold=0.6, bloom_radius=2.0)
added = (out[..., :3] - dsc[..., :3]).sum(axis=(0, 1)); src = np.maximum(dsc[..., 0] - 0.6, 0).sum()
check("bloom adds neutral glow with the requested energy", np.allclose(added, 0.4 * src * np.ones(3), rtol=0.06),
      f"added {np.round(added)} expected {np.round(0.4 * src)}")
out, _ = run(dsc, bloom_intensity=0.4, bloom_threshold=3.5, bloom_radius=2.0)
check("nothing glows below the threshold", np.abs(out - dsc).max() < 1e-5)

# ----------------------------------------------------------------------------------
print("== grain ==")
mid = kfio.to_rgba(np.full((600, 900, 3), 0.18, np.float32))   # linear mid grey
def pstd(o): return kfio.srgb_encode(o[..., :3]).std(axis=(0, 1))
o, _ = run(mid, grain_amount=1, grain_color=0, grain_roughness=0, grain_size=1)
s = pstd(o)
check("grain amount 1.0 gives std ~0.05 (display-referred) at mid grey", np.all(np.abs(s - 0.05) < 0.004), f"std {np.round(s, 4)}")
o_mono, _ = run(mid, grain_amount=1, grain_color=0)
p = kfio.srgb_encode(o_mono[..., :3])
check("colour = 0 gives monochrome grain (channels identical)", np.abs(p[..., 0] - p[..., 1]).max() < 2e-3)
o_col, _ = run(mid, grain_amount=1, grain_color=1)
p = kfio.srgb_encode(o_col[..., :3]); c01 = np.corrcoef((p[..., 0] - p[..., 0].mean()).ravel(), (p[..., 1] - p[..., 1].mean()).ravel())[0, 1]
check("colour = 1 gives independent colour channels", abs(c01) < 0.05, f"channel correlation {c01:+.3f}")
a, _ = run(mid, grain_amount=1, frame=1); b, _ = run(mid, grain_amount=1, frame=1); c, _ = run(mid, grain_amount=1, frame=2)
check("same frame gives identical grain; next frame gives new grain", np.abs(a - b).max() == 0 and np.abs(a - c).max() > 0.01)
d, _ = run(mid, grain_amount=1, frame=2, grain_animate=0); e, _ = run(mid, grain_amount=1, frame=7, grain_animate=0)
check("animate off keeps the same grain on every frame", np.abs(d - e).max() == 0)
dark = kfio.to_rgba(np.full((300, 450, 3), 0.001, np.float32)); o, _ = run(dark, grain_amount=1, grain_midtone_bias=1)
check("midtone bias = 1: almost no grain in deep blacks", pstd(o).max() < 0.004, f"std {np.round(pstd(o), 4)}")
# grain size relative to diagonal: after reducing the big frame the texture has the same strength as at native size
# (grain size 3.0 so the grain is larger than a pixel at both resolutions and can be shown in both)
big = kfio.to_rgba(np.full((1350, 2400, 3), 0.18, np.float32)); small = kfio.to_rgba(np.full((675, 1200, 3), 0.18, np.float32))
ob, _ = run(big, grain_amount=1, grain_color=0, grain_size=3); os_, _ = run(small, grain_amount=1, grain_color=0, grain_size=3)
ob_small = kfio.resize(kfio.srgb_encode(ob[..., :3]), 1200, 675)
sb, ss = ob_small.std(), kfio.srgb_encode(os_[..., :3]).std()
check("grain looks the same at 2400x1350 and 1200x675 (size is relative to the diagonal)", abs(sb / ss - 1) < 0.15,
      f"std after reduction {sb:.4f} vs native {ss:.4f}")

# ----------------------------------------------------------------------------------
print("== same look at every resolution and every format ==")
settings = dict(lens_amount=0.35, ca_amount=3, vig_amount=1.0, bloom_intensity=0.3, halation_intensity=0.4)
ref_w, ref_h = 800, 450
ref_in = scene(ref_w, ref_h)
ref_out, _ = run(ref_in, **settings)
effect = np.abs(ref_out[..., :3] - ref_in[..., :3]).mean()
for (w, h) in ((1600, 900), (2400, 1350), (400, 225)):
    big_in = scene(w, h)
    big_out, _ = run(big_in, **settings)
    cmp_big = down(big_out, ref_w, ref_h)
    err = np.abs(cmp_big - ref_out[..., :3]).mean()
    check(f"{w}x{h} matches 800x450 after scaling to the same size", err < 0.12 * effect,
          f"error {err:.4f} vs effect size {effect:.4f}  ({100 * err / effect:.1f}% of the effect)")
# orientation: rotate the scene 90 degrees, process, rotate back
rot_in = np.ascontiguousarray(np.rot90(ref_in))
rot_out, _ = run(rot_in, **settings)
back = np.rot90(rot_out, -1)
err = np.abs(back[..., :3] - ref_out[..., :3]).mean()
check("portrait (rotated 90°) gives the same result as landscape", err < 0.05 * effect, f"error {100 * err / effect:.1f}% of the effect")
for (w, h, nm) in ((1000, 1000, "1:1"), (810, 1080, "3:4"), (864, 1080, "4:5"), (608, 1080, "9:16"), (1920, 804, "2.39:1")):
    s_in = scene(w, h)
    s_out, z = run(s_in, lens_edge=0, **settings)
    check(f"{nm} ({w}x{h}): no empty edges and sane zoom", s_out[..., 3].min() > 0.995 and 1.0 <= z < 1.3, f"min alpha {s_out[...,3].min():.3f}, zoom {z:.3f}")
# preview at reduced resolution (host renders a half-size frame)
half_in = scene(400, 225); half_out, _ = run(half_in, **settings)
err = np.abs(down(half_out, ref_w, ref_h) - ref_out[..., :3]).mean()
check("half-resolution preview looks like the full-resolution render", err < 0.12 * effect, f"error {100 * err / effect:.1f}% of the effect")

# ----------------------------------------------------------------------------------
print("== zones (Start / Full), movable centre, roundness ==")
W, H = 1000, 600
def dot_img(x, y, r=3, v=1.0):
    a = np.zeros((H, W, 4), np.float32); a[..., 3] = 1
    a[y - r:y + r + 1, x - r:x + r + 1, :3] = v
    return a
def moments(o, ch=0):
    m = np.maximum(o[..., ch], 0); s = m.sum(); ys, xs = np.mgrid[0:m.shape[0], 0:m.shape[1]]
    cx = (xs * m).sum() / s; cy = (ys * m).sum() / s
    return cx, cy, (m * (xs - cx) ** 2).sum() / s, (m * (ys - cy) ** 2).sum() / s, s

# lens zone: inside Start untouched, outside Full fully effective
near, far = dot_img(600, 300), dot_img(950, 300)
o_n, _ = run(near, lens_amount=0.5, lens_auto_zoom=0, lens_edge=0, lens_zone_start=0.5, lens_zone_full=1.0)
o_f, _ = run(far, lens_amount=0.5, lens_auto_zoom=0, lens_edge=0, lens_zone_start=0.5, lens_zone_full=1.0)
o_all, _ = run(far, lens_amount=0.5, lens_auto_zoom=0, lens_edge=0)
check("lens zone: dot inside Start is not moved", abs(moments(o_n)[0] - 600) < 0.05, f"x {moments(o_n)[0]:.2f}")
check("lens zone: dot outside Full is moved, less than with no zone",
      moments(o_f)[0] < 949 and moments(o_f)[0] > moments(o_all)[0] - 0.001, f"x {moments(o_f)[0]:.1f} (no zone {moments(o_all)[0]:.1f})")
# moved centre: the optical centre follows the zone centre
o_c, _ = run(dot_img(250, 300), lens_amount=0.8, lens_auto_zoom=0, lens_edge=0, zone_center_x=0.25)
check("moved zone centre is also the lens centre (dot at the new centre stays)", abs(moments(o_c)[0] - 250) < 0.05)
o_c, _ = run(dot_img(350, 300), lens_amount=0.8, lens_auto_zoom=0, lens_edge=0, zone_center_x=0.25)
check("distortion pulls towards the moved centre", moments(o_c)[0] < 349.5, f"x {moments(o_c)[0]:.2f}")
for cxy in ((0.25, 0.5), (0.7, 0.3)):
    o, z = run(flat, lens_amount=0.8, ca_amount=2, lens_zone_start=0.3, lens_zone_full=1.0, zone_center_x=cxy[0], zone_center_y=cxy[1], lens_edge=0)
    check(f"auto-zoom with moved centre {cxy} and zone leaves no empty edges", o[..., 3].min() > 0.995, f"min alpha {o[...,3].min():.4f}, zoom {z:.3f}")
# vignette with moved centre
g = kfio.to_rgba(np.full((540, 960, 3), 0.5, np.float32))
o, _ = run(g, vig_amount=2, zone_center_x=0.25, zone_center_y=0.5, vig_zone_start=0.2, vig_zone_full=0.8)
check("vignette: moved centre is bright, far side is dark", abs(o[270, 240, 0] - 0.5) < 1e-4 and o[270, 900, 0] < 0.2, f"{o[270,240,0]:.3f} / {o[270,900,0]:.3f}")
o_c, _ = run(g, vig_amount=1.5, zone_roundness=1, vig_zone_start=0.3, vig_zone_full=0.9)
o_o, _ = run(g, vig_amount=1.5, zone_roundness=0, vig_zone_start=0.3, vig_zone_full=0.9)
check("roundness 0 (oval) follows the frame: short-side edge middle darker than with a circle", o_o[0, 480, 0] < o_c[0, 480, 0] - 0.01)
o_e, _ = run(g, vig_amount=1.5, vig_zone_start=0.0, vig_zone_full=0.0)
check("Start = Full = 0 means 'everywhere': uniform gain, no zone", np.abs(o_e[..., 0] - 0.5 * 2 ** -1.5).max() < 1e-4)

# ----------------------------------------------------------------------------------
print("== lens blur types ==")
kw = dict(lblur_amount=3.0, lblur_zone_start=0.3, lblur_zone_full=0.6, vig_amount=0)
d_far = dot_img(800, 300)            # +300 px right of centre: radial = along x, swirl = along y
m0 = moments(d_far)
res = {}
for t, nm in ((0, "soft"), (1, "radial"), (2, "swirl")):
    o, _ = run(d_far, lblur_enable=1, lblur_type=t, **kw)
    res[nm] = moments(o)
    check(f"lens blur {nm}: energy conserved and centre of mass stays", abs(res[nm][4] / m0[4] - 1) < 0.03 and abs(res[nm][0] - 800) < (4 if nm == "soft" else 1.5) and abs(res[nm][1] - 300) < 1.5,
          f"energy {res[nm][4] / m0[4]:.3f}, centroid ({res[nm][0]:.1f},{res[nm][1]:.1f})")
check("lens blur soft: spreads equally in x and y", res["soft"][2] > m0[2] + 5 and abs(res["soft"][2] / res["soft"][3] - 1) < 0.15,
      f"var x {res['soft'][2]:.1f} y {res['soft'][3]:.1f}")
check("lens blur radial: streaks along the radius (x)", res["radial"][2] > 3 * res["radial"][3] and res["radial"][2] > m0[2] + 5,
      f"var x {res['radial'][2]:.1f} y {res['radial'][3]:.1f}")
check("lens blur swirl: streaks around the centre (y here)", res["swirl"][3] > 3 * res["swirl"][2] and res["swirl"][3] > m0[3] + 5,
      f"var x {res['swirl'][2]:.1f} y {res['swirl'][3]:.1f}")
for ang, ax in ((0, "x"), (90, "y")):
    o, _ = run(d_far, lblur_enable=1, lblur_type=3, lblur_angle=ang, **kw)
    mm = moments(o); big, small = (mm[2], mm[3]) if ax == "x" else (mm[3], mm[2])
    check(f"lens blur directional {ang}°: streaks along {ax}", big > 3 * small and big > 5, f"var x {mm[2]:.1f} y {mm[3]:.1f}")
o, _ = run(dot_img(500, 300), lblur_enable=1, lblur_type=1, **kw)
check("lens blur zone: dot at the centre (inside Start) stays sharp", np.abs(o - dot_img(500, 300)).max() < 1e-5)
o_s, _ = run(d_far, lblur_enable=1, lblur_type=0, lblur_amount=3.0, lblur_zone_start=0.0, lblur_zone_full=0.0)
o_s2, _ = run(dot_img(500, 300), lblur_enable=1, lblur_type=0, lblur_amount=3.0, lblur_zone_start=0.0, lblur_zone_full=0.0)
check("lens blur Start = Full = 0 blurs everywhere (also the centre)", moments(o_s2)[2] > 20, f"var x {moments(o_s2)[2]:.1f}")

# ----------------------------------------------------------------------------------
print("== blur on chromatic aberration ==")
dd = dot_img(900, 300, r=4); dd[..., :3] += 0.2
o0, _ = run(dd, ca_amount=15, lens_auto_zoom=0, lens_edge=0, ca_blur=0)
o1, _ = run(dd, ca_amount=15, lens_auto_zoom=0, lens_edge=0, ca_blur=6)
check("CA blur softens the red/blue fringe (red peak falls) and conserves its energy",
      (o1[..., 0] - o1[..., 1]).std() < 0.8 * (o0[..., 0] - o0[..., 1]).std() and abs(o1[..., 0].sum() / o0[..., 0].sum() - 1) < 0.02,
      f"R-G spread {(o0[...,0]-o0[...,1]).std():.3f} -> {(o1[...,0]-o1[...,1]).std():.3f}, energy {o1[...,0].sum() / o0[...,0].sum():.3f}")
check("CA blur keeps the fringe position (red centroid)", abs(moments(o0, 0)[0] - moments(o1, 0)[0]) < 0.6, f"{moments(o0,0)[0]:.2f} vs {moments(o1,0)[0]:.2f}")
check("CA blur keeps the green channel unchanged", np.abs(o0[..., 1] - o1[..., 1]).max() < 1e-4)

# ----------------------------------------------------------------------------------
print("== bloom / halation: blur, stretch, zone ==")
dsc2 = np.zeros((600, 1000, 4), np.float32); dsc2[..., 3] = 1
yy, xx = np.mgrid[0:600, 0:1000]; dsc2[np.hypot(xx - 500, yy - 300) < 10, :3] = 3.0
def added(**k):
    o, _ = run(dsc2, **k); a = o.copy(); a[..., :3] -= dsc2[..., :3]; a[..., 3] = 1; return a
b0 = added(bloom_intensity=0.5, bloom_radius=2.0)
b1 = added(bloom_intensity=0.5, bloom_radius=2.0, bloom_blur=1.0)
check("bloom blur softens the glow (peak falls) and conserves energy", b1[..., 0].max() < 0.8 * b0[..., 0].max() and abs(b1[..., 0].sum() / b0[..., 0].sum() - 1) < 0.03,
      f"peak {b0[...,0].max():.3f} -> {b1[...,0].max():.3f}, energy {b1[...,0].sum() / b0[...,0].sum():.3f}")
sx = added(bloom_intensity=0.5, bloom_radius=2.0, bloom_stretch=1.0, bloom_stretch_angle=0)
sy = added(bloom_intensity=0.5, bloom_radius=2.0, bloom_stretch=1.0, bloom_stretch_angle=90)
mx, my = moments(sx), moments(sy)
check("bloom stretch at 0° makes the glow wider than tall", mx[2] > 2 * mx[3], f"var x {mx[2]:.0f} y {mx[3]:.0f}")
check("bloom stretch at 90° makes the glow taller than wide", my[3] > 2 * my[2], f"var x {my[2]:.0f} y {my[3]:.0f}")
check("bloom stretch conserves energy", abs(mx[4] / moments(b0)[4] - 1) < 0.05, f"{mx[4] / moments(b0)[4]:.3f}")
hs = added(halation_intensity=0.5, halation_radius=2.0, halation_stretch=1.0, halation_stretch_angle=0)
mh = moments(hs)
check("halation stretch works too (wider than tall)", mh[2] > 2 * mh[3], f"var x {mh[2]:.0f} y {mh[3]:.0f}")
bz = added(bloom_intensity=0.5, bloom_radius=2.0, bloom_zone_start=0.3, bloom_zone_full=0.6)
check("bloom zone: highlight at the centre gives (almost) no glow inside Start", bz[..., 0].sum() < 0.03 * b0[..., 0].sum(), f"{bz[...,0].sum() / b0[...,0].sum():.3f}")
dsc3 = dsc2.copy(); dsc3[np.hypot(xx - 500, yy - 300) < 10, :3] = 0; dsc3[np.hypot(xx - 900, yy - 300) < 10, :3] = 3.0
o, _ = run(dsc3, bloom_intensity=0.5, bloom_radius=2.0, bloom_zone_start=0.3, bloom_zone_full=0.6)
o2, _ = run(dsc3, bloom_intensity=0.5, bloom_radius=2.0)
check("bloom zone: highlight outside Full glows fully", abs((o[..., 0] - dsc3[..., 0]).sum() / (o2[..., 0] - dsc3[..., 0]).sum() - 1) < 0.05)

# ----------------------------------------------------------------------------------
print("== grain zone ==")
mid2 = kfio.to_rgba(np.full((600, 900, 3), 0.18, np.float32))
o, _ = run(mid2, grain_amount=1, grain_color=0, grain_zone_start=0.3, grain_zone_full=0.6)
pc = kfio.srgb_encode(o[..., :3])
check("grain zone: centre has no grain, edges have full grain", pc[250:350, 400:500].std() < 0.002 and pc[:, :60].std() > 0.04,
      f"centre {pc[250:350, 400:500].std():.4f}, edge {pc[:, :60].std():.4f}")

# ----------------------------------------------------------------------------------
print("== Show Zones overlay ==")
g = kfio.to_rgba(np.full((540, 960, 3), 0.5, np.float32))
a, _ = run(g, vig_amount=1.0, show_zones=0); b, _ = run(g, vig_amount=1.0, show_zones=1)
Rz = math.hypot(480, 270)
yy2, xx2 = np.mgrid[0:540, 0:960]; dist = np.hypot(xx2 + 0.5 - 480, yy2 + 0.5 - 270)
chg = np.abs(a[..., :3] - b[..., :3]).max(axis=2) > 1e-4
ring = np.abs(dist - 0.35 * Rz) < 4
cross = dist < 20
check("rings: something is drawn", chg.sum() > 100, f"{chg.sum()} pixels")
check("rings: drawn only at the Start ring radius (and centre cross, Full ring at the corner)", not chg[~(ring | cross) & (dist < 0.9 * Rz)].any())
check("rings: Start ring is visible on the horizontal axis", chg[270, int(480 + 0.35 * Rz) - 2:int(480 + 0.35 * Rz) + 3].any() or chg[int(270 + 0.35 * Rz) - 2:int(270 + 0.35 * Rz) + 3, 480].any())
c, _ = run(g, vig_amount=1.0, vig_zone_start=0, vig_zone_full=0, show_zones=1)
check("rings: nothing drawn when all zones are 'everywhere'", np.abs(c - run(g, vig_amount=1.0, vig_zone_start=0, vig_zone_full=0)[0]).max() == 0)
c, _ = run(g, show_zones=1)
check("rings: nothing drawn when no effect is active", np.abs(c - g).max() < 1e-6)
d, _ = run(g, vig_amount=1.0, show_zones=0)
check("show_zones off: picture identical to a plain render", np.abs(d - a).max() == 0)

# ----------------------------------------------------------------------------------
print("== v2 effects at every resolution / orientation ==")
s2 = dict(lblur_enable=1, lblur_type=1, lblur_amount=1.5, lblur_zone_start=0.4, lblur_zone_full=0.9,
          ca_amount=4, ca_blur=1.0, ca_zone_start=0.3, ca_zone_full=0.9, lens_amount=0.3, lens_zone_start=0.3, lens_zone_full=0.9,
          vig_amount=1.0, bloom_intensity=0.3, bloom_blur=0.5, bloom_zone_start=0.2, bloom_zone_full=0.7,
          halation_intensity=0.4, zone_center_x=0.4, zone_center_y=0.55)
r_in = scene(800, 450); r_out, _ = run(r_in, **s2)
eff2 = np.abs(r_out[..., :3] - r_in[..., :3]).mean()
for (w, h) in ((1600, 900), (400, 225)):
    bo, _ = run(scene(w, h), **s2)
    err = np.abs(down(bo, 800, 450) - r_out[..., :3]).mean()
    check(f"v2 look at {w}x{h} matches 800x450 after scaling", err < 0.15 * eff2, f"{100 * err / eff2:.1f}% of the effect")
for t, nm in ((0, "soft"), (1, "radial"), (2, "swirl")):
    s3 = dict(s2, lblur_type=t, zone_center_x=0.5, zone_center_y=0.5)
    base, _ = run(r_in, **s3)
    rb, _ = run(np.ascontiguousarray(np.rot90(r_in)), **s3)
    err = np.abs(np.rot90(rb, -1)[..., :3] - base[..., :3]).mean()
    e3 = np.abs(base[..., :3] - r_in[..., :3]).mean()
    check(f"portrait gives the same result as landscape with lens blur {nm}", err < 0.06 * e3, f"{100 * err / e3:.1f}% of the effect")
for (w, h, nm) in ((1000, 1000, "1:1"), (864, 1080, "4:5"), (608, 1080, "9:16"), (1920, 804, "2.39:1")):
    o, z = run(scene(w, h), lens_edge=0, **s2)
    check(f"{nm}: no empty edges with zones and moved centre", o[..., 3].min() > 0.995 and z < 1.4, f"min alpha {o[...,3].min():.3f}, zoom {z:.3f}")

# ----------------------------------------------------------------------------------
print("== multi-threading ==")
import subprocess, tempfile, hashlib
def run_hash(rgba, threads, args):
    with tempfile.TemporaryDirectory() as d:
        a, b = os.path.join(d, "in.kfi"), os.path.join(d, "out.kfi")
        kfio.write_kfi(a, rgba)
        env = dict(os.environ); env["KF_THREADS"] = str(threads)
        subprocess.run([kfio.CLI, a, b] + args, check=True, capture_output=True, env=env)
        return hashlib.sha256(open(b, "rb").read()).hexdigest()
mt_in = scene(613, 331)
mt_args = ("lens_enable=1 lens_amount=0.2 ca_enable=1 ca_amount=2 ca_blur=0.4 lblur_enable=1 lblur_type=2 lblur_amount=1.5 "
           "vig_enable=1 vig_amount=1 bloom_enable=1 bloom_intensity=0.4 bloom_stretch=0.4 halation_enable=1 halation_intensity=0.4 "
           "grain_enable=1 grain_amount=0.5 grain_color=0.5 grain_roughness=0.3 frame=4").split()
h1 = run_hash(mt_in, 1, mt_args)
check("multi-threaded result is bit-identical to single-threaded (2 threads)", run_hash(mt_in, 2, mt_args) == h1)
check("multi-threaded result is bit-identical to single-threaded (7 threads)", run_hash(mt_in, 7, mt_args) == h1)
check("multi-threaded result is bit-identical to single-threaded (default thread count)", run_hash(mt_in, 0, mt_args) == h1)

# ----------------------------------------------------------------------------------
print("== speed (CPU reference, 2 cores) ==")
big12 = scene(4000, 2250)  # ~9 MP, UHD-ish
t = time.time(); run(big12, lens_amount=0.3, ca_amount=2, vig_amount=1, bloom_intensity=0.3, halation_intensity=0.4, grain_amount=1)
print(f"INFO  4000x2250, all six effects: {time.time() - t:.1f}s (includes file I/O)")

print()
print(f"{sum(RESULTS)} / {len(RESULTS)} checks passed")
sys.exit(0 if all(RESULTS) else 1)
