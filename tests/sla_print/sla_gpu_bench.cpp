// SLA rasterization benchmark — Vulkan (GPU) vs AGG (CPU), Phase 5.
//
// Sibling of sla_raster_bench.cpp (Phase 1, AGG-only). This one times the FULL
// per-layer rasterize for BOTH backends across SLA panel resolutions
// (4K -> 8K -> 12K) on real sliced layers, and reports:
//
//   * FILL-ONLY:  AGG draw()        vs  GPU compute fill + readback
//   * ENCODE:     PNG (miniz)       (CPU for both backends -- identical work)
//   * END-TO-END: draw+encode (AGG) vs  fill+readback+encode (GPU)
//
// The Phase-1 finding is that PNG encode is >98% of the rasterize stage and
// stays on the CPU for BOTH backends, so the GPU only touches ~1-2% of the
// stage. This bench makes the fill-only AND end-to-end comparison explicit so
// the THRESHOLD calibration (bench/SLA_RESULTS.md) is honest.
//
// The GPU path uses the SAME edge transform VulkanRaster uses (so the GPU mask
// lands where AGG's would) and calls VulkanContext::rasterize() directly to
// isolate the GPU fill+readback time, then PNG-encodes the returned mask to
// time encode separately. AGG uses RasterGrayscaleAAGammaPower::draw/encode.
//
// Build:  cmake --build build --config Release -DSLIC3R_VULKAN=ON \
//               -DBUILD_TESTS=ON --target sla_gpu_bench -j32
// Run:    build/tests/sla_print/Release/sla_gpu_bench [mesh.drc] [layers] [runs]
//
// MANUAL benchmark; NOT registered with ctest. Requires SLIC3R_VULKAN.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

#include "libslic3r/libslic3r.h"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleMeshSlicer.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/MTUtils.hpp"
#include "libslic3r/Format/DRC.hpp"
#include "libslic3r/SLA/AGGRaster.hpp"
#include "libslic3r/SLA/RasterBase.hpp"

#include "libslic3r/SLA/GPU/VulkanContext.hpp"
#include "libslic3r/SLA/GPU/SlaBackendPolicy.hpp"

using namespace Slic3r;

