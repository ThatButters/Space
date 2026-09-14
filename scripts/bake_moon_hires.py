"""Fetch NASA's CGI Moon Kit (LRO: LROC WAC colour mosaic and LOLA elevation) and bake the textures
the engine uses for the Moon.

Sources (public domain, NASA Scientific Visualization Studio, https://svs.gsfc.nasa.gov/4720):
  lroc_color_poles_16k.tif  16384x8192 sRGB albedo, poles filled
  ldem_64.tif               23040x11520 float32 heights in km relative to 1737.4 km (64 px/deg, ~474 m/px)

Output (assets/textures/moon_hires):
  moon_color_16k.dds    BC7 sRGB albedo
  moon_normal_64.dds    BC5 tangent-space slopes: R = east component, G = north component of the unit normal
  moon_height_32.dds    R16 height (11520x5760), 0..1 mapped to [min, max] km
  moon_height.txt       "min_km max_km" for the height map

Usage: python scripts/bake_moon_hires.py [--texbake path/to/TexBake.exe] [--force]
Needs numpy and Pillow. Peak RAM about 6 GB.
"""
import argparse
import os
import subprocess
import sys
import urllib.request

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "assets", "textures", "moon_hires")
BASE = "https://svs.gsfc.nasa.gov/vis/a000000/a004700/a004720/"
MOON_RADIUS_M = 1737400.0


def fetch(name):
    path = os.path.join(OUT, name)
    if not (os.path.exists(path) and os.path.getsize(path) > 1_000_000):
        print("downloading", name)
        urllib.request.urlretrieve(BASE + name, path)
    return path


def find_texbake(arg):
    if arg:
        return arg
    for cfg in ("release", "debug"):
        cand = os.path.join(ROOT, "build", cfg, "TexBake.exe")
        if os.path.exists(cand):
            return cand
    sys.exit("TexBake.exe not found: build the project first (scripts\\build.cmd release)")


def bake(texbake, raw, dds, w, h, c, extra):
    cmd = [texbake, raw, dds, "--raw", str(w), str(h), str(c)] + extra
    print(" ".join(os.path.basename(x) if os.path.sep in x else x for x in cmd))
    subprocess.check_call(cmd)
    os.remove(raw)


def write_r16_dds(path, dem, lo, hi):
    import struct
    h, w = dem.shape
    level = dem[: h // 2 * 2, : w // 2 * 2].reshape(h // 2, 2, w // 2, 2).mean(axis=(1, 3), dtype=np.float64).astype(np.float32)
    levels = []
    while True:
        levels.append(np.clip((level - lo) / (hi - lo) * 65535.0 + 0.5, 0, 65535).astype("<u2"))
        lh, lw = level.shape
        if lh == 1 and lw == 1:
            break
        nh, nw = max(lh // 2, 1), max(lw // 2, 1)
        if lh >= 2 and lw >= 2:
            level = level[: nh * 2, : nw * 2].reshape(nh, 2, nw, 2).mean(axis=(1, 3))
        elif lh >= 2:
            level = level[: nh * 2].reshape(nh, 2, lw).mean(axis=1)
        else:
            level = level[:, : nw * 2].reshape(lh, nw, 2).mean(axis=2)
    H0, W0 = levels[0].shape
    header = struct.pack("<4sIIIIIII11I", b"DDS ", 124, 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000, H0, W0, 0, 0, len(levels), *([0] * 11))
    header += struct.pack("<II4sIIIII", 32, 0x4, b"DX10", 0, 0, 0, 0, 0)
    header += struct.pack("<IIIII", 0x1000 | 0x400000 | 0x8, 0, 0, 0, 0)
    header += struct.pack("<IIIII", 56, 3, 0, 1, 0)  # DXGI_FORMAT_R16_UNORM, TEXTURE2D
    with open(path, "wb") as f:
        f.write(header)
        for lv in levels:
            f.write(lv.tobytes())
    print(f"wrote {os.path.basename(path)} {W0}x{H0}, {len(levels)} mips")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--texbake", default=None)
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args()
    texbake = find_texbake(args.texbake)
    os.makedirs(OUT, exist_ok=True)
    Image.MAX_IMAGE_PIXELS = None

    color_dds = os.path.join(OUT, "moon_color_16k.dds")
    if args.force or not os.path.exists(color_dds):
        img = np.asarray(Image.open(fetch("lroc_color_poles_16k.tif")).convert("RGB"))
        h, w, _ = img.shape
        raw = os.path.join(OUT, "moon_color.raw")
        img.tofile(raw)
        del img
        bake(texbake, raw, color_dds, w, h, 3, [])

    normal_dds = os.path.join(OUT, "moon_normal_64.dds")
    height_dds = os.path.join(OUT, "moon_height_32.dds")
    if args.force or not (os.path.exists(normal_dds) and os.path.exists(height_dds)):
        dem = np.asarray(Image.open(fetch("ldem_64.tif")), dtype=np.float32)  # km, row 0 = north
        h, w = dem.shape
        lo, hi = float(dem.min()), float(dem.max())
        print(f"elevation {w}x{h}, {lo:.3f} .. {hi:.3f} km")
        with open(os.path.join(OUT, "moon_height.txt"), "w") as f:
            f.write(f"{lo:.6f} {hi:.6f}\n")

        # Heights for terrain self-shadowing: half resolution (~950 m/px) but 16-bit (0.3 m steps), written
        # straight to an uncompressed R16 DDS with a box-filtered mip chain (BC4's 8 bits would be ~80 m).
        write_r16_dds(height_dds, dem, lo, hi)
        # Slopes by central differences (wrapping in longitude), metres per metre.
        dy_m = np.pi * MOON_RADIUS_M / h
        raw = os.path.join(OUT, "moon_normal.raw")
        with open(raw, "wb") as f:
            for y0 in range(0, h, 1024):
                y1 = min(y0 + 1024, h)
                rows = np.arange(y0, y1)
                lat = (0.5 - (rows + 0.5) / h) * np.pi
                dx_m = 2.0 * np.pi * MOON_RADIUS_M * np.maximum(np.cos(lat), 0.01) / w
                up = dem[np.maximum(rows - 1, 0)]
                dn = dem[np.minimum(rows + 1, h - 1)]
                mid = dem[y0:y1]
                d_east = (np.roll(mid, -1, axis=1) - np.roll(mid, 1, axis=1)) * 1000.0 / (2.0 * dx_m[:, None])
                d_north = (up - dn) * 1000.0 / (2.0 * dy_m)
                inv = 1.0 / np.sqrt(1.0 + d_east * d_east + d_north * d_north)
                nx = -d_east * inv
                ny = -d_north * inv
                rg = np.empty((y1 - y0, w, 2), dtype=np.uint8)
                rg[..., 0] = np.clip(nx * 127.5 + 127.5 + 0.5, 0, 255).astype(np.uint8)
                rg[..., 1] = np.clip(ny * 127.5 + 127.5 + 0.5, 0, 255).astype(np.uint8)
                f.write(rg.tobytes())
        del dem
        bake(texbake, raw, normal_dds, w, h, 2, ["--bc5"])
    print("done")


if __name__ == "__main__":
    main()
