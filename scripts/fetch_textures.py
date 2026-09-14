#!/usr/bin/env python3
"""Download planet surface maps into assets/textures.

Sources are NASA / USGS data (Blue Marble, Black Marble, LRO WAC, Viking/MOLA, Cassini, Juno, Messenger,
Magellan) as repackaged equirectangular JPEGs by Solar System Scope (CC BY 4.0,
https://www.solarsystemscope.com/textures/). Only needs the standard library.
"""

import sys
import urllib.request
from pathlib import Path

BASE = "https://www.solarsystemscope.com/textures/download/"
FILES = [
    "8k_sun.jpg",
    "8k_mercury.jpg",
    "4k_venus_atmosphere.jpg",
    "8k_earth_daymap.jpg",
    "8k_earth_nightmap.jpg",
    "8k_earth_clouds.jpg",
    "8k_moon.jpg",
    "8k_mars.jpg",
    "8k_jupiter.jpg",
    "8k_saturn.jpg",
    "8k_saturn_ring_alpha.png",
    "2k_uranus.jpg",
    "2k_neptune.jpg",
]


def main():
    out = Path(__file__).resolve().parent.parent / "assets" / "textures"
    out.mkdir(parents=True, exist_ok=True)
    failed = []
    for name in FILES:
        dst = out / name
        if dst.exists() and dst.stat().st_size > 100_000:
            print(f"  have {name}")
            continue
        try:
            req = urllib.request.Request(BASE + name, headers={"User-Agent": "Mozilla/5.0 (Space explorer)"})
            with urllib.request.urlopen(req, timeout=120) as r, dst.open("wb") as f:
                data = r.read()
                f.write(data)
            head = data[:4]
            ok = head[:3] == b"\xff\xd8\xff" or head == b"\x89PNG"
            print(f"  {'ok  ' if ok else 'BAD '} {name} {len(data) / 1e6:.1f} MB")
            if not ok:
                dst.unlink()
                failed.append(name)
        except Exception as e:
            print(f"  FAIL {name}: {e}")
            failed.append(name)
    if failed:
        print("failed:", ", ".join(failed))
        sys.exit(1)
    print("done")


if __name__ == "__main__":
    main()
