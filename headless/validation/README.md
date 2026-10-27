# Linux Ada / Blackwell validation

Code tested: `3063c5c45c6295e2ad0390409c5fad37c52faee6`.

| GPU | Compute capability | CUDA / NVRTC | CTest | Three-stage fixture |
|---|---|---|---|---|
| GeForce RTX4090 |8.9|13.0|3/3 pass|pass|
| RTX PRO6000 Blackwell Workstation Edition |12.0|13.2|3/3 pass|pass|

The complete512x512 synthetic packed visibility buffers are **bit-identical** between GPUs: same background, triangle IDs and packed depths. See `cross-gpu.json` for the matching SHA256 hashes. Both report49,038 covered pixels,2 triangles entering the medium queue and36 huge tiles. This exercises all three raster stages.

Real static avatars rendered successfully at512 and1024 on both devices. The real-asset builds differ (Ada336,734 triangles; Blackwell302,218), so these timings must not be interpreted as paired hardware speedups. CUDA-event medians exclude parse, IO, compile, GPU uploads, framebuffer clear and shading.

| Workload | RTX4090 median | RTX PRO6000 Blackwell median |
|---|---:|---:|
|Same synthetic fixture512|0.039584ms|0.038048ms|
|Different real avatar builds512|0.024512ms|0.021216ms|
|Different real avatar builds1024|0.026592ms|0.022400ms|

These sub-millisecond tests are small; scheduling/launch overhead affects results. They do not measure a full rendered frame or an avatar pipeline.

Drivers exposed CUDA driver API13.2 on both machines. No driver/toolkit upgrade or Python-environment change was needed. Each run selected the device architecture through NVRTC. No source patch to the raster algorithm was needed for architecture support; the fork adds guarded headless queue handling.

The headless runner is an integration milestone. The original Windows/Vulkan desktop viewer, full PBR/alpha behavior, skinning, morph playback, animation and differentiable fitting are outside this validation. Prior nvdiffrast comparisons found non-identical visibility assignments, so matching across these GPUs does not establish parity with another rasterizer.
