"""Bake LROC NAC terrain around the Apollo landing sites into local terrain patches.

Input (per site, in assets/textures/apollo_sites/<site>/, from the LROC PDS "NAC_DTM_APOLLO*" products):
  NAC_DTM_APOLLO11.TIF / .LBL           2 m/px float32 DTM, equirectangular, heights in metres on the 1737.4 km sphere
  NAC_DTM_APOLLO11_M*_*CM*.TIF / .LBL   orthorectified NAC image(s), same projection (optional but preferred)

Output (same folder), read by the engine when all four exist:
  patch_albedo.dds   BC7 sRGB: the orthophoto with the image's own sun shading divided out (using the DTM),
                     tinted and scaled to match the global LROC colour map at the site; the lunar module in the
                     photo (and its shadow) painted out so the 3D model is not doubled
  patch_normal.dds   BC7: r,g east/north normal components, b the fine relief height (+-FINE_M m around 0.5) --
                     from the DTM plus metre-scale relief recovered from the orthophoto's own shading
                     (photoclinometry: brightness residuals along the Sun azimuth integrate to height)
  patch_height.dds   R16 heights (DTM + fine relief), mapped to [h_min, h_max] metres
  patch.txt          "lon_min_deg lat_min_deg lon_span_deg lat_span_deg h_min_m h_max_m"
The patch is cropped to a window around the lander (default 4 x 4 km) on a regular latitude/longitude grid.

Also bakes HiRISE sites on Mars (OTHER_SITES: "jezero" for Perseverance) from a DTEEC .IMG + ORTHO .JP2.

Usage: python scripts/bake_apollo_sites.py [--site apollo11|jezero] [--size-km 4] [--texbake path]
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

# Sites on other bodies (HiRISE for Mars): folder under assets/textures, the projection sphere radius the
# products were mapped on, lander lat/lon (planetocentric), DTM attached-label .IMG, orthophoto .JP2 + .LBL,
# and the orthophoto's Sun (incidence deg, map azimuth deg clockwise from north). No paint-out: a rover is
# a few pixels at 25 cm and the model sits on top of it.
OTHER_SITES = {
    "jezero": {
        "folder": os.path.join("mars_sites", "jezero"),
        "radius": 3394839.8133163,
        "latlon": (18.4447, 77.4508),  # Octavia E. Butler Landing (Perseverance)
        "dtm": "DTEEC_045994_1985_046060_1985_U01.IMG",
        "ortho": "ESP_045994_1985_RED_A_01_ORTHO.JP2",
        "sun": (47.583, 267.0),  # ESP_045994_1985: 15:14 local, Sun in the west
        "tint_map": "8k_mars.jpg",
    },
}

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

    class Rows:
        """Window reader for multi-gigabyte mosaics (a whole-file memmap fails on Windows): only the
        requested rows are read, straight from the file."""
        shape = (lines, samples)
        ndim = 2

        def __getitem__(self, key):
            rs, cs = key
            r0, r1 = rs.start or 0, rs.stop if rs.stop is not None else lines
            item = np.dtype(dtype).itemsize
            with open(path, "rb") as f:
                f.seek(offset + r0 * samples * item)
                block = np.fromfile(f, dtype=dtype, count=(r1 - r0) * samples).reshape(r1 - r0, samples)
            return block[:, cs]

    arr = Rows()
    lab = {k: (float(val(k).split()[0]) if val(k) else None) for k in
           ("MAXIMUM_LATITUDE", "MINIMUM_LATITUDE", "EASTERNMOST_LONGITUDE", "WESTERNMOST_LONGITUDE", "MAP_SCALE")}
    return arr, lab


def lon180(x):
    return (x + 180.0) % 360.0 - 180.0


def crop_window(lab, shape, lat0, lon0, size_km, radius=MOON_R):
    h, w = shape
    lat_max, lat_min = lab["MAXIMUM_LATITUDE"], lab["MINIMUM_LATITUDE"]
    lon_w, lon_e = lon180(lab["WESTERNMOST_LONGITUDE"]), lon180(lab["EASTERNMOST_LONGITUDE"])
    half_lat = math.degrees(size_km * 500.0 / radius)
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


FINE_M = 2.0  # the fine relief channel spans -FINE_M .. +FINE_M metres


def box_blur(a, r):
    """Separable box blur of radius r (two passes ~ Gaussian), edge-replicated, via cumulative sums."""
    for _ in range(2):
        for axis in (0, 1):
            pad = np.pad(a, [(r + 1, r) if ax == axis else (0, 0) for ax in (0, 1)], mode="edge")
            c = np.cumsum(pad, axis=axis, dtype=np.float64)
            n = a.shape[axis]
            if axis == 0:
                a = ((c[2 * r + 1:2 * r + 1 + n, :] - c[:n, :]) / (2 * r + 1)).astype(np.float32)
            else:
                a = ((c[:, 2 * r + 1:2 * r + 1 + n] - c[:, :n]) / (2 * r + 1)).astype(np.float32)
    return a


def fine_relief(ratio, mask, inc, az, mpp, gain=1.0):
    """Metre-scale height from an orthophoto: the brightness left after dividing out the DTM's shading is
    the tilt of the ground toward the Sun (Lambert: dI/I ~ tan(incidence) * tilt). Integrating that tilt
    along the Sun's azimuth gives height, high-passed so long shading gradients do not drift away.
    ratio: image / DTM shading; mask: pixels to ignore (the lander); returns metres, zero mean locally."""
    from scipy import ndimage, signal
    local = box_blur(ratio, max(int(12.0 / mpp), 2))            # local mean brightness (~24 m)
    res = ratio / np.maximum(local, 1e-3) - 1.0
    res[mask] = 0.0
    # Camera striping (per-column and per-row offsets) would integrate into ridges: take it out.
    res = res - np.median(res, axis=0, keepdims=True)
    res = res - np.median(res, axis=1, keepdims=True)
    res = np.clip(box_blur(res, 1), -0.35, 0.35)                # sensor noise is one pixel; rocks are several
    tilt = (res / max(math.tan(inc), 0.3) * gain).astype(np.float32)  # radians, positive = tilted toward the Sun
    # Rotate so the Sun's map azimuth (clockwise from north) points along +columns, integrate, rotate back.
    ang = 90.0 - az
    rot = ndimage.rotate(tilt, ang, reshape=True, order=1, mode="constant", cval=0.0).astype(np.float32)
    # Leaky integration from both sides (features ~15 m across and smaller keep their full height, long
    # slopes fade away): height rises toward the Sun (+columns), so integrate from the far side.
    k = math.exp(-mpp / 8.0)
    fwd = signal.lfilter([mpp], [1.0, -k], rot[:, ::-1], axis=1)[:, ::-1]
    bwd = -signal.lfilter([mpp], [1.0, -k], rot, axis=1)
    h = (0.5 * (fwd + bwd)).astype(np.float32)
    h = h - box_blur(h, max(int(12.0 / mpp), 4))
    # A one-directional integration leaves fibres along the Sun line: soften across it.
    h = ndimage.uniform_filter1d(h, max(int(1.5 / mpp), 2), axis=0)
    back = ndimage.rotate(h, -ang, reshape=True, order=1, mode="constant", cval=0.0)
    # Crop the rotated-back array to the original shape (rotation with reshape pads symmetrically).
    oy = (back.shape[0] - ratio.shape[0]) // 2
    ox = (back.shape[1] - ratio.shape[1]) // 2
    fine = back[oy:oy + ratio.shape[0], ox:ox + ratio.shape[1]].astype(np.float32)
    fine[mask] = 0.0
    return np.clip(fine, -FINE_M, FINE_M), tilt


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


def global_colour_from(path, lat, lon):
    """Mean linear colour of an equirectangular (-180..180) global map around lat/lon."""
    if not os.path.exists(path):
        return np.array([0.45, 0.28, 0.18])
    Image.MAX_IMAGE_PIXELS = None
    im = Image.open(path)
    W, H = im.size
    x = int((lon180(lon) + 180.0) / 360.0 * W)
    y = int((90.0 - lat) / 180.0 * H)
    crop = np.asarray(im.crop((x - 4, y - 4, x + 4, y + 4)).convert("RGB"), dtype=np.float32) / 255.0
    lin = np.where(crop <= 0.04045, crop / 12.92, ((crop + 0.055) / 1.055) ** 2.4)
    return lin.reshape(-1, 3).mean(axis=0)


def sun_azimuth_check(res, dem, step, az_candidates):
    """Which Sun azimuth explains the photo: the brightness residual should correlate positively with
    the DTM slope toward the Sun. Returns (best azimuth, {azimuth: correlation})."""
    gy, gx = np.gradient(dem, step)
    d_e, d_n = gx, -gy
    r = np.asarray(Image.fromarray(res.astype(np.float32)).resize((dem.shape[1], dem.shape[0]), Image.BILINEAR))
    out = {}
    for az in az_candidates:
        a = math.radians(az)
        tilt = d_e * math.sin(a) + d_n * math.cos(a)
        m = np.isfinite(r) & np.isfinite(tilt)
        out[az] = float(np.corrcoef(r[m].ravel(), tilt[m].ravel())[0, 1])
    return max(out, key=out.get), out


def bake_other_site(site, size_km, texbake):
    """A HiRISE (or similar) site: attached-label DTM .IMG + JP2 orthophoto on the same equirectangular grid."""
    spec = OTHER_SITES[site]
    folder = os.path.join(ROOT, "assets", "textures", spec["folder"])
    dtm_path = os.path.join(folder, spec["dtm"])
    ortho_path = os.path.join(folder, spec["ortho"])
    if not os.path.exists(dtm_path):
        print(f"{site}: DTM not downloaded, skipping")
        return False
    lat0, lon0 = spec["latlon"]
    radius = spec["radius"]
    arr, lab = read_pds_img(dtm_path)
    (r0, r1, c0, c1), bounds = crop_window(lab, arr.shape, lat0, lon0, size_km, radius)
    dem = np.array(arr[r0:r1, c0:c1], dtype=np.float32)
    valid = dem > -1e30
    dem = fill_nodata(dem, valid)
    h, w = dem.shape
    step = lab["MAP_SCALE"]
    lo, hi = float(dem.min()), float(dem.max())
    print(f"{site}: DTM window {w}x{h} at {step:.2f} m, heights {lo:.1f} .. {hi:.1f} m, {valid.mean() * 100:.0f}% valid")
    gy, gx = np.gradient(dem, step)
    d_east, d_north = gx, -gy
    inv = 1.0 / np.sqrt(1.0 + d_east ** 2 + d_north ** 2)
    nx, ny = -d_east * inv, -d_north * inv

    tint = global_colour_from(os.path.join(ROOT, "assets", "textures", spec["tint_map"]), lat0, lon0)
    fine, tilt, sun_en, albedo = None, None, (0.0, 1.0), None
    olbl = ortho_path[:-4] + ".LBL"
    if os.path.exists(ortho_path) and os.path.exists(olbl):
        os.environ.setdefault("OPENCV_IO_MAX_IMAGE_PIXELS", "2000000000")
        import cv2
        olab = read_label(olbl)
        # Decode at half resolution (50 cm): the 4 km window then fits the 8192 texture cap.
        full = cv2.imread(ortho_path, cv2.IMREAD_REDUCED_GRAYSCALE_2)
        if full is None:
            full = np.asarray(Image.open(ortho_path).reduce(2))
        (q0, q1, p0, p1), _ = crop_window(olab, full.shape[:2], lat0, lon0, size_km, radius)
        img = np.array(full[q0:q1, p0:p1], dtype=np.float32)
        del full
        good = img > 0
        sun_inc, sun_az = spec["sun"]
        inc, az = math.radians(sun_inc), math.radians(sun_az)
        sun = np.array([math.sin(inc) * math.sin(az), math.sin(inc) * math.cos(az), math.cos(inc)])
        shade = np.clip(nx * sun[0] + ny * sun[1] + inv * sun[2], 0.05, 1.0) / max(sun[2], 0.05)
        shade_img = np.asarray(Image.fromarray(shade.astype(np.float32)).resize((img.shape[1], img.shape[0]), Image.BILINEAR))
        flat = img / np.maximum(shade_img, 0.12)
        flat[~good] = np.median(flat[good])
        mpp = step * h / img.shape[0]
        # Sanity check on the Sun azimuth: raw brightness must follow the DTM slope toward the Sun.
        res0 = img / np.maximum(box_blur(img, max(int(12.0 / mpp), 2)), 1e-3) - 1.0
        best, corr = sun_azimuth_check(res0, dem, step, [sun_az, (sun_az + 180.0) % 360.0])
        print(f"{site}: Sun azimuth check {corr} -> using {sun_az:.0f}" + ("" if best == sun_az else "  (WARNING: opposite fits better)"))
        mask = np.zeros(img.shape, dtype=bool)
        fine, tilt = fine_relief(flat, mask | ~good, inc, az, mpp, gain=0.6)
        sun_en = (math.sin(az), math.cos(az))
        print(f"{site}: fine relief from shading, {np.abs(fine).mean():.2f} m mean, {np.abs(fine).max():.2f} m max")
        flat = flat / np.median(flat) * float(tint.mean())
        albedo = np.clip(flat[..., None] * (tint / max(tint.mean(), 1e-4))[None, None, :], 0.0, 1.0)
        print(f"{site}: albedo from {os.path.basename(ortho_path)} ({img.shape[1]}x{img.shape[0]} at {mpp:.2f} m, incidence {sun_inc:.0f} deg)")
    if albedo is None:
        print(f"{site}: no orthophoto, using the global tint")
        albedo = np.broadcast_to(tint, (h, w, 3)).copy()
    write_patch(folder, site, dem, step, albedo, fine, tilt, sun_en, bounds, texbake)
    return True


def write_patch(folder, site, dem, step, albedo, fine, tilt, sun_en, bounds, texbake):
    """The four output files from the DTM window, albedo and (optional) fine relief."""
    h, w = dem.shape
    ah, aw = albedo.shape[:2]
    s = min(1.0, 8192.0 / max(ah, aw))
    if fine is not None:
        fh, fw = fine.shape
        dem_up = np.asarray(Image.fromarray(dem).resize((fw, fh), Image.BILINEAR), dtype=np.float32)
        full = dem_up + fine
        step_f = step * (h / fh)
    else:
        fh, fw = h, w
        dem_up = dem
        full = dem
        fine = np.zeros_like(dem)
        step_f = step
    lo, hi = float(full.min()), float(full.max())
    gy, gx = np.gradient(full, step_f)
    d_e, d_n = gx, -gy  # row 0 is north
    if tilt is not None:
        # Along the photo's Sun line the shading gives the slope directly (crisp, no integration fibres):
        # the DTM's coarse slope plus the fine tilt; across it the integrated height is all there is.
        ce, cn = np.gradient(dem_up, step_f)[1], -np.gradient(dem_up, step_f)[0]
        par_coarse = ce * sun_en[0] + cn * sun_en[1]
        par_fine = d_e * sun_en[0] + d_n * sun_en[1]
        par_new = par_coarse + np.tan(tilt)
        d_e = d_e + (par_new - par_fine) * sun_en[0]
        d_n = d_n + (par_new - par_fine) * sun_en[1]
    inv_f = 1.0 / np.sqrt(1.0 + d_e ** 2 + d_n ** 2)
    nrm = np.empty((fh, fw, 3), dtype=np.uint8)
    nrm[..., 0] = np.clip(-d_e * inv_f * 127.5 + 127.5 + 0.5, 0, 255)
    nrm[..., 1] = np.clip(-d_n * inv_f * 127.5 + 127.5 + 0.5, 0, 255)
    nrm[..., 2] = np.clip((fine / FINE_M * 0.5 + 0.5) * 255 + 0.5, 0, 255)
    if s < 1.0:
        nrm = np.asarray(Image.fromarray(nrm).resize((int(fw * s), int(fh * s)), Image.LANCZOS))
    bake_raw(texbake, nrm, os.path.join(folder, "patch_normal.dds"), 3, ["--linear"])
    hs = min(1.0, 8192.0 / max(fh, fw))
    hn = (full - lo) / max(hi - lo, 1e-3)
    if hs < 1.0:
        hn = np.asarray(Image.fromarray(hn.astype(np.float32)).resize((int(fw * hs), int(fh * hs)), Image.BILINEAR))
    write_r16_dds(os.path.join(folder, "patch_height.dds"), hn)
    srgb = np.where(albedo <= 0.0031308, albedo * 12.92, 1.055 * np.power(albedo, 1 / 2.4) - 0.055)
    rgb8 = (np.clip(srgb, 0, 1) * 255 + 0.5).astype(np.uint8)
    if s < 1.0:
        rgb8 = np.asarray(Image.fromarray(rgb8).resize((int(aw * s), int(ah * s)), Image.LANCZOS))
    bake_raw(texbake, rgb8, os.path.join(folder, "patch_albedo.dds"), 3, [])
    with open(os.path.join(folder, "patch.txt"), "w") as f:
        f.write(f"{bounds[0]:.9f} {bounds[1]:.9f} {bounds[2]:.9f} {bounds[3]:.9f} {lo:.3f} {hi:.3f}\n")
    print(f"{site}: patch written, bounds lon {bounds[0]:.5f}+{bounds[2]:.5f} lat {bounds[1]:.5f}+{bounds[3]:.5f}")


def bake_site(site, size_km, texbake):
    if site in OTHER_SITES:
        return bake_other_site(site, size_km, texbake)
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

    # DTM normals (east/north components), central differences: the image's own shading is divided out
    # with these, and the fine relief recovered from the photo is added on top for the final maps.
    gy, gx = np.gradient(dem, step)
    d_east, d_north = gx, -gy  # row 0 is north
    inv = 1.0 / np.sqrt(1.0 + d_east ** 2 + d_north ** 2)
    nx, ny = -d_east * inv, -d_north * inv
    fine, tilt, sun_en = None, None, (0.0, 1.0)  # metres / radians at orthophoto resolution, once a photo is processed

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
        # The relief the DTM cannot resolve (boulders, small craters, rims) from the photo's shading.
        fine, tilt = fine_relief(flat, mask | ~good, inc, az, mpp, gain=0.6)
        sun_en = (math.sin(az), math.cos(az))  # unit vector toward the Sun in (east, north)
        print(f"{site}: fine relief from shading, {np.abs(fine).mean():.2f} m mean, {np.abs(fine).max():.2f} m max")
        # Normalise to the global map's brightness and colour at the site.
        flat = flat / np.median(flat) * float(tint.mean())
        albedo = np.clip(flat[..., None] * (tint / max(tint.mean(), 1e-4))[None, None, :], 0.0, 1.0)
        print(f"{site}: albedo from {os.path.basename(tif)} ({img.shape[1]}x{img.shape[0]}, incidence {math.degrees(inc):.0f} deg)")
        break
    if albedo is None:
        print(f"{site}: no usable orthophoto, using the DTM shading-free global tint")
        albedo = np.broadcast_to(tint, (h, w, 3)).copy()
    write_patch(folder, site, dem, step, albedo, fine, tilt, sun_en, bounds, texbake)
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
    sites = [args.site] if args.site else sorted(LM_SITES) + sorted(OTHER_SITES)
    for site in sites:
        bake_site(site, args.size_km, texbake)


if __name__ == "__main__":
    main()
