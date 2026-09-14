"""Fetch public-domain global maps of Mars, Mercury and the major moons (USGS Astrogeology mosaics of NASA mission
data) and bake the textures the engine uses, following the same conventions as bake_moon_hires.py.

Every final map is equirectangular, north up (row 0 = 90N), east longitude increasing to the right and longitude 0
at the horizontal centre (left edge = 180W). Source products come in several layouts; each entry below records
where the east longitude of the image's left edge is (after an optional horizontal mirror) and the script rolls the
image so 180W lands on the left edge. All layouts were checked against named features from the USGS Gazetteer of
Planetary Nomenclature (see the notes per body).

Outputs (assets/textures/<body>_hires, git-ignored; downloaded sources and labels are kept next to them):
  <body>_color.dds     BC7 sRGB colour/albedo, full mip chain (width <= 16384, exactly 2:1)
  <body>_normal.dds    BC5 slopes from the DEM: R = east, G = north component of the unit normal  (Mars, Mercury)
  <body>_height.dds    R16 height at half the DEM resolution, 0..1 mapped to [min, max] km       (Mars, Mercury)
  <body>_height.txt    "min_km max_km"

Usage: python scripts/bake_planets_hires.py [body ...] [--texbake path] [--force] [--preview-dir DIR]
Needs numpy and Pillow. Peak RAM about 7 GB (Mars DEM), downloads about 8 GB.
"""
import argparse
import os
import struct
import subprocess
import sys
import urllib.request
import zlib
from concurrent.futures import ThreadPoolExecutor

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEX = os.path.join(ROOT, "assets", "textures")
S3 = "https://asc-pds-services.s3.us-west-2.amazonaws.com/"
MOSAIC = S3 + "mosaic/"

