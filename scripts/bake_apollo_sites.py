"""Bake LROC NAC terrain around the Apollo landing sites into local terrain patches.

Input (per site, in assets/textures/apollo_sites/<site>/, from the LROC PDS "NAC_DTM_APOLLO*" products):
  NAC_DTM_APOLLO11.TIF / .LBL           2 m/px float32 DTM, equirectangular, heights in metres on the 1737.4 km sphere
  NAC_DTM_APOLLO11_M*_*CM*.TIF / .LBL   orthorectified NAC image(s), same projection (optional but preferred)

Output (same folder), read by the engine when all four exist:
  patch_albedo.dds   BC7 sRGB: the orthophoto with the image's own sun shading divided out (using the DTM),
                     tinted and scaled to match the global LROC colour map at the site; the lunar module in the
                     photo (and its shadow) painted out so the 3D model is not doubled
  patch_normal.dds   BC5 east/north normal components from the DTM
  patch_height.dds   R16 heights, mapped to [h_min, h_max] metres
  patch.txt          "lon_min_deg lat_min_deg lon_span_deg lat_span_deg h_min_m h_max_m"
The patch is cropped to a window around the lander (default 4 x 4 km) on a regular latitude/longitude grid.

Usage: python scripts/bake_apollo_sites.py [--site apollo11] [--size-km 4] [--texbake path]
"""
import argparse
import glob
import math
import os
import re
import struct
import subprocess
import sys

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SITES_DIR = os.path.join(ROOT, "assets", "textures", "apollo_sites")
MOON_R = 1737400.0

# Lunar module descent stage positions (LROC, planetocentric east longitude).
LM_SITES = {
    "apollo11": (0.67416, 23.47314),
    "apollo12": (-3.01239, -23.42157),
    "apollo14": (-3.64530, -17.47136),
    "apollo15": (26.13222, 3.63386),
    "apollo16": (-8.97301, 15.50019),
    "apollo17": (20.19080, 30.77168),
}


def read_label(path):
    text = open(path, encoding="latin-1").read()
    def num(key):
        m = re.search(r"^\s*" + key + r"\s*=\s*([-+0-9.eE]+)", text, re.M)
        return float(m.group(1)) if m else None
    return {k: num(k) for k in ("MAXIMUM_LATITUDE", "MINIMUM_LATITUDE", "EASTERNMOST_LONGITUDE", "WESTERNMOST_LONGITUDE",
                                "MAP_SCALE", "INCIDENCE_ANGLE", "SUB_SOLAR_AZIMUTH", "NORTH_AZIMUTH", "SOLAR_AZIMUTH",
                                "SUB_SOLAR_LATITUDE", "SUB_SOLAR_LONGITUDE")}


# Sun direction when each orthophoto was taken: (incidence deg, map azimuth deg clockwise from north). LROC's
# "sub solar azimuth" is in the raw image frame, so the map azimuth is read off the lander's shadow instead.
ORTHO_SUN = {
    "M150361817": (62.5, 270.0),  # Apollo 11: LM shadow points due east -> Sun in the west
    "M150368601": (63.4, 270.0),
}


def read_pds_img(path):
    """PDS3 .IMG with an attached label: returns (array, label dict). Raw little-endian int16/uint16/float32."""
    with open(path, "rb") as f:
        head = f.read(65536).decode("latin-1", errors="replace")
    def val(key):
        m = re.search(r"^\s*" + key + r"\s*=\s*\"?([^\r\n\"]+)", head, re.M)
        return m.group(1).strip() if m else None
    record = int(val("RECORD_BYTES"))
    pointer = val(r"\^IMAGE")
    lines, samples = int(val("LINES")), int(val("LINE_SAMPLES"))
    bits = int(val("SAMPLE_BITS"))
    stype = val("SAMPLE_TYPE") or ""
    offset = (int(re.findall(r"\d+", pointer)[0]) - 1) * record if pointer and "BYTES" not in pointer else int(re.findall(r"\d+", pointer)[0])
    dtype = {16: "<i2" if "UNSIGNED" not in stype else "<u2", 32: "<f4"}[bits]
    arr = np.memmap(path, dtype=dtype, mode="r", offset=offset, shape=(lines, samples))
    lab = {k: (float(val(k).split()[0]) if val(k) else None) for k in
           ("MAXIMUM_LATITUDE", "MINIMUM_LATITUDE", "EASTERNMOST_LONGITUDE", "WESTERNMOST_LONGITUDE", "MAP_SCALE")}
    return arr, lab


