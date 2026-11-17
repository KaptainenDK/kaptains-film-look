#!/usr/bin/env python3
# Kaptains Film Look - Copyright (C) 2026 Henrik Jonas Kaptain
# SPDX-License-Identifier: GPL-3.0-or-later (with an additional permission for the Adobe SDKs, see LICENSE-EXCEPTION)
"""Run the Kaptains Film Look core on one RAF still and write comparison images.
usage: KF_IMAGES=<folder> test_raf.py NAME [preset-name]   (reads NAME.raf and NAME.JPG from KF_IMAGES)
"""
import json
import os
import sys

import numpy as np
import rawpy
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kfio  # noqa: E402

SRC = os.environ.get("KF_IMAGES", "images")          # folder with NAME.raf + NAME.JPG (camera JPG for comparison)
OUT = os.environ.get("KF_OUT", "results")            # output folder, created if missing
name = sys.argv[1]
preset_name = sys.argv[2] if len(sys.argv) > 2 else "reference_look"
presets = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "presets.json")))
params = presets[preset_name]

# --- neutral base from the RAF (camera white balance, linear, no tone curve) ---
with rawpy.imread(f"{SRC}/{name}.raf") as r:
    lin16 = r.postprocess(use_camera_wb=True, no_auto_bright=True, output_bps=16, gamma=(1, 1),
                          output_color=rawpy.ColorSpace.sRGB)
h, w, _ = lin16.shape
h2, w2 = h // 2, w // 2
lin = lin16[: h2 * 2, : w2 * 2].reshape(h2, 2, w2, 2, 3).astype(np.float32).mean(axis=(1, 3)) / 65535.0
del lin16

# --- match overall brightness to the camera JPG so the comparison is fair ---
jpg = np.asarray(Image.open(f"{SRC}/{name}.JPG").convert("RGB").resize((w2, h2), Image.BILINEAR)).astype(np.float32) / 255.0
def luma(x): return 0.2126 * x[..., 0] + 0.7152 * x[..., 1] + 0.0722 * x[..., 2]
exposure = np.median(luma(kfio.srgb_decode(jpg))) / np.median(luma(lin))
base = np.clip(kfio.srgb_encode(np.clip(lin * exposure, 0, 1)), 0, 1).astype(np.float32)   # display-referred sRGB
print(f"{name}: {w2}x{h2}, exposure match x{exposure:.2f}, clipped pixels {(lin.max(axis=2) >= 0.999).mean() * 100:.2f}%")

import time
t = time.time()
out, zoom = kfio.run(kfio.to_rgba(base), input_space=1, **params)
print(f"processed in {time.time() - t:.1f}s, auto-zoom {zoom:.3f}")
look = out[..., :3]

os.makedirs(OUT, exist_ok=True)
tag = f"{name}_{preset_name}"
kfio.save_png(base, f"{OUT}/{name}_base.png")
kfio.save_png(look, f"{OUT}/{tag}.png")
json.dump(params, open(f"{OUT}/{tag}_settings.json", "w"), indent=2)

def small(a, width=1500):
    im = Image.fromarray((np.clip(a, 0, 1) * 255 + .5).astype(np.uint8))
    return im.resize((width, int(a.shape[0] * width / a.shape[1])), Image.LANCZOS)

# side-by-side overview: neutral base | with Kaptains Film Look
a, b = small(base, 960), small(look, 960)
sheet = Image.new("RGB", (960 * 2 + 10, a.height), (10, 10, 10)); sheet.paste(a, (0, 0)); sheet.paste(b, (970, 0))
sheet.save(f"{OUT}/{tag}_sidebyside.jpg", quality=92)

# 100% crops (of the half-size frame): brightest area (glow), top-left corner (vignette / CA), flat area (grain)
lum = luma(kfio.srgb_decode(base))
from scipy.ndimage import uniform_filter
sm = uniform_filter(lum, 61)
by, bx = np.unravel_index(np.argmax(sm), sm.shape)
def crop(a, cy, cx, cw=900, ch=600):
    y0 = int(np.clip(cy - ch // 2, 0, a.shape[0] - ch)); x0 = int(np.clip(cx - cw // 2, 0, a.shape[1] - cw))
    return a[y0:y0 + ch, x0:x0 + cw]
def pair(cy, cx, nm):
    c1, c2 = crop(base, cy, cx), crop(look, cy, cx)
    im = Image.new("RGB", (c1.shape[1] * 2 + 10, c1.shape[0]), (10, 10, 10))
    u8 = lambda a: (np.clip(a, 0, 1) * 255 + .5).astype(np.uint8)   # values above white are clipped for the file
    im.paste(Image.fromarray(u8(c1)), (0, 0)); im.paste(Image.fromarray(u8(c2)), (c1.shape[1] + 10, 0))
    im.save(f"{OUT}/{tag}_crop_{nm}.jpg", quality=95)
fx, fy = (float(v) for v in os.environ.get("CROP_SUN", "0.6,0.12").split(","))
pair(int(fy * h2), int(fx * w2), "highlights")
pair(0, 0, "corner_topleft")
pair(h2 - 1, w2 - 1, "corner_bottomright")
print("brightest area at", bx, by)