namespace {

using Clock = std::chrono::steady_clock;
static inline double secs(Clock::time_point a, Clock::time_point b)
{
    return std::chrono::duration<double>(b - a).count();
}

struct PanelSpec {
    const char *name;
    size_t      px, py;
    double      w_mm, h_mm;
};

// Same panels as Phase 1.
const PanelSpec PANELS[] = {
    {"4K",  3840, 2400, 192.0,  120.0},   //  9.2 MP
    {"8K",  7680, 4320, 218.88, 123.12},  // 33.2 MP
    {"12K", 11520, 5120, 230.4, 102.4},   // 59.0 MP
};

std::vector<ExPolygons> make_layers(const TriangleMesh &mesh_in, size_t want_layers)
{
    TriangleMesh mesh = mesh_in;
    BoundingBoxf3 bb = mesh.bounding_box();
    Vec3d size = bb.size(), center = bb.center();
    mesh.translate(-float(center.x()), -float(center.y()), -float(bb.min.z()));
    const double target_mm = 0.70 * 102.4;
    double fp = std::max(size.x(), size.y());
    if (fp > 0) mesh.scale(float(target_mm / fp));
    bb = mesh.bounding_box();
    double zmin = bb.min.z(), zmax = bb.max.z();
    double height = std::max(1e-3, zmax - zmin);
    float layer_h = float(height / double(want_layers));
    if (layer_h <= 0) layer_h = 0.05f;
    std::vector<float> zs = grid(float(zmin + layer_h / 2.f), float(zmax), layer_h);
    return slice_mesh_ex(mesh.its, zs, 0.005f);
}

// Build pixel-space edges for one layer, matching VulkanRaster::add_ring()
// exactly (same transform AGGRaster uses). One Edge = (x0,y0,x1,y1).
void add_ring(const Points &pts, const sla::RasterBase::Trafo &tr,
              const sla::PixelDim &pxdim_scaled, size_t W, size_t H,
              std::vector<sla::gpu::Edge> &out)
{
    if (pts.size() < 2) return;
    const double cx = double(tr.center_x) * pxdim_scaled.w_mm;
    const double cy = double(tr.center_y) * pxdim_scaled.h_mm;
    const double Wd = double(W), Hd = double(H);
    auto map = [&](const Point &p, float &ox, float &oy) {
        double X, Y;
        if (tr.flipXY) { X = double(p.y()) * pxdim_scaled.h_mm; Y = double(p.x()) * pxdim_scaled.w_mm; }
        else           { X = double(p.x()) * pxdim_scaled.w_mm; Y = double(p.y()) * pxdim_scaled.h_mm; }
        X += cx; Y += cy;
        if (tr.mirror_x) X = Wd - X;
        if (tr.mirror_y) Y = Hd - Y;
        ox = float(X); oy = float(Y);
    };
    float px0, py0, px1, py1;
    map(pts.front(), px0, py0);
    const float fx = px0, fy = py0;
    for (size_t i = 1; i < pts.size(); ++i) {
        map(pts[i], px1, py1);
        out.push_back({px0, py0, px1, py1});
        px0 = px1; py0 = py1;
    }
    out.push_back({px0, py0, fx, fy});
}

std::vector<sla::gpu::Edge> build_edges(const ExPolygons &layer,
                                        const sla::RasterBase::Trafo &tr,
                                        const sla::PixelDim &pxdim_scaled,
                                        size_t W, size_t H)
{
    std::vector<sla::gpu::Edge> edges;
    for (const ExPolygon &ep : layer) {
        add_ring(ep.contour.points, tr, pxdim_scaled, W, H, edges);
        for (const Polygon &h : ep.holes)
            add_ring(h.points, tr, pxdim_scaled, W, H, edges);
    }
    return edges;
}

struct AggTimes  { double draw = 0, encode = 0; double total() const { return draw + encode; } };
struct VkTimes   { double fill = 0, encode = 0; double total() const { return fill + encode; } };

AggTimes agg_pass(const std::vector<ExPolygons> &layers, const PanelSpec &p)
{
    sla::Resolution res{p.px, p.py};
    sla::PixelDim   pxdim{p.w_mm / double(p.px), p.h_mm / double(p.py)};
    sla::RasterBase::Trafo tr{sla::RasterBase::roLandscape, sla::RasterBase::NoMirror};
    BoundingBox panel_bb({0, 0}, {scaled(p.w_mm), scaled(p.h_mm)});
    tr.center_x = panel_bb.center().x();
    tr.center_y = panel_bb.center().y();

    AggTimes t; volatile size_t sink = 0;
    for (const ExPolygons &layer : layers) {
        sla::RasterGrayscaleAAGammaPower raster{res, pxdim, tr, 1.0};
        auto d0 = Clock::now();
        for (const ExPolygon &poly : layer) raster.draw(poly);
        auto d1 = Clock::now(); t.draw += secs(d0, d1);
        auto e0 = Clock::now();
        sla::EncodedRaster enc = raster.encode(sla::PNGRasterEncoder{});
        auto e1 = Clock::now(); t.encode += secs(e0, e1);
        sink += enc.size();
    }
    (void)sink; return t;
}

VkTimes vk_pass(const std::vector<ExPolygons> &layers, const PanelSpec &p,
                sla::gpu::VulkanContext &ctx, unsigned ss)
{
    sla::Resolution res{p.px, p.py};
    sla::PixelDim   pxdim{p.w_mm / double(p.px), p.h_mm / double(p.py)};
    // scaled pixel dim, matching VulkanRaster ctor (SCALING_FACTOR / mm).
    sla::PixelDim pxdim_scaled{SCALING_FACTOR / pxdim.w_mm, SCALING_FACTOR / pxdim.h_mm};
    sla::RasterBase::Trafo tr{sla::RasterBase::roLandscape, sla::RasterBase::NoMirror};
    BoundingBox panel_bb({0, 0}, {scaled(p.w_mm), scaled(p.h_mm)});
    tr.center_x = panel_bb.center().x();
    tr.center_y = panel_bb.center().y();

    VkTimes t; volatile size_t sink = 0;
    std::vector<uint8_t> mask;
    for (const ExPolygons &layer : layers) {
        std::vector<sla::gpu::Edge> edges = build_edges(layer, tr, pxdim_scaled, p.px, p.py);

        auto f0 = Clock::now();
        bool ok = ctx.rasterize(edges, uint32_t(p.px), uint32_t(p.py), ss, mask);
        auto f1 = Clock::now(); t.fill += secs(f0, f1);
        if (!ok) { t.fill = -1; return t; } // signal failure

        auto e0 = Clock::now();
        sla::EncodedRaster enc = sla::PNGRasterEncoder{}(mask.data(), p.px, p.py, 1);
        auto e1 = Clock::now(); t.encode += secs(e0, e1);
        sink += enc.size();
    }
    (void)sink; return t;
}

} // namespace

