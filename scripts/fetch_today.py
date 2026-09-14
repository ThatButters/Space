"""Today's Earth: real cloud cover and live spacecraft orbits.

Clouds: NASA GIBS (Global Imagery Browse Services, no key) serves daily global true-colour imagery from
VIIRS on Suomi NPP. This fetches the most recent complete day at ~2 km/px (8192x4096), extracts the cloud
cover by comparing each pixel against the cloud-free Blue Marble base map, and writes
  assets/textures/earth_hires/clouds_today.png   (8192x4096 RGB: R cloud opacity, G data valid)
  assets/textures/earth_hires/clouds_today.txt   (the imagery date)
The engine uses it in place of the static cloud map when present. Day-side imagery only: the polar night
side shows no clouds, and the mosaic is stitched from orbit passes through the day.

Orbits: CelesTrak two-line elements for the ISS and Hubble, written to
  assets/models/tle.txt
so the stations are over the right part of Earth (accurate to a few hundred km for a few days).

Usage: python scripts/fetch_today.py [--date YYYY-MM-DD] [--level 5]
"""
import argparse
import datetime as dt
import io
import os
import sys
import urllib.request
from concurrent.futures import ThreadPoolExecutor

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "assets", "textures", "earth_hires")
GIBS = "https://gibs.earthdata.nasa.gov/wmts/epsg4326/best/VIIRS_SNPP_CorrectedReflectance_TrueColor/default/{date}/250m/{z}/{row}/{col}.jpg"
TLE_URL = "https://celestrak.org/NORAD/elements/gp.php?CATNR={id}&FORMAT=TLE"
TLE_IDS = {"ISS": 25544, "Hubble": 20580}


def fetch_tile(date, z, row, col):
    url = GIBS.format(date=date, z=z, row=row, col=col)
    for attempt in range(3):
        try:
            with urllib.request.urlopen(url, timeout=60) as r:
                return row, col, Image.open(io.BytesIO(r.read())).convert("RGB")
        except Exception as e:
            err = e
    print("tile failed", row, col, err)
    return row, col, None


def fetch_day(date, level):
    # GIBS EPSG:4326 "250m" matrix set: not power-of-two. Level 5 is 40x20 tiles of 512 px = 2 km/px.
    grid = {0: (2, 1), 1: (3, 2), 2: (5, 3), 3: (10, 5), 4: (20, 10), 5: (40, 20), 6: (80, 40)}
    cols, rows = grid[level]
    mosaic = Image.new("RGB", (cols * 512, rows * 512))
    jobs = [(r, c) for r in range(rows) for c in range(cols)]
    got = 0
    with ThreadPoolExecutor(16) as pool:
        for row, col, tile in pool.map(lambda rc: fetch_tile(date, level, rc[0], rc[1]), jobs):
            if tile is not None:
                mosaic.paste(tile, (col * 512, row * 512))
                got += 1
    print(f"{date}: {got}/{len(jobs)} tiles")
    return mosaic, got / len(jobs)


def clouds_from(mosaic):
    Image.MAX_IMAGE_PIXELS = None
    img = np.asarray(mosaic, dtype=np.float32) / 255.0
    h, w, _ = img.shape
    base_path = os.path.join(ROOT, "assets", "textures", "8k_earth_daymap.jpg")
    base = np.asarray(Image.open(base_path).convert("RGB").resize((w, h), Image.BILINEAR), dtype=np.float32) / 255.0
    lum = img.mean(axis=2)
    sat = (img.max(axis=2) - img.min(axis=2)) / np.maximum(img.max(axis=2), 1e-3)
    base_lum = base.mean(axis=2)
    base_sat = (base.max(axis=2) - base.min(axis=2)) / np.maximum(base.max(axis=2), 1e-3)
    # Cloud: bright, grey, and brighter than the cloud-free ground beneath. Snow and ice are bright and grey
    # in both images, so the brightening over the base removes most of them.
    brighter = np.clip((lum - base_lum * 0.85) / 0.45, 0.0, 1.0)
    grey = np.clip((0.35 - sat) / 0.3, 0.0, 1.0)
    bright = np.clip((lum - 0.28) / 0.4, 0.0, 1.0)
    cloud = brighter * grey * bright
    # Soften: clouds are not binary. A little blur (4 km) and a gentle curve keep thin cloud translucent.
    from PIL import ImageFilter
    cloud = np.asarray(Image.fromarray((np.clip(cloud, 0, 1) * 255).astype(np.uint8), "L").filter(ImageFilter.GaussianBlur(1.2)), dtype=np.float32) / 255.0
    cloud = np.power(cloud, 0.8)
    valid = lum >= 0.05  # no data where the day's passes had not yet reached (night side, missing tiles)
    cloud[~valid] = 0.0
    out = np.zeros((h, w, 3), dtype=np.uint8)
    out[..., 0] = (np.clip(cloud, 0, 1) * 255).astype(np.uint8)
    out[..., 1] = valid.astype(np.uint8) * 255
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--date", default=None)
    ap.add_argument("--level", type=int, default=5)
    args = ap.parse_args()
    os.makedirs(OUT, exist_ok=True)

    dates = [args.date] if args.date else [(dt.datetime.utcnow() - dt.timedelta(days=d)).strftime("%Y-%m-%d") for d in (1, 2, 3)]
    for date in dates:
        mosaic, coverage = fetch_day(date, args.level)
        if coverage > 0.97:
            if mosaic.width > 8192:
                mosaic = mosaic.resize((8192, 4096), Image.LANCZOS)
            clouds = clouds_from(mosaic)
            Image.fromarray(clouds, "RGB").save(os.path.join(OUT, "clouds_today.png"))
            with open(os.path.join(OUT, "clouds_today.txt"), "w") as f:
                f.write(date + "\n")
            print("clouds written for", date, f"({clouds[..., 0].mean() / 255 * 100:.0f}% mean cover)")
            break
    else:
        print("no complete day of imagery found", file=sys.stderr)

    lines = []
    for name, cat in TLE_IDS.items():
        try:
            with urllib.request.urlopen(TLE_URL.format(id=cat), timeout=30) as r:
                tle = r.read().decode().strip().splitlines()
            if len(tle) >= 3:
                lines += [name, tle[1].strip(), tle[2].strip()]
                print("TLE", name, tle[0].strip())
        except Exception as e:
            print("TLE failed", name, e)
    if lines:
        os.makedirs(os.path.join(ROOT, "assets", "models"), exist_ok=True)
        with open(os.path.join(ROOT, "assets", "models", "tle.txt"), "w") as f:
            f.write("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
