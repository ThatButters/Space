# Space — build plan, autumn 2026

Everything from the 14 Sep review, ordered so each phase makes the next one easier to verify.
Effort is working days for one person with the engine already in their head. "Proof" is the
capture that closes the item — nothing is done until that frame exists.

Baseline as of 14 Sep 2026 (commit 6791604): DLSS SR, volumetric clouds, photoclinometric landing
sites, live GIBS clouds + CelesTrak orbits, VR-safe cut tour with Earth-in-sky lunar framing.

## Phase 0 — Foundations (≈3 days)

Small things that every later phase leans on. Do these first so the captures in later phases are
judged with the right exposure and the right tour controls.

| # | Work package | Effort | Proof |
|---|---|---|---|
| ~~0.1~~ | ✅ **Auto-exposure**: log-luminance histogram in `post.comp` (compute, 64 bins), centre-weighted, eye-adaptation curve (bright→dark 3 s, dark→bright 0.6 s), manual EV offset kept as a slider, clamps per stop so the Sun never drives the Moon black. | 1 | Sunrise over the ISS limb blooms then settles; Moon surface and Earth-from-ISS both correctly exposed in one run. |
| ~~0.2~~ | ✅ **Tour strip**: a row of dots along the bottom, current stop lit, names on hover, click to jump; a small solar-system inset ("you are here"). | 1 | Screenshot of any stop shows the strip; clicking Saturn cuts there. |
| ~~0.3~~ | ✅ **Hold and time**: Space holds the visit (timer stops, drift continues); Up/Down scrubs the clock ×1 / ×60 / ×3600 at a stop with no camera rotation; skip-back replays the current stop first. | 0.5 | ISS stop scrubbed into night and back; Moon terminator moved across a crater while the camera stays put. |
| ~~0.4~~ | ✅ **Captions and credits**: caption fades in after the fade-in; date left, blurb centred; hover label suppressed for the current target; a data-credit line at live stops ("clouds: NASA GIBS 14 Sep · ISS: CelesTrak 06:12 UTC"). | 0.5 | ISS frame shows the credit line and no duplicate label. |
| ~~0.5~~ | ✅ **First run and settings**: title card (plays on its own; Space begins at once — a waiting title would stall an unattended demo); F1 split into *Look* (exposure, DLSS, HDR, music) and *Debug*; idle after the last stop loops to the intro. | 0.5 | Fresh launch reaches the title, any key starts; tour wraps. |

Phase 0 closed in the commit after 8da5bb9. Also: orbiter stops scrub at a minute per second (an hour per second
swings the station round its orbit — not VR-safe); lunar sites frame by searching bearing and pitch for Earth high on one
side and the lander low on the other, clear of the strip and caption.

## Phase 1 — Sky and light (≈5 days)

The atmosphere is the weakest link now: an exaggerated single-scatter shell that makes a thick limb
and a flat blue sky over the station. Real photos from orbit are made by multiple scattering and
aerial perspective.

| # | Work package | Effort | Proof |
|---|---|---|---|
| ~~1.1~~ ✅ | ~~**Precomputed atmosphere (Bruneton 2017)**: transmittance (256×64), single + multiple scattering (32×128×32×8), irradiance LUTs baked per atmosphere class (Earth, Mars, Venus, Titan, giants) in compute at startup; real scale heights (8 km / 1.2 km Mie), ozone layer for Earth's blue zenith.~~ Built as Hillaire 2020 instead: transmittance + multiple-scattering LUTs per class, sky ray marched per pixel. | 3 | ISS stop: thin bright limb, correct sky gradient, no blue cast on nadir ground. Side-by-side with an ISS photo of the same geometry. |
| 1.2 🟡 | **Aerial perspective** applied to the ground, the volumetric clouds and spacecraft below ~100 km (transmittance + in-scatter from the LUTs), replacing the per-pass haze hacks. Ground and clouds done; spacecraft still pending. | 1 | Saturn V stop: coast fades blue with distance; rocket stays crisp. |
| 1.3 | **The Sun**: HMI continuum granulation texture, limb darkening from a fit, corona (Baumbach model, streamers from noise) visible when the disc is occluded or off-frame, prominences at the limb, lens flare from a ghost/halo model replacing the streak sprite. Retune HDR peak so the disc is a disc, not a white hole. | 1 | Parker stop: disc with granulation; corona visible past the limb; flare ghosts move opposite the Sun. |