# color: url, extra (labels to keep), left_lon = east longitude of the left image edge after `mirror`,
#        max_w = output width cap. mirror = flip horizontally first (for west-increasing-to-the-right layouts).
# dem:   url, data offset/type from the ISIS label, metres per DN, left_lon, `down` = box-filter factor,
#        radius_km = mean radius used for the slope computation.
BODIES = {
    "mars": {
        # Viking MDIM 2.1-controlled colorized mosaic, 925 m/px (64.05 px/deg), PositiveEast, -180..180.
        # The USGS colorization is strongly blue-shifted in dark albedo regions (dark 20% of pixels average
        # R69 G57 B68 vs R132 G74 B59 in 8k_mars.jpg), so colour and regional brightness are transferred from the
        # app's existing 8k_mars.jpg while the Viking luminance detail is kept (see color_transfer()).
        "color": dict(url=MOSAIC + "Mars_Viking_ClrMosaic_global_925m.tif", labels=["Mars_Viking_ClrMosaic_global_925m_pds3.lbl"],
                      left_lon=-180.0, mirror=False, max_w=16384,
                      transfer=dict(ref="8k_mars.jpg", chroma_grid=2048, gain_grid=256)),
        # MGS MOLA MEGDR 128 px/deg (463 m), int16 metres relative to the MOLA areoid, PositiveEast, -180..180.
        "dem": dict(url=MOSAIC + "Mars_MGS_MOLA_DEM_mosaic_global_463m.tif", labels=["Mars_MGS_MOLA_DEM_mosaic_global_463m_pds3.lbl", "Mars_MGS_MOLA_DEM_mosaic_global_463m.lbl"],
                    width=46080, height=23040, dtype="<i2", scale_m=1.0, offset_m=0.0, left_lon=-180.0, down=2, radius_km=3389.5),
    },
    "mercury": {
        # MESSENGER MDIS BDR monochrome basemap 256 px/deg (166 m), tiled deflate GeoTIFF, PositiveEast, -180..180.
        "color": dict(url=MOSAIC + "Mercury_MESSENGER_MDIS_Basemap_BDR_Mosaic_Global_256ppd.tif", labels=["Mercury_MESSENGER_MDIS_Basemap_BDR_Mosaic_Global_166m_pds3.lbl"],
                      left_lon=-180.0, mirror=False, max_w=16384, tiled_reduce=4),
        # USGS MESSENGER global DEM v2, 64 px/deg (665 m), int16 * 0.5 m relative to 2439.4 km, PositiveEast, 0..360.
        "dem": dict(url=MOSAIC + "Mercury_Messenger_USGS_DEM_Global_665m_v2.tif", labels=["Mercury_Messenger_USGS_DEM_Global_665m_v2_pds3.lbl", "Mercury_Messenger_USGS_DEM_Global_665m_v2.lbl"],
                    width=23040, height=11520, dtype="<i2", scale_m=0.5, offset_m=0.0, left_lon=0.0, down=1, radius_km=2439.4),
    },
    "venus": {
        # Magellan C3-MDIR synthetic-aperture radar global mosaic, 2025 m (52.15 px/deg), greyscale (radar
        # backscatter: rough = bright). PositiveEast, clon 0, -180..180. The surface under the clouds, tinted to
        # the orange of the Venera 13 panoramas (the sky there is that colour, so the ground appears so too).
        "color": dict(url=MOSAIC + "Venus_Magellan_C3-MDIR_Global_Mosaic_2025m.tif", labels=["Venus_Magellan_C3-MDIR_Global_Mosaic_2025m_pds3.lbl", "Venus_Magellan_C3-MDIR_Global_Mosaic_2025m.lbl"],
                      left_lon=-180.0, mirror=False, max_w=16384, tint=(0.78, 0.55, 0.32)),
    },
    "io": {
        # Galileo SSI / Voyager colour merge, 1 km (31.79 px/deg). Label PositiveWest, clon 0, -180..180; ISIS keeps
        # sample numbers increasing eastward, so the image is already east-right with 180 at the left edge.
        "color": dict(url=MOSAIC + "Io_GalileoSSI-Voyager_Global_Mosaic_ClrMerge_1km.tif", labels=["Io_GalileoSSI-Voyager_Global_Mosaic_ClrMerge_1km_pds3.lbl"],
                      left_lon=-180.0, mirror=False, max_w=16384),
    },
    "europa": {
        # Voyager / Galileo SSI global mosaic, 500 m (54.53 px/deg), greyscale. PositiveWest, clon 180, 0..360:
        # east-right image with longitude 0 at the left edge.
        "color": dict(url=MOSAIC + "Europa_Voyager_GalileoSSI_global_mosaic_500m.tif", labels=["Europa_Voyager_GalileoSSI_global_mosaic_500m_pds3.lbl"],
                      left_lon=0.0, mirror=False, max_w=16384),
    },
    "ganymede": {
        # Voyager / Galileo SSI colour mosaic, 1435 m (32 px/deg). PositiveEast, clon 180, 0..360.
        "color": dict(url=MOSAIC + "Ganymede_Voyager_GalileoSSI_Global_ClrMosaic_1435m.tif", labels=["Ganymede_Voyager_GalileoSSI_Global_ClrMosaic_1435m_pds3.lbl"],
                      left_lon=0.0, mirror=False, max_w=16384),
    },
    "callisto": {
        # Voyager / Galileo SSI global mosaic, 1 km (42.05 px/deg), greyscale. PositiveWest, clon 180, 0..360.
        "color": dict(url=MOSAIC + "Callisto_Voyager_GalileoSSI_global_mosaic_1km.tif", labels=["Callisto_Voyager_GalileoSSI_global_mosaic_1km_pds3.lbl"],
                      left_lon=0.0, mirror=False, max_w=16384),
    },
    "titan": {
        # Cassini ISS global mosaic PIA19658 (938 nm, through T100), 4 km (11.22 px/deg). PositiveWest, clon 180, 0..360.
        "color": dict(url=MOSAIC + "Titan_ISS_P19658_Mosaic_Global_4km.tif", labels=["Titan_ISS_P19658_Mosaic_Global_4km_pds3.lbl"],
                      left_lon=0.0, mirror=False, max_w=16384),
    },
    "rhea": {
        # Cassini / Voyager global mosaic, 417 m (32 px/deg). PositiveWest, clon 180, UpperLeftX = 0: 180 at the left edge.
        "color": dict(url=MOSAIC + "Rhea_Cassini_Voyager_mosaic_global_417m.tif", labels=["Rhea_Cassini_Voyager_mosaic_global_417m_pds3.lbl"],
                      left_lon=-180.0, mirror=False, max_w=16384),
    },
    "triton": {
        # Voyager 2 colour mosaic with global fill (PIA18668, P. Schenk / LPI), 600 m (39.27 px/deg). PositiveEast, -180..180.
        "color": dict(url=MOSAIC + "Triton_Voyager2_ClrMosaic_GlobalFill_600m.tif", labels=["Triton_Voyager2_ClrMosaic_GlobalFill_600m_pds3.lbl"],
                      left_lon=-180.0, mirror=False, max_w=16384),
    },
    "phobos": {
        # Viking Orbiter mosaic with DLR control (P. Stooke), 40 px/deg. PositiveEast, -180..180.
        "color": dict(url=MOSAIC + "Phobos_Viking_Mosaic_40ppd_DLRcontrol.tif", labels=["Phobos_Viking_Mosaic_40ppd_DLRcontrol_pds3.lbl"],
                      left_lon=-180.0, mirror=False, max_w=8192),
    },
    "deimos": {
        # Viking-based cylindrical map (P. Stooke, control P. Thomas), 10 px/deg; world file: left edge at 0.
        "color": dict(url=S3 + "wms_basemaps/Deimos/deimoscyl4.jpg", labels=[], extra=[S3 + "wms_basemaps/Deimos/deimoscyl4.jgw"],
                      left_lon=0.0, mirror=False, max_w=8192),
    },
}


