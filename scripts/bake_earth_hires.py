"""Fetch NASA Blue Marble Next Generation (86400x43200, 500 m/px, July 2004 with topography and
bathymetry) as its eight 21600x21600 tiles plus the 2016 Black Marble night map, and bake them into
mipmapped BC7 DDS files the engine can stream straight into VRAM.

Usage:  python scripts/bake_earth_hires.py [--texbake path/to/TexBake.exe] [--size 16384]

Output: assets/textures/earth_hires/bmng_{A1..D2}.dds (each 16384^2, ~358 MB) and
        assets/textures/earth_hires/blackmarble_16k.dds (16384x8192).
Total VRAM at 16384: about 3 GB. The engine falls back to the 8K map when the tiles are missing.
"""
import argparse
import os
import subprocess
import sys
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "assets", "textures", "earth_hires")
TILES = ["A1", "B1", "C1", "D1", "A2", "B2", "C2", "D2"]
BMNG = "https://eoimages.gsfc.nasa.gov/images/imagerecords/73000/73751/world.topo.bathy.200407.3x21600x21600.{}.jpg"
NIGHT = "https://eoimages.gsfc.nasa.gov/images/imagerecords/144000/144898/BlackMarble_2016_3km.jpg"


def download(url, path):
    """Stream url to path.part, then rename: a file under its final name is always complete, so an
    interrupted multi-hundred-MB download is fetched again instead of being baked half-empty."""
    tmp = path + ".part"
    with urllib.request.urlopen(url, timeout=120) as r, open(tmp, "wb") as f:
        while True:
            chunk = r.read(8 << 20)
            if not chunk:
                break
            f.write(chunk)
    os.replace(tmp, path)


def fetch(url, path):
    if os.path.exists(path):
        return
    print("downloading", url)
    download(url, path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--texbake", default=None)
    ap.add_argument("--size", type=int, default=16384)
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args()
    texbake = args.texbake
    if texbake is None:
        for cfg in ("release", "debug"):
            cand = os.path.join(ROOT, "build", cfg, "TexBake.exe")
            if os.path.exists(cand):
                texbake = cand
                break
    if texbake is None or not os.path.exists(texbake):
        sys.exit("TexBake.exe not found: build the project first (scripts\\build.cmd release)")
    os.makedirs(OUT, exist_ok=True)

    jobs = []
    for t in TILES:
        jpg = os.path.join(OUT, f"bmng_{t}.jpg")
        fetch(BMNG.format(t), jpg)
        jobs.append((jpg, os.path.join(OUT, f"bmng_{t}.dds"), args.size, args.size))
    night = os.path.join(OUT, "blackmarble_13500.jpg")
    fetch(NIGHT, night)
    jobs.append((night, os.path.join(OUT, "blackmarble_16k.dds"), args.size, args.size // 2))

    for src, dst, w, h in jobs:
        if os.path.exists(dst) and not args.force:
            print("exists", os.path.basename(dst))
            continue
        print("baking", os.path.basename(dst))
        subprocess.check_call([texbake, src, dst, "--width", str(w), "--height", str(h)])
    print("done")


if __name__ == "__main__":
    main()