## Phase 2 — Clouds, finished (≈2.5 days)

| # | Work package | Effort | Proof |
|---|---|---|---|
| 2.1 | **Cirrus**: a thin second layer at 9–12 km from the map's faintest cover, anisotropic streaks, mostly forward-scatter. | 0.5 | 300 km capture over the Caribbean shows veils over the cumulus field. |
| 2.2 | **Night clouds**: moonlight (phase-aware) and city-light up-lighting on the underside; lightning flashes moved into the volumetric layer. | 0.5 | Night side over East Asia: clouds faintly lit, cities glowing through thin cover. |
| 2.3 | **Shadows from the volume**: the ground's cloud shadow sampled from the layer's density along the Sun ray (2 taps), not the flat map. | 0.5 | Shadows line up with the volumetric cloud edges at 60 km. |
| 2.4 | **Mid-distance impostor**: a 2 km-res shaded top-view of the layer rendered once per minute into a texture, used from 2,500 km out so the handover to the flat map is invisible. | 1 | Fly from 400 km to 8,000 km: no visible change in the cloud look. |

## Phase 3 — Ground truth (≈4 days)

| # | Work package | Effort | Proof |
|---|---|---|---|
| 3.1 | **Displaced terrain patches**: a camera-centred 1,024² grid mesh over each landing-site patch displaced by DTM + fine relief in the vertex shader, depth-tested against the ray-traced sphere (sphere discarded inside the patch footprint), skirt to the sphere at the edge. | 2 | Apollo 15: Hadley Rille and the Apennine front as real silhouettes; crater rims occlude the ground behind them from 2 m up. |
| 3.2 | **Rocks**: instanced boulders (3 meshes, 5 LODs) scattered by a rock-abundance prior (Jezero: dense; mare: sparse), size-frequency power law, sunk into the ground, contact shadow. | 1 | Perseverance stop: rocks of 10 cm – 1 m around the rover, shadows matching the Sun. |
| 3.3 | **Craft materials**: SSAO in the post pass (depth + reconstructed normals, 8 taps), screen-space reflection of Earth on the ISS arrays and radiators, wear/dust maps on the LM foil, roughness floor tuned per model. | 1 | ISS truss shows contact darkening; array glint follows Earth's position. |

## Phase 4 — Ray tracing (≈6 days)

RTX cores are idle. `VK_KHR_ray_query` from the existing fragment shaders keeps the pipeline
structure; a BLAS per model plus analytic sphere intersections for bodies.