def lon180(x):
    return (x + 180.0) % 360.0 - 180.0


def crop_window(lab, shape, lat0, lon0, size_km):
    h, w = shape
    lat_max, lat_min = lab["MAXIMUM_LATITUDE"], lab["MINIMUM_LATITUDE"]
    lon_w, lon_e = lon180(lab["WESTERNMOST_LONGITUDE"]), lon180(lab["EASTERNMOST_LONGITUDE"])
    half_lat = math.degrees(size_km * 500.0 / MOON_R)
    half_lon = half_lat / math.cos(math.radians(lat0))
    wlat0, wlat1 = max(lat0 - half_lat, lat_min), min(lat0 + half_lat, lat_max)
    wlon0, wlon1 = max(lon0 - half_lon, lon_w), min(lon0 + half_lon, lon_e)
    r0 = int(round((lat_max - wlat1) / (lat_max - lat_min) * h))
    r1 = int(round((lat_max - wlat0) / (lat_max - lat_min) * h))
    c0 = int(round((wlon0 - lon_w) / (lon_e - lon_w) * w))
    c1 = int(round((wlon1 - lon_w) / (lon_e - lon_w) * w))
    # Snap the geographic bounds to the integer pixel edges actually used.
    lat_hi = lat_max - r0 / h * (lat_max - lat_min)
    lat_lo = lat_max - r1 / h * (lat_max - lat_min)
    lon_lo = lon_w + c0 / w * (lon_e - lon_w)
    lon_hi = lon_w + c1 / w * (lon_e - lon_w)
    return (r0, r1, c0, c1), (lon_lo, lat_lo, lon_hi - lon_lo, lat_hi - lat_lo)


def fill_nodata(a, valid):
    """Nearest-row/column fill of missing DTM samples (edges of the strip)."""
    if valid.all():
        return a
    out = a.copy()
    rows = np.where(valid.any(axis=1))[0]
    for r in rows:  # rows with data first, so an empty row copies a filled one
        row_valid = valid[r]
        if row_valid.all():
            continue
        idx = np.where(row_valid)[0]
        out[r] = np.interp(np.arange(a.shape[1]), idx, a[r, idx])
    for r in range(a.shape[0]):
        if not valid[r].any():
            out[r] = out[rows[np.argmin(np.abs(rows - r))]]
    return out


