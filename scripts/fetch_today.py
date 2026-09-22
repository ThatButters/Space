"""Today's sky: real cloud cover, live spacecraft orbits and today's Sun.

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

Sun: the newest SDO/HMI flattened continuum image (limb darkening removed, so what is left is today's
sunspots and faculae) from the SDO browse archive, cropped so the disc fills the square exactly and stored
as brightness relative to the quiet photosphere (byte = ratio * 200):
  assets/textures/sun_today.png   (1024x1024 grey, solar north up, as seen from Earth)
  assets/textures/sun_today.txt   (observation time, UTC, ISO 8601)
The engine reprojects the disc onto the Sun's rotating globe for that time, so the spots turn with it.

Every output is written to a temporary file and renamed into place: the app hot-loads these files, and a
fetch cut short (logoff, a closed laptop) must not leave half a PNG behind.

Usage: python scripts/fetch_today.py [--date YYYY-MM-DD] [--level 5] [--only clouds,tle,sun]
"""
import argparse
import datetime as dt
import io
import os
import re
import sys
import time
import urllib.error
import urllib.request
from concurrent.futures import ThreadPoolExecutor

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "assets", "textures", "earth_hires")
GIBS = "https://gibs.earthdata.nasa.gov/wmts/epsg4326/best/VIIRS_SNPP_CorrectedReflectance_TrueColor/default/{date}/250m/{z}/{row}/{col}.jpg"
TLE_URL = "https://celestrak.org/NORAD/elements/gp.php?CATNR={id}&FORMAT=TLE"
TLE_IDS = {"ISS": 25544, "Hubble": 20580}
SDO_BROWSE = "https://sdo.gsfc.nasa.gov/assets/img/browse/{y:04d}/{m:02d}/{d:02d}/"


def utcnow():
    return dt.datetime.now(dt.timezone.utc)


def save_atomic(path, write):
    """write(tmp_path) produces the file; it replaces `path` only once complete."""
    tmp = path + ".part"
    write(tmp)
    os.replace(tmp, path)


def write_text(text):
    def write(path):
        with open(path, "w") as f:
            f.write(text)
    return write


def fetch_bytes(url, timeout=60, attempts=3):
    """GET with retries and backoff. A 404 (no imagery for that tile or day yet) is final, not retried."""
    err = None
    for attempt in range(attempts):
        try:
            with urllib.request.urlopen(url, timeout=timeout) as r:
                return r.read()
        except urllib.error.HTTPError as e:
            if e.code == 404:
                raise
            err = e
        except Exception as e:
            err = e
        time.sleep(1.5 * (attempt + 1))
    raise err


def fetch_tile(date, z, row, col):
    url = GIBS.format(date=date, z=z, row=row, col=col)
    try:
        return row, col, Image.open(io.BytesIO(fetch_bytes(url))).convert("RGB")
    except Exception as e:
        print("tile failed", row, col, e)
        return row, col, None


