#ifndef SLA_GPU_SLABACKENDPOLICY_HPP
#define SLA_GPU_SLABACKENDPOLICY_HPP

#include <cstdint>
#include <cstddef>
#include <string>

#include <libslic3r/SLA/RasterBase.hpp>

// Phase 5: runtime GPU-vs-CPU selection policy for SLA rasterization.
//
// This header implements the "is it worth it" gate from
// docs/gpu_sla_rasterization_scope.md (§"Runtime GPU-vs-CPU selection policy"):
//
//     pick_backend(resolution, layer_count):
//         if VULKAN disabled by config/env            -> AGG
//         if loader missing or 0 physical devices     -> AGG
//         dev = best device (DISCRETE_GPU preferred)
//         if dev.maxImageDimension2D < panel dims     -> AGG
//         if dev.local VRAM < working-set estimate    -> AGG
//         work = resolution.megapixels * layer_count
//         if work < THRESHOLD (calibrated)            -> AGG
//         else                                        -> Vulkan(dev)
//
// The CORE decision (pick_backend_core) is PURE: it takes the already-queried
// device capabilities, the resolution, the layer count, and the config, and
// returns a BackendChoice. It pulls in NO Vulkan headers, so it builds and is
// unit-testable even in an SLIC3R_VULKAN=OFF build (the tests construct
// synthetic DeviceCaps). The thin wrapper pick_backend() (guarded by
// SLIC3R_VULKAN, defined in the .cpp) queries the live VulkanContext for the
// caps and the SLA_GPU env override, then calls pick_backend_core().
//
// NOTE (integration): this is provided as a real, testable function. It is NOT
// wired into the live SLAArchive::create_raster() factory -- the headless SLA
// CLI is disabled so the end-to-end path cannot be exercised, and the Phase-5
// benchmark (below) shows the naive GPU fill path does not beat AGG end-to-end
// (PNG encode dominates; see bench/SLA_RESULTS.md "Phase 5"). The commented
// integration point at the bottom of this header shows exactly how
// create_raster() WOULD call it, guarded entirely behind SLIC3R_VULKAN.

namespace Slic3r {
namespace sla {

enum class BackendChoice {
    AGG,    // CPU anti-aliased scanline fill (RasterGrayscaleAAGammaPower)
    Vulkan, // headless Vulkan compute fill (VulkanRaster)
};

// Why a given choice was made -- surfaced for logging/tests so the gate is
// transparent (matches the ordered checks in the scope pseudocode).
enum class BackendReason {
    DisabledByConfig,   // SLA_GPU=0 / config off
    ForcedByConfig,     // SLA_GPU=1 (force GPU; still subject to hard caps)
    NoUsableDevice,     // loader missing / 0 devices / device not usable
    PanelTooLarge,      // maxImageDimension2D < panel dims
    InsufficientVram,   // device-local VRAM < working-set estimate
    BelowThreshold,     // work = megapixels*layers < THRESHOLD
    AboveThreshold,     // work >= THRESHOLD -> Vulkan
};

// Already-queried device capabilities (filled from VulkanContext in the real
// path; constructed directly by the unit tests). usable=false models "no
// loader / 0 devices / unusable device".
struct DeviceCaps {
    bool        usable = false;
    uint32_t    device_type = 0;        // VkPhysicalDeviceType (0 == OTHER)
    uint32_t    max_image_dim_2d = 0;   // px
    uint64_t    device_local_vram = 0;  // bytes
    std::string device_name;
};

// Tri-state GPU enable, parsed from config/env (SLA_GPU=0/1/auto).
enum class GpuEnable {
    Off,   // never use GPU (SLA_GPU=0)
    Auto,  // policy decides (default)
    Force, // use GPU if a usable device with adequate caps exists (SLA_GPU=1);
           // still rejected by the hard panel-size / VRAM gates, but bypasses
           // the work>=THRESHOLD economic gate.
};

struct PolicyConfig {
    GpuEnable enable = GpuEnable::Auto;