def http_size(url):
    with urllib.request.urlopen(urllib.request.Request(url, method="HEAD")) as r:
        return int(r.headers["Content-Length"])


def fetch(out, url):
    """Download url into out/ unless a file of the size reported by a HEAD request is already there."""
    path = os.path.join(out, url.rsplit("/", 1)[1])
    size = http_size(url)
    if os.path.exists(path) and os.path.getsize(path) == size:
        return path
    print(f"downloading {url} ({size / 1e6:.0f} MB)")
    tmp = path + ".part"
    with urllib.request.urlopen(url) as r, open(tmp, "wb") as f:
        while True:
            chunk = r.read(8 << 20)
            if not chunk:
                break
            f.write(chunk)
    if os.path.getsize(tmp) != size:
        sys.exit(f"short download: {url}")
    os.replace(tmp, path)
    return path


def fetch_labels(out, spec):
    for name in spec.get("labels", []):
        try:
            fetch(out, MOSAIC + name)
        except Exception as e:  # labels are provenance only
            print("label not fetched:", name, e)
    for url in spec.get("extra", []):
        fetch(out, url)


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
    try:
        subprocess.check_call(cmd)
    finally:
        if os.path.exists(raw):
            os.remove(raw)


def write_r16_dds(path, dem, lo, hi):
    """Identical to bake_moon_hires.write_r16_dds: half-resolution R16_UNORM DDS with a box-filtered mip chain."""
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


# ---------------------------------------------------------------------------------------------------------------
# Readers


def tiff_tags(path):
    with Image.open(path) as im:
        return im.size, dict(im.tag_v2)


def _decode_tile(blob, tw, th, compression):
    """Decode one compressed tile. Deflate goes straight through zlib; LZW (and anything else libtiff knows) is
    wrapped in a minimal single-strip TIFF so Pillow's libtiff decoder can handle it."""
    if compression in (8, 32946):
        return np.frombuffer(zlib.decompress(blob), dtype=np.uint8).reshape(th, tw)
    entries = [(256, 3, 1, tw), (257, 3, 1, th), (258, 3, 1, 8), (259, 3, 1, compression), (262, 3, 1, 1),
               (273, 4, 1, 0), (277, 3, 1, 1), (278, 3, 1, th), (279, 4, 1, len(blob))]
    ifd_size = 2 + 12 * len(entries) + 4
    data_off = 8 + ifd_size
    ifd = struct.pack("<H", len(entries))
    for tag, typ, cnt, val in entries:
        val = data_off if tag == 273 else val
        ifd += struct.pack("<HHI", tag, typ, cnt) + (struct.pack("<HH", val, 0) if typ == 3 else struct.pack("<I", val))
    ifd += struct.pack("<I", 0)
    import io
    with Image.open(io.BytesIO(b"II*\0" + struct.pack("<I", 8) + ifd + blob)) as im:
        return np.asarray(im.convert("L"))


