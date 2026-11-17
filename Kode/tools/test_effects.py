#!/usr/bin/env python3
# Kaptains Film Look - Copyright (C) 2026 Henrik Jonas Kaptain
# SPDX-License-Identifier: GPL-3.0-or-later (with an additional permission for the Adobe SDKs, see LICENSE-EXCEPTION)
"""Exaggerated one-effect-at-a-time contact sheet, to check that every effect is visible.
usage: KF_IMAGES=<folder> test_effects.py NAME   (reads NAME.raf and NAME.JPG from KF_IMAGES)
"""
import os
import sys

import numpy as np
import rawpy
from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kfio  # noqa: E402

SRC = os.environ.get("KF_IMAGES", "images")          # folder with NAME.raf + NAME.JPG (camera JPG for comparison)
OUT = os.environ.get("KF_OUT", "results")            # output folder, created if missing
name = sys.argv[1]

Z = dict(zone_center_x=0.5, zone_center_y=0.5)
ALL = dict(lens_amount=0.5, lens_zone_start=0.3, lens_zone_full=1.0,
           ca_amount=8, ca_blur=1.0, ca_zone_start=0.3, ca_zone_full=1.0,
           lblur_enable=1, lblur_type=1, lblur_amount=2.0, lblur_zone_start=0.45, lblur_zone_full=1.0,
           vig_amount=2.0, vig_zone_start=0.3, vig_zone_full=1.0,
           bloom_intensity=0.8, bloom_threshold=0.5, bloom_radius=4, bloom_blur=0.4, bloom_stretch=0.6,
           halation_intensity=1.5, halation_threshold=0.6, halation_radius=3, halation_blur=0.2, halation_stretch=0.4,
           grain_amount=1.2, grain_size=2.0, grain_color=0.6, grain_zone_start=0.2, grain_zone_full=0.8)
EFFECTS = [
    ("Original (neutral RAF)", {}),
    ("Lens Distortion 0.9, zone 0.3..1.0", dict(lens_amount=0.9, lens_zone_start=0.3, lens_zone_full=1.0)),
    ("Chromatic Aberration 12", dict(ca_amount=12)),
    ("CA 12 + CA Blur 3.0", dict(ca_amount=12, ca_blur=3.0)),
    ("Lens Blur SOFT 4, zone 0.4..1.0", dict(lblur_enable=1, lblur_type=0, lblur_amount=4, lblur_zone_start=0.4, lblur_zone_full=1.0)),
    ("Lens Blur RADIAL 5, zone 0.3..0.9", dict(lblur_enable=1, lblur_type=1, lblur_amount=5, lblur_zone_start=0.3, lblur_zone_full=0.9)),
    ("Lens Blur SWIRL 5, zone 0.3..0.9", dict(lblur_enable=1, lblur_type=2, lblur_amount=5, lblur_zone_start=0.3, lblur_zone_full=0.9)),
    ("Lens Blur DIRECTIONAL 4, 0 deg (anamorphic)", dict(lblur_enable=1, lblur_type=3, lblur_amount=4, lblur_angle=0, lblur_zone_start=0.3, lblur_zone_full=0.9)),
    ("Vignette 3 stops, moved centre (0.35, 0.45)", dict(vig_amount=3.0, vig_zone_start=0.15, vig_zone_full=0.9, zone_center_x=0.35, zone_center_y=0.45)),
    ("Bloom 1.2, blur 0.5, stretch 1.5 horizontal", dict(bloom_intensity=1.2, bloom_threshold=0.45, bloom_radius=4, bloom_blur=0.5, bloom_stretch=1.5, bloom_stretch_angle=0)),
    ("Halation 2.0, blur 0.3, stretch 1.0 vertical", dict(halation_intensity=2.0, halation_threshold=0.55, halation_radius=3, halation_blur=0.3, halation_stretch=1.0, halation_stretch_angle=90)),
    ("Grain 2, size 3, colour 1, zone 0.2..0.8", dict(grain_amount=2.0, grain_size=3.0, grain_roughness=0.5, grain_color=1.0, grain_zone_start=0.2, grain_zone_full=0.8)),
    ("All effects together", ALL),
    ("All effects + Show Zones (preview rings)", dict(ALL, show_zones=1)),
    ("Moved centre (0.3, 0.6) + Show Zones", dict(ALL, zone_center_x=0.3, zone_center_y=0.6, show_zones=1)),
]

