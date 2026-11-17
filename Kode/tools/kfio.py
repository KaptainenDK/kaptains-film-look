# Kaptains Film Look - Copyright (C) 2026 Henrik Jonas Kaptain
# SPDX-License-Identifier: GPL-3.0-or-later (with an additional permission for the Adobe SDKs, see LICENSE-EXCEPTION)
"""Helpers for the Kaptains Film Look core test harness (not part of the plugin)."""
import os
import struct
import subprocess
import tempfile

import numpy as np

MAGIC = 0x3149464B  # 'KFI1'
CLI = os.environ.get("KF_CLI", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "core", "kf_cli"))


def write_kfi(path, rgba):
    h, w, c = rgba.shape
    assert c == 4
    with open(path, "wb") as f:
        f.write(struct.pack("<4i", MAGIC, w, h, 4))
        f.write(np.ascontiguousarray(rgba, dtype="<f4").tobytes())


def read_kfi(path):
    with open(path, "rb") as f:
        magic, w, h, c = struct.unpack("<4i", f.read(16))
        assert magic == MAGIC and c == 4
        return np.frombuffer(f.read(), dtype="<f4").reshape(h, w, 4).copy()


def to_rgba(rgb):
    h, w, _ = rgb.shape
    out = np.ones((h, w, 4), dtype=np.float32)
    out[..., :3] = rgb
    return out


def run(rgba, **params):
    """Run the C++ core. Parameters are passed as key=value (bool -> 0/1, tuple -> a,b,c)."""
    with tempfile.TemporaryDirectory() as d:
        a, b = os.path.join(d, "in.kfi"), os.path.join(d, "out.kfi")
        write_kfi(a, rgba)
        args = [CLI, a, b]
        for k, v in params.items():
            if isinstance(v, bool):
                v = int(v)
            if isinstance(v, (tuple, list)):
                v = ",".join(str(x) for x in v)
            args.append(f"{k}={v}")
        r = subprocess.run(args, capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError(r.stderr)
        zoom = float(r.stdout.strip().split("=")[1]) if "auto_zoom" in r.stdout else None
        return read_kfi(b), zoom


def srgb_encode(x):
    x = np.clip(x, 0, None)
    return np.where(x <= 0.0031308, x * 12.92, 1.055 * np.power(x, 1 / 2.4) - 0.055)


def srgb_decode(x):
    x = np.clip(x, 0, None)
    return np.where(x <= 0.04045, x / 12.92, np.power((x + 0.055) / 1.055, 2.4))


def save_png(rgb_encoded, path):
    from PIL import Image
    Image.fromarray((np.clip(rgb_encoded, 0, 1) * 255 + 0.5).astype(np.uint8)).save(path)


def resize(rgb, w, h):
    from PIL import Image
    chans = [np.asarray(Image.fromarray(rgb[..., i].astype(np.float32), mode="F").resize((w, h), Image.LANCZOS)) for i in range(rgb.shape[2])]
    return np.stack(chans, axis=2)
