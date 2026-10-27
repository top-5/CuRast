# Linux CUDA visibility runner

This fork adds a **headless opaque-geometry visibility target**, independent of the Windows/Vulkan desktop viewer. It is useful for Linux pipeline integration and architecture checks. It does not implement the viewer's texturing, alpha/translucency, skinning, animation or morph playback. Do not treat its triangle-ID visualization as a photorealistic rendering.

The existing desktop target is preserved, but its full Linux port is unfinished. `MappedFile.h`, platform-specific IO and viewer dependencies still require work.

## Build

Requirements: Linux, CMake3.24+, C++20 compiler, CUDA Toolkit13.1 recommended (13.0 minimum) with NVRTC/CCCL, and a compatible NVIDIA driver. CUDA13.0 is sufficient for Ada; Blackwell requires a toolkit supporting the installed GPU's compute capability. No display server, GLFW, Vulkan, TurboJPEG, NVCC device compilation or Windows library is needed for this target.

```sh
cmake -S . -B build-linux \
  -DCMAKE_BUILD_TYPE=Release \
  -DCURAST_HEADLESS_ONLY=ON \
  -DCURAST_GPU_TESTS=ON \
  -DCUDAToolkit_ROOT=/usr/local/cuda-13.1
cmake --build build-linux -j4
ctest --test-dir build-linux --output-on-failure
```

Select the installed toolkit path on each machine. The C++ host executable compiles CuRast's existing visibility kernel at runtime with NVRTC, automatically choosing the device's compute capability (no hard-coded Ada/Blackwell architecture).

CMake fetches a pinned fastgltf commit and that project's pinned simdjson/simdutf dependencies. To reuse an existing checkout, add `-DFETCHCONTENT_SOURCE_DIR_FASTGLTF=/absolute/path/to/fastgltf`. This avoids a duplicate clone and supports cached/offline builds when dependencies are already present.

On Linux, the headless target is the default. `-DCURAST_HEADLESS_ONLY=OFF` selects the original desktop build and its existing platform/toolkit requirements.

## Run

```sh
build-linux/headless/curast-headless --self-test --resolution 512 --repeat 31
```

Use a space between flag and value:

```sh
build-linux/headless/curast-headless --self-test --resolution 512 --repeat 31 \
  --report self-test.json
build-linux/headless/curast-headless --input avatar.glb --resolution 1024 \
  --repeat 31 --output visibility.ppm --dump visibility.bin --report avatar.json
```

The runner reads static triangle primitives in the selected scene, retaining node transforms. Double-sided materials append reversed triangles explicitly, maintaining a source-bound primitive range. POSITION bounds determine the camera; the per-vertex transforms execute in the original GPU rasterizer. fastgltf handles indexed/non-indexed and interleaved/normalized accessors. Unknown required extensions are rejected by the parser. Skinning, morph targets, animations and non-opaque material inputs are rejected explicitly. Existing GLB files are never exported or rewritten.

The PPM visualizes triangle IDs, not materials. `--frames 60` renders a complete turntable with one resident CUDA allocation/NVRTC module; `--azimuth 30` changes the initial view. JSON cameras contain row-major world-to-clip matrices. The binary dump concatenates those frames, each row-major bottom-up packed uint64 visibility, using the original `pack_pixel()` layout. Its empty value is `0xfffffff000000000`, matching `resolve.cu:kernel_clearFramebuffer`. The low33 bits contain the cumulative triangle ID; the upper bits hold packed positive view-space depth. Preserve primitive ranges to map IDs back to the source.

## Correctness and timing

The self-test contains small, medium and large screen-space triangles. It checks background and known interior triangle IDs, requires a nonempty medium queue, and requires huge-triangle tiles. This exercises all three stages instead of testing only dense microtriangles. Tests create a CUDA context; enable `CURAST_GPU_TESTS` only on GPU-equipped hosts. Reserve the GPU through the host's normal scheduling/lock mechanism before running tests.

JSON reports include GPU model, compute capability, driver/runtime/NVRTC versions, configured Git revision, resolution, triangle count, queue counts and timing scope. Two warmups precede the requested repetitions. CUDA-event times include **only the three raster stages**, excluding IO/parsing, GPU uploads, framebuffer clear, JIT compilation, shading, barycentric reconstruction and gradients. They are not full-frame or pipeline timings.

Headless-only queue guards bound huge-tile writes/reads and reject incomplete output if capacity is exceeded. The desktop path is unchanged. A conservative input bound prevents tile-counter overflow.

Passing the fixture establishes portability of the tested core, not complete glTF material/rendering correctness. Cross-renderer comparisons must separately check silhouette, depth and source-triangle ownership. Prior research found small mask differences and non-identical visible IDs against nvdiffrast; this is not a drop-in differentiable fitting backend.

## Licensing

The integration and included CuRast kernels follow the upstream AGPL-3.0 terms. Dependencies retain their own licenses (fastgltf, simdjson/simdutf, GLM and nlohmann/json). No avatar assets are distributed with this fork.

## Verified devices

The Linux target has been built and executed on GeForce RTX4090 (Ada,8.9,CUDA13.0) and RTX PRO6000 Blackwell Workstation Edition (12.0,CUDA13.2). Both passed all three CTest cases and rendered real avatars at512/1024. The complete synthetic three-stage visibility frame was bit-identical across architectures. See [validation/README.md](validation/README.md) and its JSON receipts for exact scope and timing limits.

## Python review adapter and cache

Seed's `python -m seed.render.review input.glb --out <new-directory>` uses nvdiffrast by default (CuRast is optional after slower end-to-end measurements) and adds Torch-CUDA perspective-correct base-colour/clay shading plus PNG/MP4 output. `--backend curast` selects this comparison backend. Static opaque GLBs only, not animated rig playback or full PBR.

The adapter supplies `SEED_CURAST_PTX_CACHE` keyed on the executable, kernel/header content, native toolkit version and target architecture. Successful PTX is written atomically; incompatible/corrupt PTX recompiles. Standalone callers must provide their own valid content-addressed key or leave caching unset. JSON records `ptx_cache_hit` separately from compile time.
