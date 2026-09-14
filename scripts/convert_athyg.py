#!/usr/bin/env python3
"""Convert the AT-HYG database (Gaia DR3 + Hipparcos + Tycho merge, ~2.5M stars) into the engine's
packed catalogue format. Same output layout as fetch_gaia.py, so the engine loads either.

Download the two gzip parts from https://github.com/astronexus/ATHYG-Database/tree/main/data and run:
    python scripts/convert_athyg.py athyg_v32-1.csv.gz athyg_v32-2.csv.gz

Columns used: x0, y0, z0 (equatorial cartesian, parsecs), dist, absmag (V band), ci (B-V).
Only needs the standard library.
"""

import argparse
import csv
import gzip
import math
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from fetch_gaia import R_GAL, blackbody  # noqa: E402


def temperature_from_bv(bv: float) -> float:
    # Ballesteros' formula.
    c = min(max(bv, -0.4), 2.5)
    t = 4600.0 * (1.0 / (0.92 * c + 1.7) + 1.0 / (0.92 * c + 0.62))
    return min(max(t, 2500.0), 40000.0)


def _chain(first_line, f):
    yield first_line
    yield from f


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("inputs", nargs="+", help="athyg csv or csv.gz files")
    ap.add_argument("--max-dist", type=float, default=20000.0, help="parsecs; AT-HYG marks unknown as 100000")
    ap.add_argument("--out", default=str(Path(__file__).resolve().parent.parent / "assets" / "gaia" / "gaia.stars"))
    args = ap.parse_args()

    records = []
    hip_index = []  # (hipparcos id, star index) for constellation figures
    skipped = 0
    fieldnames = None
    for path in args.inputs:
        opener = gzip.open if path.endswith(".gz") else open
        with opener(path, "rt", encoding="utf-8", newline="") as f:
            # The multi-part release only carries the header in part 1.
            first = f.readline()
            if first.startswith("id,"):
                fieldnames = first.strip().split(",")
                reader = csv.DictReader(f, fieldnames=fieldnames)
            else:
                if fieldnames is None:
                    sys.exit(f"{path} has no header and no earlier part supplied one")
                reader = csv.DictReader(_chain(first, f), fieldnames=fieldnames)
            for row in reader:
                try:
                    dist = float(row["dist"])
                    x0, y0, z0 = float(row["x0"]), float(row["y0"]), float(row["z0"])
                    absmag = float(row["absmag"])
                except (ValueError, KeyError):
                    skipped += 1
                    continue
                if dist <= 0.0 or dist >= args.max_dist:
                    skipped += 1
                    continue
                try:
                    bv = float(row["ci"])
                except ValueError:
                    bv = 0.65
                try:
                    hip = int(float(row["hip"])) if row.get("hip") else 0
                except ValueError:
                    hip = 0
                if hip > 0:
                    hip_index.append((hip, len(records)))

                gx = R_GAL[0][0] * x0 + R_GAL[0][1] * y0 + R_GAL[0][2] * z0
                gy = R_GAL[1][0] * x0 + R_GAL[1][1] * y0 + R_GAL[1][2] * z0
                gz = R_GAL[2][0] * x0 + R_GAL[2][1] * y0 + R_GAL[2][2] * z0
                x, y, z = gx, gz, -gy  # engine axes: x galactic centre, y north pole

                lum = 10 ** (-0.4 * (absmag - 4.83))  # solar luminosities, V band
                r, g, b = blackbody(temperature_from_bv(bv))
                records.append((x, y, z, lum, r, g, b, 0.0))
        print(f"  {path}: {len(records)} stars so far ({skipped} skipped)", flush=True)

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("wb") as f:
        f.write(struct.pack("<4sIII", b"STAR", 1, len(records), 0))
        pack = struct.Struct("<8f")
        for rec in records:
            f.write(pack.pack(*rec))
    print(f"Wrote {len(records)} stars to {out} ({out.stat().st_size / 1e6:.1f} MB)")

    # Sidecar: Hipparcos id -> star index, sorted by id, as uint32 pairs.
    hip_path = out.with_name("hip_index.bin")
    hip_index.sort()
    with hip_path.open("wb") as f:
        f.write(struct.pack("<4sII", b"HIPX", 1, len(hip_index)))
        pack = struct.Struct("<II")
        for hip, idx in hip_index:
            f.write(pack.pack(hip, idx))
    print(f"Wrote {len(hip_index)} Hipparcos ids to {hip_path}")


if __name__ == "__main__":
    main()
