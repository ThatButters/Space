#!/usr/bin/env python3
"""Fetch a Gaia DR3 star subset from the ESA archive and write it as a packed binary catalogue.

Output format (assets/gaia/gaia.stars), little-endian, matches the engine's StarGpu struct:
    char[4]  magic "STAR"
    uint32   version = 1
    uint32   count
    uint32   reserved
    count x { float32 x, y, z, radiance; float32 r, g, b, pad }

Positions are parsecs in a galactic frame remapped to the engine's axes (x toward the galactic
centre, y toward the north galactic pole, z completing a right-handed set). The Sun is at the origin.

Usage: python scripts/fetch_gaia.py [--max-rows N] [--parallax-min MAS] [--gmag-max MAG] [--sample FRACTION]
Only needs the standard library.
"""

import argparse
import csv
import io
import math
import struct
import sys
import time
import urllib.parse
import urllib.request
from pathlib import Path

TAP = "https://gea.esac.esa.int/tap-server/tap"

# ICRS -> galactic rotation (Hipparcos / Gaia convention).
R_GAL = (
    (-0.0548755604162154, -0.8734370902348850, -0.4838350155487132),
    (+0.4941094278755837, -0.4448296299600112, +0.7469822444972189),
    (-0.8676661490190047, -0.1980763734312015, +0.4559837761750669),
)


def blackbody(kelvin: float):
    t = min(max(kelvin, 1500.0), 40000.0) / 100.0
    r = 1.0 if t <= 66 else min(max(1.2929 * (t - 60) ** -0.1332, 0.0), 1.0)
    g = min(max(0.3901 * math.log(t) - 0.6318, 0.0), 1.0) if t <= 66 else min(max(1.1299 * (t - 60) ** -0.0755, 0.0), 1.0)
    b = 1.0 if t >= 66 else (0.0 if t <= 19 else min(max(0.5432 * math.log(t - 10) - 1.1962, 0.0), 1.0))
    return r * r, g * g, b * b


def temperature_from_bp_rp(bp_rp: float) -> float:
    # Rough Gaia BP-RP -> Teff (Andrae et al. style polynomial, clamped to sane stellar range).
    if bp_rp is None or math.isnan(bp_rp):
        return 5500.0
    c = min(max(bp_rp, -0.5), 4.0)
    log_t = 3.999 - 0.654 * c + 0.709 * c * c - 0.316 * c ** 3
    return min(max(10 ** log_t, 2500.0), 40000.0)


def submit_job(query: str) -> str:
    data = urllib.parse.urlencode({
        "REQUEST": "doQuery", "LANG": "ADQL", "FORMAT": "csv", "PHASE": "RUN", "QUERY": query,
    }).encode()
    req = urllib.request.Request(f"{TAP}/async", data=data, method="POST")

    class NoRedirect(urllib.request.HTTPRedirectHandler):
        def redirect_request(self, req, fp, code, msg, headers, newurl):
            return None

    opener = urllib.request.build_opener(NoRedirect)
    try:
        resp = opener.open(req)
        job_url = resp.geturl()
    except urllib.error.HTTPError as e:
        if e.code in (301, 302, 303) and "Location" in e.headers:
            job_url = e.headers["Location"]
        else:
            raise
    return job_url


def wait_job(job_url: str, poll: float = 5.0):
    while True:
        with urllib.request.urlopen(f"{job_url}/phase") as r:
            phase = r.read().decode().strip()
        print(f"  job phase: {phase}", flush=True)
        if phase == "COMPLETED":
            return
        if phase in ("ERROR", "ABORTED"):
            with urllib.request.urlopen(f"{job_url}/error") as r:
                raise RuntimeError(r.read().decode()[:2000])
        time.sleep(poll)


def fetch_results(job_url: str) -> str:
    with urllib.request.urlopen(f"{job_url}/results/result") as r:
        return r.read().decode()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--max-rows", type=int, default=3_000_000, help="TOP N rows (archive async cap is 3M)")
    ap.add_argument("--parallax-min", type=float, default=0.4, help="mas; 0.4 = within ~2.5 kpc")
    ap.add_argument("--gmag-max", type=float, default=13.5)
    ap.add_argument("--sample", type=float, default=0.25, help="fraction of the random_index space to draw from")
    ap.add_argument("--out", default=str(Path(__file__).resolve().parent.parent / "assets" / "gaia" / "gaia.stars"))
    ap.add_argument("--csv", default=None, help="skip the download and convert this CSV instead")
    args = ap.parse_args()

    if args.csv:
        text = Path(args.csv).read_text()
    else:
        random_cap = int(1_811_709_771 * args.sample)
        query = (
            f"SELECT TOP {args.max_rows} ra, dec, parallax, phot_g_mean_mag, bp_rp "
            f"FROM gaiadr3.gaia_source "
            f"WHERE parallax > {args.parallax_min} AND parallax_over_error > 5 "
            f"AND phot_g_mean_mag < {args.gmag_max} AND random_index < {random_cap}"
        )
        print("Submitting ADQL job:\n  " + query, flush=True)
        job_url = submit_job(query)
        print(f"  job: {job_url}", flush=True)
        wait_job(job_url)
        print("Downloading results...", flush=True)
        text = fetch_results(job_url)
        raw = Path(args.out).with_suffix(".csv")
        raw.parent.mkdir(parents=True, exist_ok=True)
        raw.write_text(text)
        print(f"  saved raw CSV to {raw} ({len(text) / 1e6:.1f} MB)", flush=True)

    print("Converting...", flush=True)
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    records = []
    reader = csv.DictReader(io.StringIO(text))
    for row in reader:
        try:
            ra = math.radians(float(row["ra"]))
            dec = math.radians(float(row["dec"]))
            plx = float(row["parallax"])
            gmag = float(row["phot_g_mean_mag"])
        except (ValueError, KeyError):
            continue
        if plx <= 0:
            continue
        try:
            bp_rp = float(row["bp_rp"])
        except ValueError:
            bp_rp = float("nan")

        dist_pc = 1000.0 / plx
        cx, cy, cz = math.cos(dec) * math.cos(ra), math.cos(dec) * math.sin(ra), math.sin(dec)
        gx = R_GAL[0][0] * cx + R_GAL[0][1] * cy + R_GAL[0][2] * cz
        gy = R_GAL[1][0] * cx + R_GAL[1][1] * cy + R_GAL[1][2] * cz
        gz = R_GAL[2][0] * cx + R_GAL[2][1] * cy + R_GAL[2][2] * cz
        # Engine axes: x = galactic centre, y = north galactic pole, z = -galactic rotation direction.
        x, y, z = gx * dist_pc, gz * dist_pc, -gy * dist_pc

        abs_g = gmag + 5.0 * math.log10(plx) - 10.0
        lum = 10 ** (-0.4 * (abs_g - 4.67))  # solar luminosities in the G band
        temp = temperature_from_bp_rp(bp_rp)
        r, g, b = blackbody(temp)
        records.append((x, y, z, lum, r, g, b, 0.0))

    with out.open("wb") as f:
        f.write(struct.pack("<4sIII", b"STAR", 1, len(records), 0))
        pack = struct.Struct("<8f")
        for rec in records:
            f.write(pack.pack(*rec))
    print(f"Wrote {len(records)} stars to {out} ({out.stat().st_size / 1e6:.1f} MB)")


if __name__ == "__main__":
    try:
        main()
    except Exception as e:
        print(f"ERROR: {e}", file=sys.stderr)
        sys.exit(1)
