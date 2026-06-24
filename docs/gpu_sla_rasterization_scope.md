# Scope: optional GPU SLA-rasterization prototype (vendored static Vulkan)

**Goal.** Move SLA layer rasterization — the one slicing stage that is a genuine
"all layers at once" GPU fit — onto the GPU via **Vulkan compute**, as an *optional,
runtime-selected, fallback-guarded* feature. Cross-vendor (AMD/NVIDIA/Intel), AGPL-clean,
and self-contained (no installable GPU dependency). FFF is unaffected (it produces
vector toolpaths, not raster masks).

## Why Vulkan, statically vendored

- **Cross-vendor.** One backend runs on AMD (RADV/AMDVLK), NVIDIA, and Intel — not
  NVIDIA-locked like CUDA. Right for a broad slicer user base.
- **AGPL-clean.** Vulkan-Headers and Vulkan-Loader are **Apache-2.0** (one-way
  compatible into AGPL-3.0), so we vendor + statically link them into the AGPL binary
  with no friction. No proprietary toolkit (cf. CUDA's nvcc/cudart). Same category as
  the OpenGL the app already links; only the GPU driver stays a system library.
- **Self-contained.** Static loader + embedded SPIR-V means **no installable
  dependency**: no Vulkan SDK, no `libvulkan.so`, no runtime shader compiler.
- **Runtime decision.** `vkEnumeratePhysicalDevices` + device limits let us choose
  GPU vs CPU *at runtime, per workload*, based on whether the hardware makes it worth it.

### What can and cannot be bundled

| Layer | License | Bundling |
|---|---|---|
| Vulkan-Headers | Apache-2.0 | submodule in `deps_src/`, header-only |
| Vulkan-Loader (`libvulkan`) | Apache-2.0 | submodule, build **static** `libvulkan.a`, link in |
| SPIR-V shaders | ours | compiled at build (glslang), **embedded** as byte arrays |
| ICD / GPU driver | vendor | **never bundled** — `dlopen`'d at runtime by the loader |

The driver is not a shipped dependency: any machine with a usable GPU already has an
ICD. No GPU / no driver / no suitable device → loader returns zero devices → **CPU
(AGG) fallback**. So the binary runs everywhere; the GPU path simply lights up where
the hardware exists and the workload justifies it.

## Why this is the right (and only) GPU target in the slicer

SLA/resin printing needs each layer as a **raster mask** at the panel's native
resolution — regular, data-parallel, fixed-grid work, exactly the GPU rasterizer's
job, and already structured as an embarrassingly-parallel per-layer loop. (FFF's
per-layer work is irregular polygon-boolean / Voronoi-graph computation: GPU-hostile.)

**Cost driver:** SLA panels are 4K (3840×2400 ≈ 9 MP) → 12K (11520×5120 ≈ 59 MP);
prints are hundreds–thousands of layers. Total work ≈ `layers × panel_megapixels`,
dominated by polygon fill + PNG encode — seconds to minutes on high-res panels.

## Current architecture (verified) — the seam

- `SLAPrint::Steps::rasterize()` (`src/libslic3r/SLAPrintSteps.cpp:1049`): per-layer
  lambda does `for (poly : layer.transformed_slices()) raster.draw(poly);`
- Dispatch `m_printer->draw_layers(N, lvlfn, cancel, ex_tbb)` (`SLAPrint.hpp:405`) —
  **already parallel across all layers**; per layer: `create_raster()` → `drawfn` →
  `encode()` → store `EncodedRaster` (PNG bytes).
- `RasterBase` (`SLA/RasterBase.hpp:57`): abstract `draw(ExPolygon)` / `encode()` +
  `Resolution`/`PixelDim`/`Trafo`. `AGGRaster` is the only backend today (CPU AA fill).
- **Drop-in point:** `SLAArchive::create_raster()` is a virtual factory. Returning a
  Vulkan backend changes nothing upstream (slicing, supports, hollowing, elephant-foot)
  or downstream (encode, archive writers).

## Design

### Phase 1 — `VulkanRaster : public sla::RasterBase` drop-in

- One shared `VulkanContext` (instance + logical device + command pool + pipeline +
  embedded SPIR-V), created once, reused across the parallel `draw_layers` loop.
  **Headless: no surface/swapchain** — compute + offscreen storage image only.
- `draw(const ExPolygon&)` accumulates polygons (don't dispatch per-polygon).
- `encode()`: upload polygon/edge buffers → dispatch fill into an R8 storage image at
  `Resolution` → read back → hand bytes to the existing `RasterEncoder` (PNG on CPU
  initially; nvJPEG-style GPU encode is a later option).
- `Trafo` (portrait/mirror/rotation) → a small transform in the shader/push-constants.

**Fill + anti-aliasing.** Two viable kernels; pick by AA fidelity vs simplicity:
1. *Compute even-odd*: per-pixel (or per-tile) edge-crossing parity into the storage
   image; AA via NxN supersample + box downfilter to 8-bit coverage.
2. *Graphics pipeline* (offscreen, still no swapchain): render polygon triangles with
   **stencil even-odd**, MSAA (8–16×), resolve to R8 — uses the hardware rasterizer's
   AA. Likely the closest match to AGG's scanline AA.
SLA needs sub-pixel coverage for dimensional accuracy, so AA must match AGG within a
bounded tolerance (see Validation).

### Phase 2 — batched "all layers at once" (optional, higher ceiling)

If per-layer dispatch/upload overhead dominates, override `draw_layers` for the Vulkan
archive: upload the whole polygon stack once, render N masks into a 2D-array image in
batched dispatches (VRAM-bounded; a 96 GB card holds thousands of 59 MP masks), encode
in parallel. Realizes the full upload-once / amortized-launch win.

## Runtime GPU-vs-CPU selection policy (the "is it worth it" gate)

At startup (or first SLA raster), enumerate devices and decide dynamically:

```
pick_backend(resolution, layer_count):
    if VULKAN disabled by config/env            -> AGG
    if loader missing or 0 physical devices     -> AGG          # no driver/ICD
    dev = best device (prefer DISCRETE_GPU; else INTEGRATED)
    if dev.maxImageDimension2D < panel dims     -> AGG          # can't fit a mask
    if dev.local VRAM < working-set estimate    -> AGG
    work = resolution.megapixels * layer_count
    if work < THRESHOLD (calibrated)            -> AGG          # GPU+transfer not worth it
    else                                        -> Vulkan(dev)
```

THRESHOLD is calibrated from the benchmark (below): small/low-res jobs stay on CPU
where launch+transfer overhead would dominate; large/high-res jobs go to GPU. Decision
is per-run and hardware-aware — integrated GPUs and weak cards self-exclude.

## Dependencies & build (static, vendored)

- `deps_src/Vulkan-Headers` (submodule, Apache-2.0) — interface target.
- `deps_src/Vulkan-Loader` (submodule, Apache-2.0) — built **static** (`libvulkan.a`).
  Note: the static loader still does runtime ICD discovery (reads
  `/usr/share/vulkan/icd.d/*.json`, `dlopen`s the driver) — static-linking the loader
  removes the *loader* dependency, not the driver, by design.
- Shaders: `*.comp`/`*.vert`/`*.frag` compiled to SPIR-V at build via vendored
  **glslang** (BSD/Apache) and **embedded** as `const uint32_t[]` — no runtime compiler.
- CMake: `option(SLIC3R_VULKAN ... OFF)`; a `libslic3r_sla_gpu` static lib (Vulkan
  backend + embedded SPIR-V) linked PRIVATE into libslic3r. `create_raster()` selects
  Vulkan vs AGG at runtime via the policy above. Never a hard dependency; OFF builds
  and non-GPU machines are byte-for-byte unaffected on the slicing result.

## Validation

1. **Pixel parity vs AGG.** For a set of SLA models, rasterize with AGG and Vulkan;
   decode both `EncodedRaster`s and diff. Require exact match on fully-covered/empty
   pixels and a bounded AA delta on edge pixels (e.g. ≤1–2 grayscale levels on <X% of
   edge pixels). Gate the feature on passing.
2. **Round-trip.** Slice to a full `.sl1` archive with each backend; confirm it loads
   and layer count/metadata match.
3. **Determinism.** Same input → same mask across runs/devices (within AA tolerance).

## Benchmark

`rasterize()` is the discrete `slapsRasterize` step, independently timeable. Build an
SLA analogue of `bench/slice_bench.sh`:
- A shipped SLA machine profile (panel `display_pixels_x/y`) + a couple of models.
- Slice in SLA mode; measure the rasterize step wall-time, min-of-N, pinned.
- **Sweep panel resolution (4K→8K→12K) × layer count** — GPU advantage grows with
  panel megapixels; report the curve and use it to calibrate the selection THRESHOLD.

## Effort / risk / ROI

- **Effort:** medium(+). Vulkan boilerplate (instance/device/pipeline/descriptors) is
  more verbose than CUDA, but headless-clean and one-time. Bulk = the VulkanContext +
  fill shader + AA matching + static vendoring + selection policy + validation harness.
  ~2 weeks to a measured Phase-1 prototype.
- **Risk:** medium. Matching AGG's anti-aliased coverage closely enough is the main
  correctness risk; static-loader build config and pixel parity are the rest. All
  contained behind the RasterBase factory with a CPU fallback.
- **ROI:** real but **scoped to SLA**. Big win for high-res panels / tall prints; **zero**
  for FFF (path unused). Honest framing: the one branch where GPU "all layers at once"
  pays — cross-vendor and AGPL-clean — not a general slicer speedup.

## This host

Vulkan **1.3 loader present** (`libvulkan.so.1.3.204`); GPUs are NVIDIA (3090 + 2×
Blackwell) with driver 580 (Vulkan-capable). Missing only dev tooling (headers, shader
compiler) — which we vendor anyway. So Phase 1 is testable here once the submodules
land. AMD validation needs an AMD/RADV machine (or lavapipe software ICD for CI).

## Phased task breakdown

1. SLA benchmark harness + AGG baseline rasterize-step timing across panel sizes.
2. Vendor `Vulkan-Headers` + `Vulkan-Loader` (static) + glslang submodules; `SLIC3R_VULKAN`
   gate; `VulkanRaster : RasterBase` skeleton returning AGG output (prove the seam + build).
3. Headless `VulkanContext` + compute/offscreen fill shader → R8 mask → readback.
4. AA matching + pixel-parity validation vs AGG; gate the feature on it.
5. Runtime selection policy + THRESHOLD calibration; benchmark; decide on Phase 2.
