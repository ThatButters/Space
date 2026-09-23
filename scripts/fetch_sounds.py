#!/usr/bin/env python3
"""Download the recorded sounds for silly mode (F9) into assets/sounds. Only needs the standard library.

The farts are one person's real recordings, Jixolros's "realpoot" series on Freesound, all CC0 (public
domain): https://freesound.org/people/Jixolros/ . The files are Freesound's high-quality OGG previews,
which need no account. farts.txt maps each file to the kinds of fart it plays as (a file may play as
several); edit it to re-cast a clip without rebuilding. Without these files the app synthesises its own."""

import sys
import urllib.request
from pathlib import Path

PREVIEWS = "https://cdn.freesound.org/previews/"
UPLOADER = 6518586  # Jixolros

# Freesound id -> the kinds it plays as (SillyMode reads these names from farts.txt).
FARTS = {
    558130: "short",           # small-realpoot106: short and tonal
    558132: "short",           # small-realpoot113
    558474: "short",           # small-realpoot114
    558134: "short medium",    # small-realpoot111
    558149: "medium",          # medium-realpoot109
    561534: "medium",          # small-realpoot118
    561535: "medium",          # small-realpoot117
    558488: "medium deep",     # small-realpoot110-bullfrog: low and croaky
    561204: "long deep",       # medium-realpoot118: 1.5 s, the most tonal of the set
    558128: "long deep",       # big-realpoot101
    558127: "epic",            # big-realpoot102: 6.5 s, forceful
    565746: "squeaky",         # medium-realpoot132: the brightest
    558133: "squeaky",         # small-realpoot112
    558131: "squeaky",         # small-realpoot105
    556506: "bouncy",          # small-realpoots100-104: five in a row
    558476: "crackly",         # medium-realpoot111: the noisiest
    558148: "crackly",         # medium-realpoot110
}


def main():
    out = Path(__file__).resolve().parent.parent / "assets" / "sounds"
    out.mkdir(parents=True, exist_ok=True)
    for sound_id in FARTS:
        path = out / f"{sound_id}.ogg"
        if path.exists() and path.stat().st_size > 0:
            continue
        url = f"{PREVIEWS}{sound_id // 1000}/{sound_id}_{UPLOADER}-hq.ogg"
        print("fetch", url)
        req = urllib.request.Request(url, headers={"User-Agent": "Space/1.0"})
        with urllib.request.urlopen(req, timeout=60) as r:
            path.write_bytes(r.read())
    lines = ["# kind file  (kinds: short medium long deep epic squeaky bouncy crackly)"]
    for sound_id, kinds in FARTS.items():
        lines += [f"{kind} {sound_id}.ogg" for kind in kinds.split()]
    (out / "farts.txt").write_text("\n".join(lines) + "\n")
    print(f"{len(FARTS)} recordings in {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
