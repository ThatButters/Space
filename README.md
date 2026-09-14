# Space

A galaxy/universe explorer built on Vulkan 1.3 for RTX 50-series hardware. Sit back, play some music, fly.

## Requirements

- Windows 11, Visual Studio 2022 Build Tools (MSVC, CMake, Ninja)
- [Vulkan SDK](https://vulkan.lunarg.com/) 1.4+ (`winget install KhronosGroup.VulkanSDK`)
- [vcpkg](https://github.com/microsoft/vcpkg) checked out at `..\vcpkg` or pointed to by `VCPKG_ROOT`

## Build and run

```
scripts\build.cmd release --run
scripts\build.cmd debug          (validation layers on, console attached)
```

The first build downloads and compiles dependencies through vcpkg (a few minutes).
Output lands in `build\<preset>\Space.exe` with `shaders\` and `assets\` beside it.

## Real star data

The engine looks for `assets\gaia\gaia.stars` next to the executable and falls back to a procedural
galaxy if it is missing. To build the catalogue (2.49 million stars, Gaia DR3 distances):

```
curl -LO https://raw.githubusercontent.com/astronexus/ATHYG-Database/main/data/athyg_v32-1.csv.gz
curl -LO https://raw.githubusercontent.com/astronexus/ATHYG-Database/main/data/athyg_v32-2.csv.gz
python scripts\convert_athyg.py athyg_v32-1.csv.gz athyg_v32-2.csv.gz
```

`scripts\fetch_gaia.py` queries the ESA Gaia archive directly for a larger, deeper subset, but the
archive's async service is often slow or unavailable, so AT-HYG is the reliable default.
The catalogue is git-ignored (80 MB).

## Controls

| Input | Action |
|---|---|
| Right mouse (hold) | Look around (during the tour: look freely, the view eases back when released) |
| W A S D | Fly forward / left / back / right |
| R / F (or Space / Ctrl) | Up / down |
| Q / E | Roll |
| Scroll | Change speed (exponential) |
| Shift | 5x boost |
| 0-9 | Go to Sun, Mercury, Venus, Earth, Moon, Mars, Jupiter, Saturn, Uranus, Neptune (not during the tour) |
| T | Autopilot tour on / off (the only key that stops it; flight keys wait until it is off) |
| F1 | Toggle UI |
| Esc | Quit |

## Music

Whatever is playing on the default output device is captured (WASAPI loopback, nothing is recorded)
and analysed live into 32 bands. Nothing is triggered per beat: everything is continuous and slow, built
to hold up over an hour. The bands are mapped to directions in space (bass below you, highs above, mids
around the plane), so each region of sky breathes with its own part of the mix and the music wraps around
you. Phrase-scale energy (this passage against the last half minute) sets dust density, glow, and how rich
and warm the mood colour is; the colour itself follows the spectral tilt from ember through violet to ice.
Bass widens the bloom, highs cool the galactic glow. "swell waves" (off by default) adds a slow spherical
tide from your position on sustained energy rises, at most every seven seconds.

## Constellations

```
python scripts\fetch_constellations.py
```

Fetches the 88 IAU figures (Stellarium's modern sky culture, CC BY-SA 4.0). In the UI, open
"constellations", type a name, click it: the figure lights up and the camera swings onto it.
Figures are drawn between the real catalogue stars, so they only look right from near the Sun.

You start in orbit around Earth with the planets where they really are today (JPL approximate
elements). The camera rides along with the nearest body, so the time-scale slider moves the worlds
without leaving you behind.

## Spacecraft models

```
python scriptsetch_models.py
```

Public-domain NASA models: the NASA-3D-Resources GLBs plus higher-quality versions where NASA publishes
them. That includes the 2.7M-triangle ISS with 4K textures and the Eyes on the Solar System glTF packages for
Hubble, LRO, SOHO, Juno, Voyager, Pioneer and Parker, which have normal and roughness maps. Missing upgrades
fall back to the original GLBs. Craft are shaded with glTF metallic-roughness PBR.

## Planet surface maps

```
python scripts\fetch_textures.py
```

Downloads ~57 MB of equirectangular maps (NASA / USGS imagery packaged by Solar System Scope, CC BY 4.0)
into `assets\textures`. Bodies without a map fall back to procedural surfaces. Git-ignored.

### High-resolution Earth

```
python scripts\bake_earth_hires.py
```

Fetches NASA's Blue Marble Next Generation (86400x43200, 500 m/px, with topography and bathymetry)
as eight 21600² tiles plus the 2016 Black Marble night map (~420 MB), and bakes them with `TexBake`
(built alongside the app, DirectXTex on the GPU) into mipmapped BC7 DDS tiles at 16384² each, about
3 GB of VRAM. Takes ~5 minutes once. When the tiles are present Earth is sampled from the tile grid;
otherwise the 8K map is used. Assets are read from the source tree, so nothing is copied per build.

### The Moon

```
python scriptsake_moon_hires.py
```

Fetches NASA's CGI Moon Kit (LRO: the LROC WAC colour mosaic and LOLA elevation, ~1.25 GB) and bakes
a 16K BC7 colour map, a 23K BC5 normal map derived from the 64 px/deg elevation model, and a 16-bit
height map. The Moon is lit with a Lunar-Lambert model, and craters and mountains cast real shadows by
marching the height map toward the Sun. Its orbit comes from Meeus' lunar theory and it is tidally
locked with real libration, so the phase you see is tonight's phase.

Up close, where the global maps run out (~470 m per pixel), procedural regolith takes over: craters
from 256 m down to 25 cm, anchored in double precision so they stay put under your feet. Planet
surfaces are ray-traced spheres, so landers sit exactly on the ground.

### Calm clock

Near a planet or moon, or beside a spacecraft, simulated time is held close to real time (the UI says so),
so taking the controls never leaves you riding a station that laps Earth every three seconds. Far from
everything the time-scale slider applies as set. Untick "calm clock near worlds and craft" to override.

### Planets and moons

```
python scriptsake_planets_hires.py
```

Downloads USGS / NASA global mosaics and bakes them the same way (about 8 GB of sources, kept for re-runs):
Mars (Viking colour 925 m + MOLA elevation for relief and terrain shadows), Mercury (MESSENGER basemap +
DEM), Io, Europa, Ganymede, Callisto (Galileo / Voyager), Titan and Rhea (Cassini), Triton (Voyager 2),
Phobos and Deimos. Bodies without a baked map keep the 8K maps or their procedural surfaces.

### Apollo landing sites up close

```
python scriptsake_apollo_sites.py
```

Turns the LROC NAC terrain products (2 m elevation models and 50 cm orthophotos, downloaded into
`assets	exturespollo_sites\<site>`) into local patches around each lander: real terrain relief and
shadows, and the orthophoto (with its own lighting divided out and the lander painted out) as the ground
colour. The global map blends into the patch at its edges; procedural regolith only adds what is finer
than the data.

### Landing sites

Surface stops happen in local morning light. The tour runs the clock forward as a time-lapse,
and "go to" jumps it. Each Apollo site has its Lunar Flag Assembly beside the lander. Spacecraft cast
shadows (4096² sun-aligned depth map) on the ground and on themselves.

## DLSS

With the NGX SDK dropped into `external/dlss` (see its README) and an RTX GPU, the scene renders at DLSS's
internal resolution (Quality mode: 2/3 of the display) with the same sub-pixel jitter the TAA used, a
depth-derived motion-vector pass feeds DLSS, and it reconstructs the display-size image that bloom and
tonemapping consume. The F1 panel has the toggle and quality (performance .. DLAA); `--no-dlss` and
`--dlss N` (0-4) from the command line. Without the SDK the engine's own TAA is used.

## HDR

On an HDR display the swapchain is scRGB (RGBA16F) and the Sun, bright stars and bloom use the
panel's real headroom. Paper white and peak nits are in the UI. `--sdr` forces the sRGB path.

## Command-line switches

```
--goto <Body|Craft>    start at a body or spacecraft (e.g. --goto Saturn, --goto ISS)
--surface <Body> <lat> <lon> <altKm>   start above a surface point (e.g. --surface Earth 40 -105 400)
--tour                 start the autopilot tour (--tour-pace <x> speeds it up)
--size <w> <h>         window size
--sdr                  SDR swapchain even on an HDR display
--time-scale <days/s>  simulated days per real second (default 0.02)
--no-stars --no-bodies --no-volumetrics --no-sky --no-bloom
                       isolate passes
--diag                 dump HDR / bloom / swapchain frames (PPM) at fixed frames
--diag-time <s>        dump once at a wall-clock time
```

## Layout

```
src/core     window, input, logging
src/gfx      Vulkan context, swapchain, images, pipelines (dynamic rendering, sync2, VMA)
src/render   frame orchestration: sky -> HDR -> tonemap -> UI
src/scene    camera (double precision, quaternion)
src/app      main loop and debug UI
shaders/     GLSL, compiled to SPIR-V by glslc at build time
external/    NVIDIA DLSS SDK drop-in (see external/dlss/README.md)
docs/        ROADMAP.md
```

See `docs/ROADMAP.md` for what comes next.
