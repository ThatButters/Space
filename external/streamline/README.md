# NVIDIA Streamline (DLSS) SDK drop-in

The build looks for the Streamline SDK here and enables `SPACE_HAS_STREAMLINE` when it finds `include/sl.h`.
Nothing in this folder except this README is tracked by git.

## Getting the SDK

Streamline is open source: https://github.com/NVIDIA-RTX/Streamline

1. Download the latest release (2.14.x as of September 2026) or clone and build it.
2. Copy so the layout is:

```
external/streamline/
  include/        sl.h, sl_dlss.h, sl_dlss_g.h, sl_dlss_d.h, sl_reflex.h, ...
  lib/x64/        sl.interposer.lib
  bin/x64/        sl.interposer.dll, sl.common.dll, sl.dlss.dll, sl.dlss_g.dll, nvngx_dlss.dll, nvngx_dlssg.dll, ...
```

3. Re-run CMake. The DLLs are copied next to `Space.exe` after each build.

## What the integration will provide (milestone 5)

- DLSS Super Resolution: render the HDR scene at a lower internal resolution and let the transformer model
  reconstruct it. Requires motion vectors and depth from the star/planet passes, plus jittered projection.
- DLSS Multi Frame Generation (RTX 50 series): up to 3 generated frames per rendered frame.
- Ray Reconstruction: once planets are ray traced.
- Reflex: latency reduction, required by Frame Generation.

DLSS 5 (3D-Guided Neural Rendering) is not yet in the public SDK. When it lands, it plugs into the same
Streamline feature list and will be enabled only while the camera is near a planet surface.
