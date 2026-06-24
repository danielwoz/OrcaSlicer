#include "VulkanRaster.hpp"

#ifdef SLIC3R_VULKAN

#include <cstdlib>

#include <libslic3r/SLA/AGGRaster.hpp>
#include "VulkanContext.hpp"

namespace Slic3r {
namespace sla {

// Phase 3: real headless Vulkan compute fill.
//
// draw() accumulates polygon edges in PIXEL space, transformed with the SAME
// mapping AGGRaster uses (pxdim scale, flipXY, center translate, mirror x/y) so
// the GPU mask lands exactly where AGG's would. encode() dispatches the compute
// fill ONCE into an R8 mask, reads it back, and hands the bytes to the encoder.
//
// AGG fallback: if no usable VulkanContext exists (no GPU / driver / suitable
// device), encode() rasterizes through an AGG backend instead, so the seam
// never crashes on a no-GPU machine. Construction never throws.
VulkanRaster::VulkanRaster(const Resolution        &res,
                           const PixelDim          &pxdim,
                           double                   gamma,
                           const RasterBase::Trafo &tr)
    : m_res(res)
    , m_pxdim_scaled(SCALING_FACTOR, SCALING_FACTOR)
    , m_gamma(gamma)
    , m_trafo(tr)
{
    if (pxdim.w_mm != 0 && pxdim.h_mm != 0) {
        m_pxdim_scaled.w_mm /= pxdim.w_mm;
        m_pxdim_scaled.h_mm /= pxdim.h_mm;
    }

    // Decide GPU vs AGG once, at construction. Acquiring the shared context
    // here triggers (idempotent) device init. If no usable device exists we
    // build an AGG backend now and feed it in draw(), so encode() can delegate.
    auto ctx = gpu::VulkanContext::get();
    m_use_gpu = (ctx && ctx->is_usable());
    if (!m_use_gpu)
        m_agg_fallback = std::make_unique<RasterGrayscaleAAGammaPower>(res, pxdim, tr, gamma);

    // Phase 4 calibration hook: SLA_VULKAN_SS overrides the supersample factor
    // so the parity harness can sweep ss without recompiling. Unset => default
    // m_ss (the calibrated production default). Clamped to [1, 16].
    if (const char *e = std::getenv("SLA_VULKAN_SS")) {
        long v = std::strtol(e, nullptr, 10);
        if (v >= 1 && v <= 16) m_ss = unsigned(v);
    }
}

VulkanRaster::~VulkanRaster() = default;

bool VulkanRaster::gpu_active() const
{
    auto ctx = gpu::VulkanContext::get();
    return ctx && ctx->is_usable();
}

std::string VulkanRaster::gpu_device_name() const
{
    auto ctx = gpu::VulkanContext::get();
    return (ctx && ctx->is_usable()) ? ctx->device_name() : std::string{};
}

// Transform one closed ring of scaled-integer points into pixel-space edges,
// matching AGGRaster::to_path() exactly.
void VulkanRaster::add_ring(const Points &pts)
{
    if (pts.size() < 2)
        return;

    const double cx = double(m_trafo.center_x) * m_pxdim_scaled.w_mm;
    const double cy = double(m_trafo.center_y) * m_pxdim_scaled.h_mm;
    const double W  = double(m_res.width_px);
    const double H  = double(m_res.height_px);

    auto map = [&](const Point &p, float &ox, float &oy) {
        double X, Y;
        if (m_trafo.flipXY) {
            // _to_path_flpxy: X uses getPy, Y uses getPx
            X = double(p.y()) * m_pxdim_scaled.h_mm;
            Y = double(p.x()) * m_pxdim_scaled.w_mm;
        } else {
            X = double(p.x()) * m_pxdim_scaled.w_mm;
            Y = double(p.y()) * m_pxdim_scaled.h_mm;
        }
        // translate_all_paths(center_x * w_scaled, center_y * h_scaled)
        X += cx;
        Y += cy;
        if (m_trafo.mirror_x) X = W - X;
        if (m_trafo.mirror_y) Y = H - Y;
        ox = float(X);
        oy = float(Y);
    };

    // Build edges around the closed ring (AGG closes back to front()).
    float px0, py0, px1, py1;
    map(pts.front(), px0, py0);
    const float first_x = px0, first_y = py0;
    for (size_t i = 1; i < pts.size(); ++i) {
        map(pts[i], px1, py1);
        m_edges.push_back({px0, py0, px1, py1});
        px0 = px1; py0 = py1;
    }
    // close
    m_edges.push_back({px0, py0, first_x, first_y});
}

void VulkanRaster::draw(const ExPolygon &poly)
{
    if (m_use_gpu) {
        add_ring(poly.contour.points);
        for (const auto &h : poly.holes)
            add_ring(h.points);
    } else {
        m_agg_fallback->draw(poly);
    }
}

RasterBase::Trafo VulkanRaster::trafo() const
{
    return m_trafo;
}

EncodedRaster VulkanRaster::encode(RasterEncoder encoder) const
{
    if (m_use_gpu) {
        auto ctx = gpu::VulkanContext::get();
        if (ctx && ctx->is_usable()) {
            // Our Edge has the same layout as gpu::Edge (4 floats).
            static_assert(sizeof(Edge) == sizeof(gpu::Edge), "edge layout mismatch");
            const std::vector<gpu::Edge> &gedges =
                reinterpret_cast<const std::vector<gpu::Edge> &>(m_edges);

            std::vector<uint8_t> mask;
            if (ctx->rasterize(gedges,
                               uint32_t(m_res.width_px),
                               uint32_t(m_res.height_px),
                               m_ss, mask)) {
                return encoder(mask.data(), m_res.width_px, m_res.height_px, 1);
            }
            // GPU dispatch failed at runtime: produce an empty (black) mask of
            // the right size rather than crashing. (We did not also feed AGG on
            // the GPU path to avoid double work; a runtime dispatch failure
            // after a successful init is not expected.)
            std::vector<uint8_t> black(m_res.pixels(), 0);
            return encoder(black.data(), m_res.width_px, m_res.height_px, 1);
        }
    }

    // ---- AGG fallback (no usable GPU) -----------------------------------
    return m_agg_fallback->encode(std::move(encoder));
}

std::unique_ptr<RasterBase> create_vulkan_raster(const Resolution        &res,
                                                 const PixelDim          &pxdim,
                                                 double                   gamma,
                                                 const RasterBase::Trafo &tr)
{
    return std::make_unique<VulkanRaster>(res, pxdim, gamma, tr);
}

} // namespace sla
} // namespace Slic3r

#endif // SLIC3R_VULKAN
