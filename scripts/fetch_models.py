#!/usr/bin/env python3
"""Download spacecraft models from NASA's public-domain sources into assets/models. Only needs the standard library.

- glTF binaries from NASA's 3D resource library (https://github.com/nasa/NASA-3D-Resources); these are the
  baseline models the app always has.
- Higher-quality versions kept alongside them (the baseline files are never touched and stay as fallbacks):
  the HD ISS from the same library, and multi-file glTF packages from NASA's Eyes on the Solar System, each in
  its own folder with the .gltf plus every referenced .bin and image."""

import json
import posixpath
import sys
import urllib.parse
import urllib.request
from pathlib import Path

BASE = "https://raw.githubusercontent.com/nasa/NASA-3D-Resources/master/3D%20Models/"
MODELS = {
    "apollo_lm.glb": "Apollo Lunar Module/Apollo Lunar Module.glb",
    "perseverance.glb": "Mars 2020 Perseverance Rover/Mars 2020 Perseverance Rover.glb",
    "ingenuity.glb": "Ingenuity Mars Helicopter/Ingenuity Mars Helicopter.glb",
    "viking.glb": "Viking Lander/Viking Lander.glb",
    "insight.glb": "InSight Cruise Lander/InSight Cruise Lander (arm deployed).glb",
    "huygens.glb": "Cassini-Huygens (A)/Cassini-Huygens (A) (without Cassini).glb",
    "iss.glb": "International Space Station (ISS) (B)/International Space Station (ISS) (B).glb",
    "hubble.glb": "Hubble Space Telescope (A)/Hubble Space Telescope (A).glb",
    "jwst.glb": "James Webb Space Telescope (A)/James Webb Space Telescope (A).glb",
    "soho.glb": "Solar and Heliospheric Observatory/Solar and Heliospheric Observatory.glb",
    "lro.glb": "Lunar Reconnaissance Orbiter (A)/Lunar Reconnaissance Orbiter (A).glb",
    "mro.glb": "Mars Reconnaissance Orbiter (MRO) (A)/Mars Reconnaissance Orbiter (MRO) (A).glb",
    "juno.glb": "Juno (A)/Juno (A).glb",
    "parker.glb": "Parker Solar Probe/Parker Solar Probe.glb",
    "pioneer10.glb": "Pioneer 10/Pioneer 10.glb",
    "voyager.glb": "Voyager Probe (B)/Voyager Probe (B).glb",
    "saturn_v.glb": "Saturn V/Saturn V.glb",
    "apollo_soyuz.glb": "Apollo Soyuz/Apollo Soyuz.glb",
    # Higher-quality replacements (loaded in preference to the files above when present).
    "iss_hd.glb": "International Space Station (ISS) (D) (IGOAL)/International Space Station (ISS).glb",
}

EYES = "https://eyes.nasa.gov/assets/static/models/"
GLTF_PACKAGES = {
    "lro_eyes": "sc_lunar_reconnaissance_orbiter/LRO.gltf",
    "soho_eyes": "sc_soho/soho.gltf",
    "juno_eyes": "sc_juno/Juno.gltf",
    "hubble_eyes": "sc_hubble/Hubble.gltf",
    "voyager_eyes": "sc_voyager/Voyager.gltf",
    "pioneer_eyes": "sc_pioneer/pioneer.gltf",
    "parker_eyes": "sc_parker_solar_probe/PSP.gltf",
}

HEADERS = {"User-Agent": "Mozilla/5.0 (Space explorer)"}


def fetch(url):
    req = urllib.request.Request(url, headers=HEADERS)
    with urllib.request.urlopen(req, timeout=300) as r:
        return r.read()


def fetch_glb(name, rel, out):
    dst = out / name
    if dst.exists() and dst.stat().st_size > 10_000:
        print(f"  have {name}")
        return
    data = fetch(BASE + urllib.parse.quote(rel))
    if data[:4] != b"glTF":
        raise RuntimeError("not a GLB")
    tmp = dst.with_suffix(dst.suffix + ".part")
    tmp.write_bytes(data)
    tmp.replace(dst)
    print(f"  ok   {name} {len(data) / 1e6:.1f} MB")


def referenced_files(gltf):
    """Relative (decoded) paths of every external buffer and image a glTF references; data: URIs are skipped."""
    rels = []
    for item in gltf.get("buffers", []) + gltf.get("images", []):
        uri = item.get("uri")
        if not uri or uri.startswith("data:"):
            continue
        rel = posixpath.normpath(urllib.parse.unquote(uri).replace("\\", "/"))
        if rel.startswith("../") or rel == ".." or rel.startswith("/") or ":" in rel:
            raise RuntimeError(f"refusing unsafe uri {uri!r}")
        if rel not in rels:
            rels.append(rel)
    return rels


def fetch_gltf_package(folder, rel, out):
    url = EYES + rel
    dst_dir = out / folder
    gltf_name = posixpath.basename(rel)
    gltf_path = dst_dir / gltf_name
    if gltf_path.exists():
        gltf = json.loads(gltf_path.read_bytes())
        if all((dst_dir / f).exists() and (dst_dir / f).stat().st_size > 0 for f in referenced_files(gltf)):
            print(f"  have {folder}/{gltf_name}")
            return
    raw = fetch(url)
    gltf = json.loads(raw)
    dst_dir.mkdir(parents=True, exist_ok=True)
    total = len(raw)
    for f in referenced_files(gltf):
        dst = dst_dir / f
        if dst.exists() and dst.stat().st_size > 0:
            total += dst.stat().st_size
            continue
        data = fetch(urllib.parse.urljoin(url, urllib.parse.quote(f)))
        dst.parent.mkdir(parents=True, exist_ok=True)
        tmp = dst.with_name(dst.name + ".part")
        tmp.write_bytes(data)
        tmp.replace(dst)
        total += len(data)
    # The .gltf is written last so a present .gltf implies a complete package.
    gltf_path.write_bytes(raw)
    print(f"  ok   {folder}/{gltf_name} {total / 1e6:.1f} MB ({len(referenced_files(gltf))} files)")


def main():
    out = Path(__file__).resolve().parent.parent / "assets" / "models"
    out.mkdir(parents=True, exist_ok=True)
    failed = []
    jobs = [(name, fetch_glb, name, rel) for name, rel in MODELS.items()]
    jobs += [(folder, fetch_gltf_package, folder, rel) for folder, rel in GLTF_PACKAGES.items()]
    for label, fn, key, rel in jobs:
        try:
            fn(key, rel, out)
        except Exception as e:
            print(f"  FAIL {label}: {e}")
            failed.append(label)
    if failed:
        print("failed:", ", ".join(failed))
        sys.exit(1)
    print("done")


if __name__ == "__main__":
    main()
