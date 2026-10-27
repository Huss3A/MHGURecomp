# fsr-vulkan (vendored subset)

AMD FidelityFX Super Resolution 3.1.4 **temporal upscaler**, native Vulkan, for MHGU Recomp's Vulkan renderer
(`runtime/nvn/vulkan/fsr.c`).

- Upstream: https://github.com/FireBurn/FSR-Vulkan, commit `c64f093404125e960813f95f1996c14780f0dd69`
  ("Validate finite FSR3 frame generation inputs"), MIT.
- That port pins AMD FidelityFX SDK **v1.1.4** (MIT, Copyright (c) 2024 Advanced Micro Devices, Inc.) with its
  documented Linux / compute-only patches; see `UPSTREAM.md`, `NOTICE.md`, `LICENSE.txt` and
  `upstream/ffx-1.1.4/sdk/LICENSE.txt` (all kept unchanged).
- Shaders: compiled from the GLSL under `upstream/ffx-1.1.4/sdk/src/backends/vk/shaders/fsr3upscaler/` at run time,
  with shaderc, when a permutation is first requested (`src/ffx_fsr3_shaderblobs.cpp`, with the same macros and size
  optimisation as AMD's FidelityFX_SC run; compiled modules are cached on disk by the host). The GLSL and the headers it
  includes are embedded in the executable (`src/ffx_fsr3_glsl.c`). No pre-built SPIR-V, no shader compiler at build time.

## What is kept

| Path | What |
|---|---|
| `include/ffx_vk_portable.h`, `src/ffx_vk_portable.c`, `src/ffx_vk_fsr3_portable.cpp` | the port's public C API (`ffxVkPortableUpscaleContext*`) |
| `src/ffx_fsr3_shaderblobs.cpp`, `src/ffx_fsr3_glsl.{c,h}` | replace AMD's blob accessor: GLSL -> SPIR-V + reflection on demand; the embedded GLSL |
| `upstream/ffx-1.1.4/sdk/include/FidelityFX/host/*`, `.../gpu/{ffx_core*,ffx_common_types,fsr1,spd,fsr3upscaler}` | AMD headers the upscaler host and shaders use |
| `upstream/ffx-1.1.4/sdk/src/{shared,components/fsr3upscaler,backends/shared,backends/vk/ffx_vk.cpp}` | AMD host scheduler + Vulkan backend (upscaler only) |
| `upstream/ffx-1.1.4/CURRENT_SHA256SUMS` | the port's hash manifest filtered to the kept files (56/56 verified) |

## What was dropped

Frame generation / optical flow (host, shaders, generated blobs), FSR 4 v07 provider and assets contract,
the pre-built SPIR-V headers (`generated/`, 200 files, ~19 MB), the SDK 2.3.0 / FSR 3.1.5-3.1.6 scaffold
(`upstream/ffx-2.3.0`, `generated/ffx-2.3.0`), ray-regeneration and
radiance-cache contracts, temporal-lifecycle helper, frame-generation presenter policy, tests, examples,
capability probe, CI, the vulkaninfo JSON, the port's README/PUBLISHING docs and its CMakeLists (replaced by
`CMakeLists.txt` here). No AMD binaries, no FSR 4 weights.

## Local changes

- `src/ffx_vk_fsr3_portable.cpp`: frame-generation code guarded by `FFX_VK_PORTABLE_HAS_FSR3_FRAMEGEN` (off);
  the four `ffxVkPortableFrameGeneration*` entry points return `FFX_VK_PORTABLE_ERROR_UNSUPPORTED`.
- The upstream port's loader shim (for programs that load Vulkan at run time) is not part of this subset; MHGU Recomp
  links the Vulkan loader.
- `CMakeLists.txt` (new): one static library `ffx_vulkan_fsr3_upscaler`.