def fetch_day(date, level):
    # GIBS EPSG:4326 "250m" matrix set: not power-of-two. Level 5 is 40x20 tiles of 512 px = 2 km/px.
    grid = {0: (2, 1), 1: (3, 2), 2: (5, 3), 3: (10, 5), 4: (20, 10), 5: (40, 20), 6: (80, 40)}
    cols, rows = grid[level]
    # One tile first: a day that is not published yet answers 404 everywhere, no need to ask 800 times.
    if fetch_tile(date, level, rows // 2, cols // 2)[2] is None:
        return None, 0.0
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


def fetch_clouds(args):
    dates = [args.date] if args.date else [(utcnow() - dt.timedelta(days=d)).strftime("%Y-%m-%d") for d in (1, 2, 3)]
    for date in dates:
        mosaic, coverage = fetch_day(date, args.level)
        if coverage > 0.97:
            if mosaic.width > 8192:
                mosaic = mosaic.resize((8192, 4096), Image.LANCZOS)
            clouds = clouds_from(mosaic)
            save_atomic(os.path.join(OUT, "clouds_today.png"), lambda t: Image.fromarray(clouds, "RGB").save(t, format="PNG"))
            # The date last: the app takes it as the mark of a finished fetch.
            save_atomic(os.path.join(OUT, "clouds_today.txt"), write_text(date + "\n"))
            print("clouds written for", date, f"({clouds[..., 0].mean() / 255 * 100:.0f}% mean cover)")
            return
        if mosaic is None:
            print(date, "not published yet")
    print("no complete day of imagery found", file=sys.stderr)


def fetch_tle():
    lines = []
    for name, cat in TLE_IDS.items():
        try:
            tle = fetch_bytes(TLE_URL.format(id=cat), timeout=30).decode().strip().splitlines()
            # CelesTrak answers errors with a line of text: accept only a real element set.
            if len(tle) >= 3 and tle[1].startswith("1 ") and tle[2].startswith("2 "):
                lines += [name, tle[1].strip(), tle[2].strip()]
                print("TLE", name, tle[0].strip())
            else:
                print("TLE rejected", name, tle[:1])
        except Exception as e:
            print("TLE failed", name, e)
    if lines:
        os.makedirs(os.path.join(ROOT, "assets", "models"), exist_ok=True)
        save_atomic(os.path.join(ROOT, "assets", "models", "tle.txt"), write_text("\n".join(lines) + "\n"))


def fetch_sun():
    # Newest flattened continuum image in today's or yesterday's archive folder (one every 15 min, ~1 h behind).
    for back in (0, 1):
        day = utcnow() - dt.timedelta(days=back)
        base = SDO_BROWSE.format(y=day.year, m=day.month, d=day.day)
        try:
            listing = fetch_bytes(base, timeout=30).decode(errors="replace")
        except Exception as e:
            print("SDO listing failed", base, e)
            continue
        names = sorted(set(re.findall(r"(\d{8}_\d{6})_2048_HMIIF\.jpg", listing)))
        if names:
            stamp = names[-1]
            break
    else:
        print("no SDO/HMI image found", file=sys.stderr)
        return
    img = np.asarray(Image.open(io.BytesIO(fetch_bytes(base + stamp + "_2048_HMIIF.jpg"))).convert("RGB"), dtype=np.float32) / 255.0
    lum = img @ np.array([0.3, 0.5, 0.2], dtype=np.float32)
    # The disc: rows and columns with a long run of lit pixels (the caption is short and outside it).
    lit = lum > 0.15
    rows = np.where(lit.sum(axis=1) > 40)[0]
    cols = np.where(lit.sum(axis=0) > 40)[0]
    cy, cx = (rows[0] + rows[-1] + 1) / 2.0, (cols[0] + cols[-1] + 1) / 2.0
    r = ((rows[-1] + 1 - rows[0]) + (cols[-1] + 1 - cols[0])) / 4.0
    # Resample so the disc fills 1024x1024 exactly.
    n = 1024
    g = (np.arange(n) + 0.5) / n * 2.0 - 1.0
    X, Y = np.meshgrid(g, g)
    rr = X * X + Y * Y
    from scipy.ndimage import map_coordinates
    disc = map_coordinates(lum, [cy + Y * r - 0.5, cx + X * r - 0.5], order=1, mode="nearest")
    quiet = np.median(disc[rr < 0.8])
    ratio = np.where(rr < 1.0, disc / max(quiet, 1e-3), 1.0)
    out = (np.clip(ratio * 200.0, 0, 255) + 0.5).astype(np.uint8)
    when = dt.datetime.strptime(stamp, "%Y%m%d_%H%M%S").strftime("%Y-%m-%dT%H:%M:%SZ")
    dst = os.path.join(ROOT, "assets", "textures")
    save_atomic(os.path.join(dst, "sun_today.png"), lambda t: Image.fromarray(out, "L").save(t, format="PNG"))
    save_atomic(os.path.join(dst, "sun_today.txt"), write_text(when + "\n"))
    spots = (ratio[rr < 1.0] < 0.8).mean() * 100
    print(f"Sun written for {when} (disc radius {r:.0f} px, {spots:.3f}% of the disc in spots)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--date", default=None)
    ap.add_argument("--level", type=int, default=5)
    ap.add_argument("--only", default="clouds,tle,sun", help="comma-separated: clouds, tle, sun")
    args = ap.parse_args()
    parts = set(args.only.split(","))
    os.makedirs(OUT, exist_ok=True)
    # Quick ones first, so a slow cloud mosaic never holds up the orbits or the Sun.
    for name, run in (("tle", fetch_tle), ("sun", fetch_sun), ("clouds", lambda: fetch_clouds(args))):
        if name in parts:
            try:
                run()
            except Exception as e:
                print(name, "failed:", e, file=sys.stderr)


if __name__ == "__main__":
    main()