def read_tiled_reduced(path, factor):
    """Decode a tiled, compressed (deflate or LZW) 8-bit single-band TIFF tile by tile, box-filtering each tile by
    `factor`, so a 92160x46080 source never has to be held in memory at full resolution."""
    (w, h), tags = tiff_tags(path)
    tw, th = tags[322], tags[323]
    compression = tags[259]
    bps = tags[258]
    assert compression in (5, 8, 32946) and tags.get(317, 1) == 1 and tuple(np.atleast_1d(bps)) == (8,) and tags.get(277, 1) == 1
    assert tw % factor == 0 and th % factor == 0
    offsets, counts = tags[324], tags[325]
    across, down = (w + tw - 1) // tw, (h + th - 1) // th
    out = np.zeros((down * th // factor, across * tw // factor), dtype=np.uint8)
    rt, ct = th // factor, tw // factor

    with open(path, "rb") as f:
        for ty in range(down):
            blobs = []
            for tx in range(across):
                i = ty * across + tx
                f.seek(offsets[i])
                blobs.append(f.read(counts[i]))

            def dec(b):
                a = _decode_tile(b, tw, th, compression)
                return (a.reshape(rt, factor, ct, factor).mean(axis=(1, 3), dtype=np.float32) + 0.5).astype(np.uint8)

            with ThreadPoolExecutor(8) as ex:
                for tx, small in enumerate(ex.map(dec, blobs)):
                    out[ty * rt:(ty + 1) * rt, tx * ct:(tx + 1) * ct] = small
            if ty % 20 == 0:
                print(f"  tile row {ty}/{down}")
    return out[: h // factor, : w // factor]


def roll_to_center(img, left_lon):
    """Roll horizontally so that 180W is at the left edge, given the east longitude of the current left edge."""
    w = img.shape[1]
    shift = int(round(((left_lon + 180.0) % 360.0) / 360.0 * w)) % w
    return np.roll(img, shift, axis=1) if shift else img


_LIN = ((np.arange(256, dtype=np.float32) / 255.0) ** 2.2).astype(np.float32)
_LUMA = np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)


def _box_lin(a, factor):
    """Box-average an 8-bit RGB array in linear light by an integer factor (row chunks to bound memory)."""
    h, w, _ = a.shape
    oh, ow = h // factor, w // factor
    out = np.empty((oh, ow, 3), dtype=np.float32)
    step = max(1, 256 // factor)
    for r0 in range(0, oh, step):
        r1 = min(r0 + step, oh)
        blk = _LIN[a[r0 * factor:r1 * factor, : ow * factor]]
        out[r0:r1] = blk.reshape(r1 - r0, factor, ow, factor, 3).mean(axis=(1, 3))
    return out


def _bilinear_rows(grid, y0, y1, H, W):
    """Bilinearly upsample a coarse global grid (wrapping in longitude, clamped in latitude) for output rows y0..y1."""
    gh, gw = grid.shape[:2]
    ys = (np.arange(y0, y1) + 0.5) * gh / H - 0.5
    xs = (np.arange(W) + 0.5) * gw / W - 0.5
    iy = np.floor(ys).astype(np.int64)
    fy = (ys - iy).astype(np.float32).reshape(-1, 1, *([1] * (grid.ndim - 2)))
    ix = np.floor(xs).astype(np.int64)
    fx = (xs - ix).astype(np.float32).reshape(1, -1, *([1] * (grid.ndim - 2)))
    rows = grid[np.clip(iy, 0, gh - 1)] * (1 - fy) + grid[np.clip(iy + 1, 0, gh - 1)] * fy
    return rows[:, ix % gw] * (1 - fx) + rows[:, (ix + 1) % gw] * fx


def color_transfer(a, spec):
    """Low-frequency colour transfer from a reference global map with the same layout, in linear light:
    luminance = source luminance * (reference / source) luminance ratio on a coarse `gain_grid` (regional brightness),
    colour    = reference chromaticity (RGB / luminance) on a finer `chroma_grid`.
    Source detail finer than the gain grid is kept; the source's own hue is discarded."""
    H, W, _ = a.shape
    ref = np.asarray(Image.open(os.path.join(TEX, spec["ref"])).convert("RGB"))
    rh, rw, _ = ref.shape
    assert rw == 2 * rh and W == 2 * H and rw % spec["chroma_grid"] == 0 and spec["chroma_grid"] % spec["gain_grid"] == 0
    ref_c = _box_lin(ref, rw // spec["chroma_grid"])
    del ref
    k = spec["chroma_grid"] // spec["gain_grid"]
    ref_g = ref_c.reshape(ref_c.shape[0] // k, k, ref_c.shape[1] // k, k, 3).mean(axis=(1, 3)) @ _LUMA
    src_g = _box_lin(a, W // spec["gain_grid"]) @ _LUMA
    eps = 1e-4
    gain = np.clip(ref_g / np.maximum(src_g, eps), 0.25, 8.0).astype(np.float32)
    chroma = (ref_c / np.maximum(ref_c @ _LUMA, eps)[..., None]).astype(np.float32)
    print(f"  colour transfer from {spec['ref']}: gain {gain.min():.2f}..{gain.max():.2f} (median {np.median(gain):.2f})")
    out = np.empty_like(a)
    for y0 in range(0, H, 512):
        y1 = min(y0 + 512, H)
        y = _LIN[a[y0:y1]] @ _LUMA
        lin = (y * _bilinear_rows(gain, y0, y1, H, W))[..., None] * _bilinear_rows(chroma, y0, y1, H, W)
        out[y0:y1] = (np.clip(lin, 0.0, 1.0) ** (1.0 / 2.2) * 255.0 + 0.5).astype(np.uint8)
    return out


def prepare_color(body, out):
    spec = BODIES[body]["color"]
    src = fetch(out, spec["url"])
    fetch_labels(out, spec)
    Image.MAX_IMAGE_PIXELS = None
    if spec.get("tiled_reduce"):
        im = Image.fromarray(read_tiled_reduced(src, spec["tiled_reduce"]))
    else:
        im = Image.open(src)
        im.load()
    print(f"{body}: source {os.path.basename(src)} {im.size[0]}x{im.size[1]} {im.mode}")
    if im.mode not in ("L", "RGB"):
        im = im.convert("RGB")
    sw = im.size[0]
    tw = min(spec["max_w"], sw // 8 * 8 if sw % 8 else sw)  # multiple of 8 so the 2:1 height is a multiple of 4
    th = tw // 2
    if im.size != (tw, th):
        im = im.resize((tw, th), Image.LANCZOS)
    a = np.asarray(im)
    if a.ndim == 2:  # greyscale: replicate to RGB, natural brightness (or a flat tint in linear light)
        if spec.get("tint"):
            lin = _LIN[a][:, :, None] * np.asarray(spec["tint"], dtype=np.float32)[None, None, :]
            a = (np.clip(lin, 0.0, 1.0) ** (1.0 / 2.2) * 255.0 + 0.5).astype(np.uint8)
        else:
            a = np.repeat(a[:, :, None], 3, axis=2)
    if spec["mirror"]:
        a = a[:, ::-1]
    a = np.ascontiguousarray(roll_to_center(a, spec["left_lon"]))
    if spec.get("transfer"):
        a = color_transfer(a, spec["transfer"])
    return a


def prepare_dem(body, out):
    """Heights in km relative to the product's reference surface, oriented like the colour map, box-filtered by `down`."""
    spec = BODIES[body]["dem"]
    src = fetch(out, spec["url"])
    fetch_labels(out, spec)
    (w, h), tags = tiff_tags(src)
    assert (w, h) == (spec["width"], spec["height"]) and tags[259] == 1, "unexpected DEM layout"
    strips = tags[273]
    assert all(strips[i + 1] - strips[i] == w * 2 for i in range(len(strips) - 1)), "DEM strips not contiguous"
    mm = np.memmap(src, dtype=spec["dtype"], mode="r", offset=strips[0], shape=(h, w))
    d = spec["down"]
    dem = np.empty((h // d, w // d), dtype=np.float32)
    nodata = 0
    for y0 in range(0, h // d, 512):
        y1 = min(y0 + 512, h // d)
        block = mm[y0 * d:y1 * d].astype(np.float32)
        bad = block <= -32767
        nodata += int(bad.sum())
        if bad.any():
            block[bad] = np.nan
        if d > 1:
            block = np.nanmean(block.reshape(y1 - y0, d, w // d, d), axis=(1, 3))
        dem[y0:y1] = (block * spec["scale_m"] + spec["offset_m"]) / 1000.0
    del mm
    if nodata:
        print(f"  {nodata} nodata samples; filled with row means")
        rows = np.where(np.isnan(dem).any(axis=1))[0]
        for r in rows:
            m = np.isnan(dem[r])
            dem[r, m] = np.nanmean(dem[r]) if (~m).any() else 0.0
    return np.ascontiguousarray(roll_to_center(dem, spec["left_lon"]))


# ---------------------------------------------------------------------------------------------------------------


def bake_color(body, out, texbake, force, preview_dir):
    dds = os.path.join(out, f"{body}_color.dds")
    if not force and os.path.exists(dds) and not preview_dir:
        return
    img = prepare_color(body, out)
    h, w, _ = img.shape
    if preview_dir:
        os.makedirs(preview_dir, exist_ok=True)
        Image.fromarray(img).save(os.path.join(preview_dir, f"{body}_color.jpg"), quality=92)
    if force or not os.path.exists(dds):
        raw = os.path.join(out, f"{body}_color.raw")
        img.tofile(raw)
        del img
        bake(texbake, raw, dds, w, h, 3, [])


def bake_relief(body, out, texbake, force, preview_dir):
    spec = BODIES[body].get("dem")
    if not spec:
        return
    normal_dds = os.path.join(out, f"{body}_normal.dds")
    height_dds = os.path.join(out, f"{body}_height.dds")
    if not force and os.path.exists(normal_dds) and os.path.exists(height_dds) and not preview_dir:
        return
    dem = prepare_dem(body, out)
    h, w = dem.shape
    lo, hi = float(dem.min()), float(dem.max())
    print(f"{body}: elevation {w}x{h}, {lo:.3f} .. {hi:.3f} km, slope radius {spec['radius_km']} km")
    if preview_dir:
        os.makedirs(preview_dir, exist_ok=True)
        s = dem[::4, ::4]
        Image.fromarray(((s - lo) / (hi - lo) * 255).astype(np.uint8)).save(os.path.join(preview_dir, f"{body}_height.png"))
    if not force and os.path.exists(normal_dds) and os.path.exists(height_dds):
        return
    with open(os.path.join(out, f"{body}_height.txt"), "w") as f:
        f.write(f"{lo:.6f} {hi:.6f}\n")
    write_r16_dds(height_dds, dem, lo, hi)
    # Slopes by central differences (wrapping in longitude), metres per metre, as in bake_moon_hires.py.
    radius_m = spec["radius_km"] * 1000.0
    dy_m = np.pi * radius_m / h
    raw = os.path.join(out, f"{body}_normal.raw")
    with open(raw, "wb") as f:
        for y0 in range(0, h, 1024):
            y1 = min(y0 + 1024, h)
            rows = np.arange(y0, y1)
            lat = (0.5 - (rows + 0.5) / h) * np.pi
            dx_m = 2.0 * np.pi * radius_m * np.maximum(np.cos(lat), 0.01) / w
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("bodies", nargs="*", default=list(BODIES))
    ap.add_argument("--texbake", default=None)
    ap.add_argument("--force", action="store_true")
    ap.add_argument("--preview-dir", default=None, help="also write full-size JPEG/PNG previews here (for checking orientation)")
    args = ap.parse_args()
    texbake = find_texbake(args.texbake)
    for body in args.bodies:
        if body not in BODIES:
            sys.exit(f"unknown body {body}; choose from {', '.join(BODIES)}")
    for body in args.bodies:
        out = os.path.join(TEX, f"{body}_hires")
        os.makedirs(out, exist_ok=True)
        bake_color(body, out, texbake, args.force, args.preview_dir)
        bake_relief(body, out, texbake, args.force, args.preview_dir)
    print("done")


if __name__ == "__main__":
    main()
