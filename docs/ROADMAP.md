# Space: roadmap

A GPU galaxy/universe explorer for sitting back, listening to music, and flying through something beautiful.
Target hardware: RTX 50 series (Blackwell), Vulkan 1.3, Windows 11.

## Milestone 1: engine skeleton (this scaffold)

- [x] Window, Vulkan 1.3 device with dynamic rendering + synchronization2, VMA
- [x] HDR RGBA16F target -> ACES tonemap -> sRGB swapchain
- [x] Free-fly 6-DOF camera with double-precision position
- [x] Procedural sky pass (layered star field, nebula, galactic band) as a placeholder
- [x] ImGui overlay: fps, GPU timing, exposure, density controls
- [x] Shader hot-compile on build (glslc), self-contained exe directory

## Milestone 2: real stars

- [x] Star catalogue pipeline: AT-HYG v3.2 (Gaia DR3 + Hipparcos + Tycho, 2.49M stars) via
      `scripts/convert_athyg.py`; direct ESA archive fetch in `scripts/fetch_gaia.py` for when the
      archive's async TAP is healthy. Positions in parsecs, galactic frame, Sun at origin.
- [x] Packed 32-byte GPU star record (position, luminosity, blackbody colour)
- [x] Instanced point-sprite stars: energy-conserving Gaussian PSF, flux-driven radius, additive HDR
- [x] Camera-relative rendering (rotation-only view matrix, per-star camera subtraction)
- [x] Procedural spiral galaxy as a fallback catalogue (toggle in the UI)
- [x] Physically based bloom: 13-tap Karis downsample + tent upsample compute chain, soft knee
- [ ] Auto-exposure (histogram-based, with a manual override)
- [ ] Logarithmic depth buffer (needed once planets add opaque geometry)
- [ ] GPU frustum culling / LOD so 10M+ stars stay cheap

## Milestone 3: nebulae and galaxy

- [x] Raymarched galactic medium: exponential disc + bulge glow, two-arm log-spiral dust lanes with
      LOD-faded fbm, log-spaced steps to 26 kpc, per-pixel jitter
- [x] 16 real nebulae at catalogued positions (Orion, Carina, Lagoon, Eagle, Rosette, Veil, Pleiades,
      Rho Oph, Coalsack, ...) with emission / reflection / dark types, composited in depth order
- [x] Native HDR output (scRGB) on HDR displays with paper-white / peak controls; SDR fallback
- [ ] Half-resolution volumetrics with temporal reprojection (currently full res, ~5 ms at 1600x900)
- [ ] Per-star dust extinction (stars behind dark clouds should dim)

## Milestone 4: solar system and audio

- [x] Sun, 8 planets, 12 moons with real radii, JPL approximate Keplerian elements (positions for the
      current date), circular moon orbits, axial tilt + spin, Saturn's rings, reversed-Z depth
- [x] Camera rides with the nearest body so simulated time does not leave it behind; go-to on 0-9
- [x] Surface maps for the Sun, planets, Moon and Saturn's rings (NASA / USGS data via Solar System
      Scope's equirectangular packs, 8k where available), mipmapped, seam-safe sampling, Earth night
      lights and cloud layer. `scripts/fetch_textures.py` downloads them.
- [ ] Maps for the large moons (Io, Europa, Ganymede, Callisto, Titan, Triton) from USGS mosaics
- [ ] Normal / height maps for relief at close range
- [x] Atmosphere shells: single-scattering Rayleigh + Mie integrated through a thin shell (Earth,
      Venus, Mars, Titan, the giants), premultiplied over the surface; 12 view / 4 sun samples
- [x] Interplanetary dust motes: procedural field wrapped around the camera, sized to speed, streaked
- [ ] Bruneton-style precomputed LUTs for multiple scattering and camera-inside-atmosphere views
- [ ] Planet shadows on rings / moons, eclipse shadows
- [ ] JPL Horizons ephemerides for exact moon positions; NASA SPICE kernels for spacecraft
- [x] WASAPI loopback audio capture -> 2048-point FFT -> 32 auto-gained bands, beat detection;
      bass drives bloom, mids the nebulae, highs the stars, beats a small exposure kick
- [x] Cinematic autopilot (T): eased flights between bodies, slow sunlit-side orbits, manual input
      takes over instantly
- [x] Constellations: 88 IAU figures from Hipparcos ids, searchable list, highlight, finder that swings
      the camera onto the figure, on-screen labels
- [x] Beats as spherical shockwaves through space (dust field, then nearby stars, then a faint angular
      ripple on the far sky); mood colour from the spectral centroid; eased screen-wide effects
- [ ] VR (OpenXR): stereo swapchain, head-tracked camera, the spherical wave already works in 3D
- [ ] Spline paths through nebulae and star clusters for the tour (currently solar-system bodies only)
- [ ] Per-band star twinkle and nebula colour shifts driven by the spectrum

## Milestone 5: NVIDIA stack

- DLSS Super Resolution: done (NGX, Vulkan). Next: Multi Frame Generation + Reflex
- Vulkan ray tracing for planet shadows and reflections; DLSS Ray Reconstruction
- DLSS 5 (3D-Guided Neural Rendering) near planet surfaces once the SDK is public