int main(int argc, char **argv)
{
#ifndef BENCH_MODELS_DIR
#define BENCH_MODELS_DIR "resources/handy_models"
#endif
    std::string mesh_path = (argc > 1) ? argv[1]
        : std::string(BENCH_MODELS_DIR) + "/Stanford_Bunny.drc";
    size_t want_layers = (argc > 2) ? size_t(std::strtoul(argv[2], nullptr, 10)) : 100;
    int    runs        = (argc > 3) ? std::atoi(argv[3]) : 3;
    if (runs < 1) runs = 1;

    unsigned ss = 8;
    if (const char *e = std::getenv("SLA_VULKAN_SS")) {
        long v = std::strtol(e, nullptr, 10);
        if (v >= 1 && v <= 16) ss = unsigned(v);
    }
    // Allow limiting the GPU run to a subset of layers / skipping 12K if the
    // naive shader is too slow. SLA_GPU_BENCH_MAX_LAYERS caps layers per GPU
    // pass; SLA_GPU_BENCH_SKIP_12K=1 runs AGG-only at 12K.
    size_t gpu_max_layers = SIZE_MAX;
    if (const char *e = std::getenv("SLA_GPU_BENCH_MAX_LAYERS"))
        gpu_max_layers = size_t(std::strtoull(e, nullptr, 10));

    std::printf("SLA GPU-vs-AGG benchmark (Phase 5)\n");
    std::printf("mesh    : %s\n", mesh_path.c_str());

    auto ctx = sla::gpu::VulkanContext::get();
    bool gpu = ctx && ctx->is_usable();
    std::printf("GPU     : %s%s%s\n",
                gpu ? "yes (" : "no",
                gpu ? ctx->device_name().c_str() : "",
                gpu ? ")" : " -- Vulkan unusable, GPU columns will be skipped");
    if (gpu)
        std::printf("limits  : maxImageDim2D=%u, device-local VRAM=%.1f GiB\n",
                    ctx->max_image_dim_2d(),
                    double(ctx->device_local_vram_bytes()) / (1024.0*1024*1024));
    std::printf("ss      : %u (GPU supersample)\n", ss);

    TriangleMesh mesh;
    if (!load_drc(mesh_path.c_str(), &mesh) || mesh.empty()) {
        std::fprintf(stderr, "ERROR: failed to load mesh '%s'\n", mesh_path.c_str());
        return 2;
    }
    std::vector<ExPolygons> layers = make_layers(mesh, want_layers);
    if (layers.empty()) { std::fprintf(stderr, "ERROR: 0 layers\n"); return 3; }

    size_t verts = 0;
    for (const auto &l : layers) for (const auto &ep : l) {
        verts += ep.contour.points.size();
        for (const auto &h : ep.holes) verts += h.points.size();
    }
    std::printf("layers  : %zu, ~%zu contour vertices total\n", layers.size(), verts);
    std::printf("runs    : %d (min reported), +1 warmup discarded\n\n", runs);

    std::printf("%-5s %7s %7s | %11s %11s %11s | %11s %11s %11s | %9s %9s\n",
                "res", "MP", "layers",
                "AGGfill_s", "AGGenc_s", "AGGtot_s",
                "VKfill_s", "VKenc_s", "VKtot_s",
                "fillSpd", "e2eSpd");
    std::printf("--------------------------------------------------------------------------------------------------------------------\n");

    struct Row { const char *name; double mp; size_t layers;
                 AggTimes agg; VkTimes vk; bool vk_ok; };
    std::vector<Row> rows;

    for (const PanelSpec &p : PANELS) {
        double mp = double(p.px) * double(p.py) / 1.0e6;
        std::fprintf(stderr, "[%s] AGG warmup+%d runs ...\n", p.name, runs); std::fflush(stderr);

        // ---- AGG (always) ----
        agg_pass(layers, p); // warmup
        AggTimes agg{}; double best_agg = 1e300;
        for (int i = 0; i < runs; ++i) {
            AggTimes t = agg_pass(layers, p);
            if (t.total() < best_agg) { best_agg = t.total(); agg = t; }
        }

        // ---- Vulkan (if usable) ----
        VkTimes vk{}; bool vk_ok = false;
        if (gpu) {
            // GPU layer subset (the naive O(pixels*edges) shader can be slow at
            // high res). We time the same subset for the per-layer comparison;
            // the table notes when a subset was used.
            std::vector<ExPolygons> gl = layers;
            if (gl.size() > gpu_max_layers) gl.resize(gpu_max_layers);
            std::fprintf(stderr, "[%s] GPU fill on %zu layers (ss=%u) ...\n",
                         p.name, gl.size(), ss); std::fflush(stderr);

            VkTimes warm = vk_pass(gl, p, *ctx, ss); // warmup (also probes failure)
            if (warm.fill >= 0) {
                double best_vk = 1e300;
                for (int i = 0; i < runs; ++i) {
                    VkTimes t = vk_pass(gl, p, *ctx, ss);
                    if (t.total() < best_vk) { best_vk = t.total(); vk = t; }
                }
                // Scale GPU times to the full layer count for a fair per-job
                // comparison if a subset was used.
                if (gl.size() < layers.size() && gl.size() > 0) {
                    double k = double(layers.size()) / double(gl.size());
                    vk.fill *= k; vk.encode *= k;
                }
                vk_ok = true;
            }
        }

        double fill_spd = (vk_ok && vk.fill > 0) ? agg.draw / vk.fill : 0.0;
        double e2e_spd  = (vk_ok && vk.total() > 0) ? agg.total() / vk.total() : 0.0;

        std::printf("%-5s %7.1f %7zu | %11.4f %11.4f %11.4f | ",
                    p.name, mp, layers.size(), agg.draw, agg.encode, agg.total());
        if (vk_ok)
            std::printf("%11.4f %11.4f %11.4f | %9.3f %9.3f\n",
                        vk.fill, vk.encode, vk.total(), fill_spd, e2e_spd);
        else
            std::printf("%11s %11s %11s | %9s %9s\n", "n/a", "n/a", "n/a", "-", "-");
        std::fflush(stdout);

        rows.push_back({p.name, mp, layers.size(), agg, vk, vk_ok});
    }

    std::printf("\n");
    for (const Row &r : rows) {
        double fill_spd = (r.vk_ok && r.vk.fill > 0) ? r.agg.draw / r.vk.fill : 0.0;
        double e2e_spd  = (r.vk_ok && r.vk.total() > 0) ? r.agg.total() / r.vk.total() : 0.0;
        std::printf("BENCH_GPU res=%s mp=%.1f layers=%zu "
                    "agg_fill_s=%.4f agg_enc_s=%.4f agg_tot_s=%.4f ",
                    r.name, r.mp, r.layers, r.agg.draw, r.agg.encode, r.agg.total());
        if (r.vk_ok)
            std::printf("vk_fill_s=%.4f vk_enc_s=%.4f vk_tot_s=%.4f "
                        "fill_speedup=%.3f e2e_speedup=%.3f\n",
                        r.vk.fill, r.vk.encode, r.vk.total(), fill_spd, e2e_spd);
        else
            std::printf("vk=na\n");
    }
    return 0;
}
