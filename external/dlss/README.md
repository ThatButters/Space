# NVIDIA DLSS (NGX SDK) drop-in

The build looks for the DLSS SDK here and defines `SPACE_HAS_DLSS` when it finds `include/nvsdk_ngx_vk.h`
and `lib/nvsdk_ngx_d.lib`. Nothing in this folder except this README is tracked by git.

## Getting the SDK

The SDK is public: https://github.com/NVIDIA/DLSS (read its LICENSE.txt; the runtime DLL may be
redistributed with the application).

Copy so the layout is:

```
external/dlss/
  include/    nvsdk_ngx*.h            (from DLSS/include)
  lib/        nvsdk_ngx_d.lib         (from DLSS/lib/Windows_x86_64/x64, dynamic CRT)
              nvsdk_ngx_d_dbg.lib     (same folder, for Debug builds)
              nvngx_dlss.dll          (from DLSS/lib/Windows_x86_64/rel; copied next to Space.exe)
  LICENSE.txt
```

## What it does in Space

DLSS Super Resolution replaces the engine's TAA: the HDR scene is rendered at the internal resolution
DLSS asks for (Quality mode by default), with the same Halton jitter, plus a depth-derived motion vector
pass, and DLSS reconstructs the display-resolution image that bloom and tonemapping then consume.
Toggle and quality live in the F1 panel. Without the SDK (or on a non-RTX GPU) the engine falls back to TAA.
