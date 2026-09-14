#!/usr/bin/env python3
"""Fetch the 88 IAU constellation figures (Stellarium's "modern" sky culture, CC BY-SA 4.0) and write
assets/constellations.txt:

    C <abbr> <latin name>|<english name>
    L <hip> <hip> <hip> ...        (one polyline of Hipparcos ids, repeated per figure segment)

Only needs the standard library.
"""

import json
import sys
import urllib.request
from pathlib import Path

URL = "https://raw.githubusercontent.com/Stellarium/stellarium/master/skycultures/modern/index.json"


def main():
    out = Path(__file__).resolve().parent.parent / "assets" / "constellations.txt"
    req = urllib.request.Request(URL, headers={"User-Agent": "Mozilla/5.0 (Space explorer)"})
    with urllib.request.urlopen(req, timeout=60) as r:
        data = json.load(r)
    lines_out = []
    for c in data["constellations"]:
        cid = c["id"]  # "CON modern Aql"
        abbr = cid.split()[-1]
        name = c.get("common_name", {})
        latin = name.get("native") or abbr
        english = name.get("english") or latin
        lines_out.append(f"C {abbr} {latin}|{english}")
        for poly in c.get("lines", []):
            if len(poly) >= 2:
                lines_out.append("L " + " ".join(str(int(h)) for h in poly))
    out.write_text("\n".join(lines_out) + "\n", encoding="utf-8")
    print(f"Wrote {sum(1 for l in lines_out if l.startswith('C'))} constellations to {out}")


if __name__ == "__main__":
    try:
        main()
    except Exception as e:
        print(f"ERROR: {e}", file=sys.stderr)
        sys.exit(1)
