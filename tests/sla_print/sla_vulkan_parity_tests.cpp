// Phase 4: small Catch2 parity gate for the Vulkan SLA rasterizer, so the
// feature is gated in the normal test suite (not only the standalone
// sla_vulkan_parity harness). Compiled into sla_print_tests ONLY when
// SLIC3R_VULKAN is ON and libslic3r_sla_gpu is linked.
//
// This is intentionally small and fast (one synthetic layer with curved/
// diagonal edges, modest resolution): it asserts the DOCUMENTED parity
// tolerance (see bench/SLA_RESULTS.md "Phase 4") --
//   - empty(0) pixels match AGG EXACTLY,
//   - solid(255) pixels match AGG within <= 1 level,
//   - the mean |delta| on edge pixels is bounded.
// The exhaustive per-mesh sweep + .sl1 round-trip live in sla_vulkan_parity.
//
// If no usable GPU is present, VulkanRaster falls back to AGG; the test then
// trivially passes (it is SKIPPED-equivalent) since AGG == AGG. We detect that
// via gpu_active() and SKIP so a no-GPU CI run is not a false pass/fail.

#include <catch2/catch_all.hpp>

#ifdef SLIC3R_VULKAN

#include <cstdint>
#include <vector>
#include <cmath>
#include <string>
#include <algorithm>

#include <libslic3r/SLA/RasterBase.hpp>
#include <libslic3r/SLA/AGGRaster.hpp>
#include <libslic3r/ExPolygon.hpp>
#include <libslic3r/Point.hpp>
#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/TriangleMeshSlicer.hpp>
#include <libslic3r/BoundingBox.hpp>
#include <libslic3r/MTUtils.hpp>

#include "libslic3r/SLA/GPU/VulkanRaster.hpp"
#include "test_utils.hpp"

using namespace Slic3r;
using namespace Slic3r::sla;

namespace {
std::vector<uint8_t> raster_raw(RasterBase &r)
{
    std::vector<uint8_t> raw;
    RasterEncoder grab = [&](const void *p, size_t w, size_t h, size_t nc) -> EncodedRaster {
        raw.assign((const uint8_t *)p, (const uint8_t *)p + w * h * nc);
        return EncodedRaster{};
    };
    r.encode(grab);
    return raw;
}
} // namespace

TEST_CASE("Vulkan SLA raster matches AGG within tolerance", "[SLA][Vulkan]")
{
    const size_t W = 768, H = 768;
    const double px_mm = 0.05;
    Resolution res(W, H);
    PixelDim   pxdim(px_mm, px_mm);
    RasterBase::Trafo tr; // default landscape, origin top-left

    // Probe the GPU; if absent, the backend is AGG and parity is trivial.
    auto probe = create_vulkan_raster(res, pxdim, 1.0, tr);
    auto *vr = dynamic_cast<VulkanRaster *>(probe.get());
    if (!vr || !vr->gpu_active())
        SKIP("no usable Vulkan GPU; VulkanRaster falls back to AGG");

    // Use a REAL organic layer (frog_legs.obj sliced near mid-height): mixed
    // edge orientations, exactly like a real SLA layer -- this is what the
    // standalone sla_vulkan_parity harness validates on the benchmark meshes,
    // and gives the documented ~5-level mean edge delta. (A single pure-45-deg
    // edge is the box-supersample worst case and is intentionally NOT used as
    // the gate geometry; see bench/SLA_RESULTS.md.)
    TriangleMesh mesh = load_model("frog_legs.obj");
    REQUIRE_FALSE(mesh.empty());

    BoundingBoxf3 bb = mesh.bounding_box();
    Vec3d sz = bb.size(), c = bb.center();
    mesh.translate(-float(c.x()), -float(c.y()), -float(bb.min.z()));
    // Scale XY to ~70% of the panel short axis so it fits with margin.
    double fp = std::max(sz.x(), sz.y());
    if (fp > 0) mesh.scale(float(0.70 * (px_mm * double(W)) / fp));
    bb = mesh.bounding_box();
    float zmid = float(bb.min.z() + 0.5 * (bb.max.z() - bb.min.z()));
    std::vector<ExPolygons> slices = slice_mesh_ex(mesh.its, {zmid}, 0.005f);
    REQUIRE(slices.size() == 1);
    REQUIRE_FALSE(slices[0].empty());

    // Center the (origin-centered) layer in the panel.
    BoundingBox panel_bb({0, 0}, {scaled(px_mm * double(W)), scaled(px_mm * double(H))});
    tr.center_x = panel_bb.center().x();
    tr.center_y = panel_bb.center().y();

    auto vk = create_vulkan_raster(res, pxdim, 1.0, tr);
    for (const ExPolygon &ep : slices[0]) vk->draw(ep);
    std::vector<uint8_t> gpu = raster_raw(*vk);

    auto agg = create_raster_grayscale_aa(res, pxdim, 1.0, tr);
    for (const ExPolygon &ep : slices[0]) agg->draw(ep);
    std::vector<uint8_t> ar = raster_raw(*agg);

    REQUIRE(gpu.size() == ar.size());
    REQUIRE(!gpu.empty());

    size_t empty_agg = 0, empty_bad = 0;
    size_t solid_agg = 0; int solid_maxdiff = 0;
    size_t edge = 0; long edge_sum = 0;
    for (size_t i = 0; i < gpu.size(); ++i) {
        int g = gpu[i], a = ar[i];
        if (a == 0)   { ++empty_agg; if (g != 0) ++empty_bad; }
        if (a == 255) { ++solid_agg; int d = 255 - g; if (d > solid_maxdiff) solid_maxdiff = d; }
        if ((g > 0 && g < 255) || (a > 0 && a < 255)) { ++edge; int d = g - a; edge_sum += d < 0 ? -d : d; }
    }
    double edge_mean = edge ? double(edge_sum) / edge : 0.0;

    // Documented Phase-4 tolerance (default ss=8):
    REQUIRE(empty_agg > 0);
    REQUIRE(solid_agg > 0);
    REQUIRE(edge > 0);
    CHECK(empty_bad == 0);          // empty(0) EXACT
    CHECK(solid_maxdiff <= 1);      // solid(255) within 1 level
    CHECK(edge_mean <= 8.0);        // bounded edge AA delta (real-layer mean ~5)
}

#endif // SLIC3R_VULKAN