with rawpy.imread(f"{SRC}/{name}.raf") as r:
    lin16 = r.postprocess(use_camera_wb=True, no_auto_bright=True, output_bps=16, gamma=(1, 1), output_color=rawpy.ColorSpace.sRGB)
h4, w4 = lin16.shape[0] // 4, lin16.shape[1] // 4
lin = lin16[: h4 * 4, : w4 * 4].reshape(h4, 4, w4, 4, 3).astype(np.float32).mean(axis=(1, 3)) / 65535.0
del lin16
jpg = np.asarray(Image.open(f"{SRC}/{name}.JPG").convert("RGB").resize((w4, h4), Image.BILINEAR)).astype(np.float32) / 255.0
luma = lambda x: 0.2126 * x[..., 0] + 0.7152 * x[..., 1] + 0.0722 * x[..., 2]
exposure = np.median(luma(kfio.srgb_decode(jpg))) / np.median(luma(lin))
base = np.clip(kfio.srgb_encode(np.clip(lin * exposure, 0, 1)), 0, 1).astype(np.float32)
print(f"{name}: {w4}x{h4}, clipped {(lin.max(axis=2) >= 0.999).mean() * 100:.1f}%")

PW = 800
font = ImageFont.load_default(size=26)
panels = []
for label, kw in EFFECTS:
    if kw:
        out, z = kfio.run(kfio.to_rgba(base), input_space=1, lens_edge=1, **kw)
        img = out[..., :3]
        extra = f"   (auto-zoom {z:.2f})" if kw.get("lens_amount") else ""
    else:
        img, extra = base, ""
    im = Image.fromarray((np.clip(img, 0, 1) * 255 + .5).astype(np.uint8)).resize((PW, int(h4 * PW / w4)), Image.LANCZOS)
    d = ImageDraw.Draw(im)
    d.rectangle((0, 0, PW, 40), fill=(0, 0, 0))
    d.text((10, 6), label + extra, fill=(255, 235, 120), font=font)
    panels.append(im)

ph = panels[0].height
sheet = Image.new("RGB", (PW * 4 + 30, ph * 4 + 30), (12, 12, 12))
for i, p in enumerate(panels):
    sheet.paste(p, ((i % 4) * (PW + 10), (i // 4) * (ph + 10)))
# 9th cell: 100 % crop of the top-left corner (for chromatic aberration) from the CA panel
ca_out, _ = kfio.run(kfio.to_rgba(base), input_space=1, ca_amount=12, ca_blur=3.0, lens_edge=1)
c0 = kfio.run(kfio.to_rgba(base), input_space=1, ca_amount=12, lens_edge=1)[0][:360, :500, :3]; c1 = ca_out[:360, :500, :3]
cell = Image.new("RGB", (PW, ph), (12, 12, 12))
u8 = lambda a: Image.fromarray((np.clip(a, 0, 1) * 255 + .5).astype(np.uint8)).resize((380, 274), Image.NEAREST)
cell.paste(u8(c0), (10, 60)); cell.paste(u8(c1), (410, 60))
d = ImageDraw.Draw(cell); d.rectangle((0, 0, PW, 40), fill=(0, 0, 0))
d.text((10, 6), "Top-left corner x2: CA 12 | CA 12 + CA Blur 3", fill=(255, 235, 120), font=font)
sheet.paste(cell, (3 * (PW + 10), 3 * (ph + 10)))
os.makedirs(OUT, exist_ok=True)
sheet.save(f"{OUT}/{name}_all_effects.jpg", quality=90)
print("sheet", sheet.size)