    // Calibrated work threshold: work = panel_megapixels * layer_count. Below
    // this, stay on AGG (GPU launch + host<->device transfer not worth it).
    //
    // CALIBRATION (Phase 5, bench/SLA_RESULTS.md): on this host (RTX 3090) the
    // naive O(pixels*edges) compute fill does NOT beat AGG *end-to-end* at any
    // resolution -- PNG encode is >98% of the rasterize stage and stays on the
    // CPU for both backends, and the naive GPU fill is itself SLOWER than AGG's
    // already-tiny analytic fill at 8K/12K. So on the current fill-only path
    // there is NO work value at which Vulkan wins end-to-end; the honest
    // threshold is "effectively infinite" (GPU not worth it until encode is
    // also offloaded). We encode that as a very large sentinel so Auto mode
    // keeps everything on AGG, while still exercising the full policy (Force
    // mode and the hard caps remain meaningful, and the constant becomes the
    // real crossover once a GPU encode path lands -- see the Phase-2
    // recommendation). Megapixel-layers.
    double work_threshold_mp_layers = 1.0e18; // sentinel: "never on this path"

    // Working-set estimate knobs. The GPU path needs, per in-flight layer, an
    // R8 mask plus (in the current naive context) a uint32 scratch image and
    // the edge buffer. We size the estimate generously (bytes/pixel) and add a
    // fixed overhead so the VRAM gate is conservative.
    double   bytes_per_pixel_working_set = 8.0;  // R8 out (1) + u32 scratch (4) + slack
    uint64_t fixed_overhead_bytes        = 64ull * 1024 * 1024; // pipelines/pools/etc
    double   vram_safety_fraction        = 0.80; // only count 80% of VRAM as usable
};

// Estimate the GPU working-set (bytes) for one rasterize job at this
// resolution. The current per-layer path holds ONE panel-sized set of buffers
// in flight (Phase 3 serializes dispatches), so the working set is bounded by a
// single panel regardless of layer_count; a future batched (Phase 2) path that
// holds K masks resident would multiply the per-pixel term by K. We keep it
// single-panel here (matches the implemented backend) plus fixed overhead.
uint64_t estimate_working_set_bytes(const Resolution &res, const PolicyConfig &cfg);

// PURE core: given queried caps + workload + config, return the choice.
// `out_reason` (optional) receives why. No Vulkan dependency; fully testable.
BackendChoice pick_backend_core(const DeviceCaps  &caps,
                                const Resolution  &res,
                                size_t             layer_count,
                                const PolicyConfig &cfg,
                                BackendReason     *out_reason = nullptr);

// Human-readable reason (for logs/tests).
const char *to_string(BackendReason r);

#ifdef SLIC3R_VULKAN
// Live wrapper: parse the SLA_GPU env override, query the shared VulkanContext
// for device caps, and call pick_backend_core(). Only available when the
// vendored Vulkan backend is compiled in. `cfg_in` lets callers/tests override
// the threshold etc.; the env override takes precedence over cfg_in.enable.
BackendChoice pick_backend(const Resolution   &res,
                           size_t              layer_count,
                           const PolicyConfig &cfg_in = {},
                           BackendReason      *out_reason = nullptr,
                           std::string        *out_device_name = nullptr);
#endif

// --------------------------------------------------------------------------
// Integration point (commented; not wired in -- see header note above). This is
// how SLAArchive::create_raster() WOULD select the backend, kept entirely
// behind SLIC3R_VULKAN so OFF builds are byte-identical:
//
//   std::unique_ptr<RasterBase>
//   SLAArchive::create_raster() const {
//       const Resolution res   = /* panel display_pixels_x/y */;
//       const PixelDim   pxdim = /* panel size / pixels */;
//       const double     gamma = /* cfg */;
//       const Trafo      tr    = /* orientation/mirror */;
//   #ifdef SLIC3R_VULKAN
//       const size_t n_layers  = /* this->layer_count() */;
//       if (sla::pick_backend(res, n_layers) == sla::BackendChoice::Vulkan)
//           return sla::create_vulkan_raster(res, pxdim, gamma, tr);
//   #endif
//       return sla::create_raster_grayscale_aa(res, pxdim, gamma, tr);
//   }
// --------------------------------------------------------------------------

} // namespace sla
} // namespace Slic3r

#endif // SLA_GPU_SLABACKENDPOLICY_HPP