| # | Work package | Effort | Proof |
|---|---|---|---|
| 4.1 | Acceleration structures: BLAS per glTF model at load, TLAS per frame of visible crafts (camera-relative), `rayQueryEXT` in `craft.frag` and `planet.frag`. | 2 | Validation clean; TLAS rebuild < 0.3 ms. |
| 4.2 | **Shadows**: craft-on-ground and craft-on-craft shadows from ray queries (1 ray + 4-sample soft disc for the Sun's 0.5°), replacing the 4096² shadow map; terrain self-shadow from the displaced mesh. | 1.5 | LM shadow crisp at any camera distance; ladder rungs' shadows visible. |
| 4.3 | **Reflections**: ISS arrays and radiators reflecting Earth and each other; Huygens' foil; Parker's shield. | 1.5 | Hubble stop: Earth mirrored in the aperture door. |
| 4.4 | **Exact eclipses**: body–body occlusion by ray against the sphere list (replaces the analytic disc-cover). | 1 | Io's shadow on Jupiter has a penumbra; a lunar eclipse from Tranquility Base darkens the ground correctly. |

## Phase 5 — The tour as a story (≈5 days)

| # | Work package | Effort | Proof |
|---|---|---|---|
| 5.1 | **Script**: `assets/tour/script.json` — per stop: title, date, 20-second narration text, mood tag, dwell seconds, departure style. Written for the ear, checked against the mission record. | 1 | Every stop has a paragraph; read aloud they run 8–12 minutes total. |
| 5.2 | **Voice**: narration pre-rendered to WAV with a neural TTS (Edge/Azure or local Piper) by `scripts/build_narration.py`; the engine gets a WASAPI *render* path (it only captures today); narration ducks the music. | 1.5 | Title → Saturn V plays the first line as the fade-in ends; ducking audible. |
| 5.3 | **Soundtrack**: per-mood tracks (launch, low orbit, lunar silence, outer planets, the Sun, home) crossfaded in the black of the cut; silence at the Moon by design; the existing music-reactivity stays for user-supplied audio. | 1 | Cut ISS → Moon: music fades out over the black; silence with narration at Tranquility Base. |
| 5.4 | **Pacing and shape**: dwell per stop from the script (Moon 40 s, Hubble 15 s); departures for Saturn V and ISS as a straight pull-away that fades; "Highlights" (5 min) and "Full" (25 min) tours selectable at the title. | 1 | Two timings logged; the Saturn V leg ends with a pull-back. |
| 5.5 | **Cut variety** (still VR-safe): caption beat on black, occasional match on the Sun's screen position between stops. | 0.5 | Frame-by-frame: no camera rotation in any cut. |

## Phase 6 — The rest of the system (≈3 days)

| # | Work package | Effort | Proof |
|---|---|---|---|
| 6.1 | **Jupiter and Saturn**: two-deck parallax clouds (Jupiter), Juno-style polar vortices, Saturn's hexagon; ring particles as instanced ice chunks (10 cm – 10 m) inside the ring pass. | 2 | Ring pass: individual chunks drifting past; Jupiter's poles from Juno's stop. |
| 6.2 | **Stars**: Airy PSF from the lens model for the brightest 200, Milky Way dust lanes in the 3D field. | 0.5 | Sirius shows rings at 200 % zoom; the galactic band has structure from the Voyager stop. |
| 6.3 | **Motion blur on orbiters' ground**: per-object velocity for bodies in the motion-vector pass. | 0.5 | ISS ground streaks at high time scale. |

## Phase 7 — VR (≈8 days)

| # | Work package | Effort | Proof |
|---|---|---|---|
| 7.1 | OpenXR session, stereo swapchain, two-eye rendering through the existing camera-relative path (instanced views), DLSS per eye. | 3 | 90 fps in a headset at the title. |
| 7.2 | Head-tracked camera on top of the tour rig (the rig moves, the head looks); UI as a floating panel; captions in world space at 2 m. | 2 | Full tour in a headset without discomfort — the cut rules already exist for this. |
| 7.3 | Controllers: skip stops, hold, time scrub; laser-pointer on the tour strip. | 1 | All Phase 0 controls usable from the headset. |
| 7.4 | Comfort pass: vignette on approach if needed, seated/standing origin, resolution scaling. | 2 | A 25-minute tour run by someone who isn't the developer. |

## Order and dependencies

```
Phase 0 (foundations)
  └─ Phase 1 (atmosphere)  ─┬─ Phase 2 (clouds finish)
                            └─ Phase 3 (ground)  ─ Phase 4 (ray tracing)
Phase 5 (story) can run in parallel with 1–4 (no engine dependency beyond 0.3)
Phase 6 after 1 (needs the exposure and atmosphere baseline)
Phase 7 last (touches every pass)
```

Total ≈ 36 working days. Realistic calendar with evenings and weekends: 10–12 weeks.

## How each item gets closed

1. Build → capture the "proof" frame(s) with `scratchpad/capture_multi.ps1` at 3440×1440 → view it.
2. Commit with the proof frame's name in the message; push.
3. Update this file: strike the row, note the commit.

## Risks

- **Atmosphere LUTs** interact with every pass that fakes haze today (planet rim, atmo shell, cloud fill, dust). Budget a day of removing hacks.
- **Displaced terrain** changes what "on the ground" means for landers and the camera; the double-precision anchor must sample the height field, or landers float.
- **Ray tracing** needs `VK_KHR_acceleration_structure` + `ray_query` and a BLAS per model; the 2.7 M-triangle ISS BLAS is ~150 MB.
- **Narration** quality is a writing problem, not an engineering one; give the script real time.
- **VR** frame budget: two eyes at 90 fps is 3–4× today's load; DLSS Performance per eye and the cloud march at half rate.
