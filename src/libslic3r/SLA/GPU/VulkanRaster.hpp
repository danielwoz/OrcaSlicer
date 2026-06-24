#ifndef SLA_GPU_VULKANRASTER_HPP
#define SLA_GPU_VULKANRASTER_HPP

#include <memory>
#include <vector>
#include <string>

#include <libslic3r/SLA/RasterBase.hpp>

// This header is only meaningful when the optional vendored Vulkan backend is
// compiled in (SLIC3R_VULKAN). When the option is OFF this whole translation
// unit is inert and nothing here is built into libslic3r.
#ifdef SLIC3R_VULKAN

namespace Slic3r {
namespace sla {

// GPU SLA rasterizer backend.
//
// Phase 2 status (build infra + seam only): VulkanRaster is a RasterBase that,
// for now, simply delegates every call to the existing CPU AGG backend
// (RasterGrayscaleAAGammaPower) and returns its output. This proves the seam
// compiles, links, and produces valid raster output through the RasterBase
// factory interface. No real GPU work happens yet.
//
// Phase 3 will replace the delegate internals with a real headless
// VulkanContext (compute/offscreen fill -> R8 mask -> readback). The public
// RasterBase interface does not change, so nothing upstream/downstream of the
// create_raster() factory is affected.
class VulkanRaster : public RasterBase {
public:
    VulkanRaster(const Resolution        &res,
                 const PixelDim          &pxdim,
                 double                   gamma,
                 const RasterBase::Trafo &tr);

    ~VulkanRaster() override;

    VulkanRaster(const VulkanRaster &) = delete;
    VulkanRaster &operator=(const VulkanRaster &) = delete;

    // RasterBase interface ---------------------------------------------------
    void          draw(const ExPolygon &poly) override;
    Trafo         trafo() const override;
    EncodedRaster encode(RasterEncoder encoder) const override;

    // Phase 3 diagnostics (used by the smoke test): true if the last/next
    // encode() will use the real GPU path (a usable VulkanContext exists);
    // false if it falls back to AGG. Also exposes the chosen device name.
    bool          gpu_active() const;
    std::string   gpu_device_name() const;

    Resolution    resolution() const { return m_res; }

private:
    // One float edge (x0,y0,x1,y1) in PIXEL space. Mirrors gpu::Edge but kept
    // here so the header doesn't pull in Vulkan headers.
    struct Edge { float x0, y0, x1, y1; };

    // Map a scaled-integer Point through the exact same transform AGGRaster
    // uses (pxdim scale -> flipXY -> center translate -> mirror_x/y), into
    // top-left-origin pixel space. Appends edges of a closed point ring.
    void add_ring(const Points &pts);

    Resolution        m_res;
    PixelDim          m_pxdim_scaled; // SCALING_FACTOR / w_mm, .../h_mm
    double            m_gamma = 1.0;
    Trafo             m_trafo;

    std::vector<Edge> m_edges; // accumulated across draw() calls, pixel space

    // AGG fallback: a real AGG backend fed in lockstep with draw(). When no
    // usable GPU exists (or a GPU dispatch fails), encode() delegates here, so
    // the seam never crashes on a no-GPU machine and the fallback is a true AGG
    // render. Only constructed when the GPU is unavailable, so a GPU run does
    // not pay for the CPU draw.
    bool                        m_use_gpu = false; // decided in ctor
    std::unique_ptr<RasterBase> m_agg_fallback;    // non-null iff !m_use_gpu

    // Supersample factor per axis for the GPU fill (NxN box supersample of
    // even-odd parity, box-averaged to 8-bit coverage). Calibrated in Phase 4
    // (see bench/SLA_RESULTS.md "Phase 4"): at ss=8, solid(255) and empty(0)
    // pixels match AGG EXACTLY and the mean |delta| on edge pixels is ~5 levels
    // across the benchmark meshes -- the parity gate (sla_vulkan_parity) passes.
    // Higher ss lowers the edge mean further but costs ss^2 samples/pixel and
    // starts to disagree with AGG by 1 level on solid boundaries; lower ss
    // raises the edge mean. ss=8 is the documented default. (Overridable at
    // runtime via the SLA_VULKAN_SS env var for calibration only.)
    unsigned m_ss = 8;
};

// Free-function factory mirroring create_raster_grayscale_aa(). Phase 5 will
// decide at runtime (device enumeration / workload threshold) whether
// create_raster() returns this Vulkan backend or the AGG backend; Phase 2 only
// makes it exist, compile, and link.
std::unique_ptr<RasterBase> create_vulkan_raster(
    const Resolution        &res,
    const PixelDim          &pxdim,
    double                   gamma = 1.0,
    const RasterBase::Trafo &tr    = {});

} // namespace sla
} // namespace Slic3r

#endif // SLIC3R_VULKAN

#endif // SLA_GPU_VULKANRASTER_HPP