def write_r16_dds(path, level0):
    levels = []
    level = level0.astype(np.float32)
    while True:
        levels.append(np.clip(level * 65535.0 + 0.5, 0, 65535).astype("<u2"))
        lh, lw = level.shape
        if lh == 1 and lw == 1:
            break
        nh, nw = max(lh // 2, 1), max(lw // 2, 1)
        level = level[: nh * (2 if lh > 1 else 1), : nw * (2 if lw > 1 else 1)]
        level = level.reshape(nh, 2 if lh > 1 else 1, nw, 2 if lw > 1 else 1).mean(axis=(1, 3))
    H0, W0 = levels[0].shape
    header = struct.pack("<4sIIIIIII11I", b"DDS ", 124, 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000, H0, W0, 0, 0, len(levels), *([0] * 11))
    header += struct.pack("<II4sIIIII", 32, 0x4, b"DX10", 0, 0, 0, 0, 0)
    header += struct.pack("<IIIII", 0x1000 | 0x400000 | 0x8, 0, 0, 0, 0)
    header += struct.pack("<IIIII", 56, 3, 0, 1, 0)
    with open(path, "wb") as f:
        f.write(header)
        for lv in levels:
            f.write(lv.tobytes())


def bake_raw(texbake, arr, out, channels, extra):
    raw = out + ".raw"
    np.ascontiguousarray(arr).tofile(raw)
    h, w = arr.shape[:2]
    subprocess.check_call([texbake, raw, out, "--raw", str(w), str(h), str(channels)] + extra)
    os.remove(raw)


def global_colour_at(lat, lon):
    """Mean linear colour of the global LROC map in a ~2 km window (for tinting the grey orthophoto)."""
    path = os.path.join(ROOT, "assets", "textures", "moon_hires", "lroc_color_poles_16k.tif")
    if not os.path.exists(path):
        return np.array([0.18, 0.17, 0.16])
    Image.MAX_IMAGE_PIXELS = None
    im = Image.open(path)
    W, H = im.size
    x = int((lon + 180.0) / 360.0 * W)
    y = int((90.0 - lat) / 180.0 * H)
    crop = np.asarray(im.crop((x - 4, y - 4, x + 4, y + 4)).convert("RGB"), dtype=np.float32) / 255.0
    lin = np.where(crop <= 0.04045, crop / 12.92, ((crop + 0.055) / 1.055) ** 2.4)
    return lin.reshape(-1, 3).mean(axis=0)


def bake_site(site, size_km, texbake):
    folder = os.path.join(SITES_DIR, site)
    tag = site.upper()
    dtm_tif = os.path.join(folder, f"NAC_DTM_{tag}.TIF")
    dtm_lbl = os.path.join(folder, f"NAC_DTM_{tag}.LBL")
    if not (os.path.exists(dtm_tif) and os.path.exists(dtm_lbl)):
        print(f"{site}: DTM not downloaded, skipping")
        return False
    lat0, lon0 = LM_SITES[site]
    Image.MAX_IMAGE_PIXELS = None
    lab = read_label(dtm_lbl)
    dem_full = np.asarray(Image.open(dtm_tif), dtype=np.float32)
    (r0, r1, c0, c1), bounds = crop_window(lab, dem_full.shape, lat0, lon0, size_km)
    dem = dem_full[r0:r1, c0:c1]
    valid = dem > -1e30
    dem = fill_nodata(dem, valid)
    del dem_full
    h, w = dem.shape
    step = lab["MAP_SCALE"] or 2.0
    lo, hi = float(dem.min()), float(dem.max())
    print(f"{site}: DTM window {w}x{h} at {step:.2f} m, heights {lo:.1f} .. {hi:.1f} m")

    # Normals (east/north components), central differences.
    gy, gx = np.gradient(dem, step)
    d_east, d_north = gx, -gy  # row 0 is north
    inv = 1.0 / np.sqrt(1.0 + d_east ** 2 + d_north ** 2)
    nx, ny = -d_east * inv, -d_north * inv
    rg = np.empty((h, w, 2), dtype=np.uint8)
    rg[..., 0] = np.clip(nx * 127.5 + 127.5 + 0.5, 0, 255)
    rg[..., 1] = np.clip(ny * 127.5 + 127.5 + 0.5, 0, 255)
    bake_raw(texbake, rg, os.path.join(folder, "patch_normal.dds"), 2, ["--bc5"])
    write_r16_dds(os.path.join(folder, "patch_height.dds"), (dem - lo) / max(hi - lo, 1e-3))

    # Albedo from the orthophoto (prefer one with a moderate incidence angle).
    orthos = sorted(glob.glob(os.path.join(folder, f"NAC_DTM_{tag}_M*.TIF")) + glob.glob(os.path.join(folder, f"NAC_DTM_{tag}_M*CM.IMG")))
    tint = global_colour_at(lat0, lon0)
    albedo = None
    for tif in orthos:
        if tif.upper().endswith(".IMG"):
            arr, olab = read_pds_img(tif)
            if not olab.get("MAXIMUM_LATITUDE"):
                olab = lab  # same map grid as the DTM (4x finer)
        else:
            lbl = tif[:-4] + ".LBL"
            olab = read_label(lbl) if os.path.exists(lbl) else lab
            arr = np.asarray(Image.open(tif))
        (q0, q1, p0, p1), _ = crop_window(olab, arr.shape[:2], lat0, lon0, size_km)
        img = np.array(arr[q0:q1, p0:p1], dtype=np.float32)
        image_id = re.search(r"_(M\d+)_", os.path.basename(tif))
        sun_inc, sun_az = ORTHO_SUN.get(image_id.group(1) if image_id else "", (olab.get("INCIDENCE_ANGLE") or 60.0, 90.0))
        if img.ndim == 3:
            img = img[..., 0]
        good = img > 0
        if good.mean() < 0.5:
            continue
        scale_px = img.shape[0] / h  # ortho pixels per DTM pixel
        # The image's own shading: Lambert from the DTM normals toward the image's Sun, upsampled.
        inc = math.radians(sun_inc)
        az = math.radians(sun_az)
        sun = np.array([math.sin(inc) * math.sin(az), math.sin(inc) * math.cos(az), math.cos(inc)])  # east, north, up
        shade = np.clip(nx * sun[0] + ny * sun[1] + inv * sun[2], 0.05, 1.0) / max(sun[2], 0.05)
        shade_img = np.asarray(Image.fromarray(shade.astype(np.float32)).resize((img.shape[1], img.shape[0]), Image.BILINEAR))
        flat = img / np.maximum(shade_img, 0.12)
        flat[~good] = np.median(flat[good])
        # Paint out the lander and its shadow in the photo (the 3D model and our own shadow replace them).
        ys, xs = np.mgrid[0:img.shape[0], 0:img.shape[1]]
        lm_r = (bounds[1] + bounds[3] - lat0) / bounds[3] * img.shape[0]
        lm_c = (lon0 - bounds[0]) / bounds[2] * img.shape[1]
        mpp = step / scale_px
        shadow_len = min(9.0 / max(math.tan(math.pi / 2 - inc), 0.1), 60.0) / mpp
        ax, ay = -math.sin(az), math.cos(az)  # anti-sun direction in image (column, row) terms
        rel_c, rel_r = xs - lm_c, ys - lm_r
        along = rel_c * ax + rel_r * ay
        perp = np.abs(rel_c * ay - rel_r * ax)
        mask = (np.hypot(rel_c, rel_r) < 9.0 / mpp) | ((along > 0) & (along < shadow_len) & (perp < 5.0 / mpp))
        if mask.any():
            ring = (np.hypot(rel_c, rel_r) < 30.0 / mpp) & ~mask
            flat[mask] = np.median(flat[ring]) if ring.any() else np.median(flat)
        # Normalise to the global map's brightness and colour at the site.
        flat = flat / np.median(flat) * float(tint.mean())
        albedo = np.clip(flat[..., None] * (tint / max(tint.mean(), 1e-4))[None, None, :], 0.0, 1.0)
        print(f"{site}: albedo from {os.path.basename(tif)} ({img.shape[1]}x{img.shape[0]}, incidence {math.degrees(inc):.0f} deg)")
        break
    if albedo is None:
        print(f"{site}: no usable orthophoto, using the DTM shading-free global tint")
        albedo = np.broadcast_to(tint, (h, w, 3)).copy()
    # Cap the texture size (Vulkan limit and memory): at most 8192 on the long side.
    ah, aw = albedo.shape[:2]
    s = min(1.0, 8192.0 / max(ah, aw))
    srgb = np.where(albedo <= 0.0031308, albedo * 12.92, 1.055 * np.power(albedo, 1 / 2.4) - 0.055)
    rgb8 = (np.clip(srgb, 0, 1) * 255 + 0.5).astype(np.uint8)
    if s < 1.0:
        rgb8 = np.asarray(Image.fromarray(rgb8).resize((int(aw * s), int(ah * s)), Image.LANCZOS))
    bake_raw(texbake, rgb8, os.path.join(folder, "patch_albedo.dds"), 3, [])

    with open(os.path.join(folder, "patch.txt"), "w") as f:
        f.write(f"{bounds[0]:.9f} {bounds[1]:.9f} {bounds[2]:.9f} {bounds[3]:.9f} {lo:.3f} {hi:.3f}\n")
    print(f"{site}: patch written, bounds lon {bounds[0]:.5f}+{bounds[2]:.5f} lat {bounds[1]:.5f}+{bounds[3]:.5f}")
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--site", default=None)
    ap.add_argument("--size-km", type=float, default=4.0)
    ap.add_argument("--texbake", default=None)
    args = ap.parse_args()
    texbake = args.texbake or next((p for p in (os.path.join(ROOT, "build", c, "TexBake.exe") for c in ("release", "debug"))
                                    if os.path.exists(p)), None)
    if not texbake:
        sys.exit("TexBake.exe not found: build the project first")
    sites = [args.site] if args.site else sorted(LM_SITES)
    for site in sites:
        bake_site(site, args.size_km, texbake)


if __name__ == "__main__":
    main()
