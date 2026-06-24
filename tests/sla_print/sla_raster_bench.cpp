// SLA rasterization micro-benchmark (AGG / CPU baseline).
//
// Phase 1 of the GPU SLA rasterization project: establish the AGG-baseline
// rasterize-step wall-time across SLA panel resolutions (4K -> 8K -> 12K).
//
// This does NOT go through OrcaSlicer's CLI (the headless SLA end-to-end path
// is disabled in src/OrcaSlicer.cpp). Instead it exercises the real libslic3r
// SLA rasterization seam directly:
//
//     sla::RasterGrayscaleAAGammaPower raster{res, pxdim, trafo, gamma};
//     for (poly : layer) raster.draw(poly);
//     raster.encode(sla::PNGRasterEncoder{});
//
// which is exactly the per-layer work performed by SLAPrint::Steps::rasterize()
// (src/libslic3r/SLAPrintSteps.cpp:1077). Layer geometry is produced by slicing
// a real mesh from resources/handy_models/*.drc with the same TriangleMeshSlicer
// the production pipeline uses.
//
// Build:  cmake --build build --config Release --target sla_raster_bench -j32
// Run:    build/tests/sla_print/Release/sla_raster_bench [mesh.drc] [layers] [runs]
//
// This is a MANUAL benchmark; it is intentionally NOT registered with ctest.

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

using namespace Slic3r;

namespace {

using Clock = std::chrono::steady_clock;
static inline double secs(Clock::time_point a, Clock::time_point b)
{
    return std::chrono::duration<double>(b - a).count();
}

struct PanelSpec {
    const char *name;
    size_t      px;       // pixels in X
    size_t      py;       // pixels in Y
    double      w_mm;     // physical panel width  (X)
    double      h_mm;     // physical panel height (Y)
};

// Representative resin panels. Physical sizes are realistic monochrome-LCD
// panel dimensions for each pixel count (aspect ratios match shipping printers,
// e.g. an 8K ~10" mono panel). PixelDim = physical / pixels feeds the raster so
// the AGG fill cost reflects real pixel pitch.
const PanelSpec PANELS[] = {
    {"4K",  3840, 2400, 192.0, 120.0},   //  9.2 MP
    {"8K",  7680, 4320, 218.88, 123.12}, // 33.2 MP
    {"12K", 11520, 5120, 230.4, 102.4},  // 59.0 MP
};

// Generate per-layer ExPolygon geometry from a real mesh. The mesh is centered
// at XY origin and uniformly scaled so its XY footprint fills ~70% of the
// SMALLEST panel's short physical axis, guaranteeing meaningful fill on every
// panel. Returns the slices (in scaled/coord_t mesh coordinates, centered at 0).
std::vector<ExPolygons> make_layers(const TriangleMesh &mesh_in, size_t want_layers)
{
    TriangleMesh mesh = mesh_in;

    BoundingBoxf3 bb = mesh.bounding_box();
    Vec3d  size   = bb.size();
    Vec3d  center = bb.center();

    // Center the mesh at the origin in XY and rest it on z=0.
    mesh.translate(-float(center.x()), -float(center.y()), -float(bb.min.z()));

    // Scale XY footprint to ~70% of the smallest panel short axis (102.4 mm for
    // the 12K panel above). This keeps the model inside every panel.
    const double target_mm = 0.70 * 102.4;
    double fp = std::max(size.x(), size.y());
    if (fp > 0) {
        double s = target_mm / fp;
        mesh.scale(float(s));
    }

    bb = mesh.bounding_box();
    double zmin = bb.min.z(), zmax = bb.max.z();
    double height = std::max(1e-3, zmax - zmin);

    // Choose a layer height that yields ~want_layers layers.
    float layer_h = float(height / double(want_layers));
    if (layer_h <= 0) layer_h = 0.05f;

    std::vector<float> zs = grid(float(zmin + layer_h / 2.f), float(zmax), layer_h);

    std::vector<ExPolygons> slices = slice_mesh_ex(mesh.its, zs, 0.005f);
    return slices;
}

// Total contour+hole vertex count across all layers (a proxy for fill work).
size_t total_vertices(const std::vector<ExPolygons> &layers)
{
    size_t n = 0;
    for (const ExPolygons &layer : layers)
        for (const ExPolygon &ep : layer) {
            n += ep.contour.points.size();
            for (const Polygon &h : ep.holes) n += h.points.size();
        }
    return n;
}

struct RunTimes { double draw = 0, encode = 0; double total() const { return draw + encode; } };

// Rasterize all layers once at a given panel spec; report draw and encode time.
// Mirrors SLAPrint::Steps::rasterize(): one raster per layer, draw all polys,
// then encode to PNG. Run serially here so the per-stage split is clean (the
// production code runs layers in parallel across TBB; per-layer cost is the
// same, this just measures it deterministically).
RunTimes rasterize_all(const std::vector<ExPolygons> &layers, const PanelSpec &p)
{
    sla::Resolution res{p.px, p.py};
    sla::PixelDim   pxdim{p.w_mm / double(p.px), p.h_mm / double(p.py)};

    // Place the panel origin so the (origin-centered) model maps to panel center.
    sla::RasterBase::Trafo tr{sla::RasterBase::roLandscape, sla::RasterBase::NoMirror};
    BoundingBox panel_bb({0, 0}, {scaled(p.w_mm), scaled(p.h_mm)});
    tr.center_x = panel_bb.center().x();
    tr.center_y = panel_bb.center().y();

    const double gamma = 1.0;

    RunTimes t;
    // Keep encoded outputs alive briefly so encode() isn't optimized away.
    volatile size_t sink = 0;

    for (const ExPolygons &layer : layers) {
        sla::RasterGrayscaleAAGammaPower raster{res, pxdim, tr, gamma};

        auto d0 = Clock::now();
        for (const ExPolygon &poly : layer)
            raster.draw(poly);
        auto d1 = Clock::now();
        t.draw += secs(d0, d1);

        auto e0 = Clock::now();
        sla::EncodedRaster enc = raster.encode(sla::PNGRasterEncoder{});
        auto e1 = Clock::now();
        t.encode += secs(e0, e1);

        sink += enc.size();
    }
    (void) sink;
    return t;
}

} // namespace

int main(int argc, char **argv)
{
    // Args: [mesh.drc] [layers] [runs]
#ifndef BENCH_MODELS_DIR
#define BENCH_MODELS_DIR "resources/handy_models"
#endif
    std::string mesh_path =
        (argc > 1) ? argv[1]
                   : std::string(BENCH_MODELS_DIR) + "/OrcaCube_v2.drc";
    size_t want_layers = (argc > 2) ? size_t(std::strtoul(argv[2], nullptr, 10)) : 200;
    int    runs        = (argc > 3) ? std::atoi(argv[3]) : 5;
    if (runs < 1) runs = 1;

    std::printf("SLA raster benchmark (AGG / CPU baseline)\n");
    std::printf("mesh    : %s\n", mesh_path.c_str());

    TriangleMesh mesh;
    bool loaded = load_drc(mesh_path.c_str(), &mesh);
    if (!loaded || mesh.empty()) {
        std::fprintf(stderr, "ERROR: failed to load mesh '%s'\n", mesh_path.c_str());
        return 2;
    }

    std::vector<ExPolygons> layers = make_layers(mesh, want_layers);
    if (layers.empty()) {
        std::fprintf(stderr, "ERROR: slicing produced 0 layers\n");
        return 3;
    }

    size_t nonempty = 0;
    for (const ExPolygons &l : layers) if (!l.empty()) ++nonempty;
    size_t verts = total_vertices(layers);

    std::printf("layers  : %zu (%zu non-empty), ~%zu contour vertices total\n",
                layers.size(), nonempty, verts);
    std::printf("runs    : %d (min reported), +1 warmup discarded\n\n", runs);

    std::printf("%-5s %9s %8s %12s %12s %10s %10s %8s\n",
                "res", "MP", "layers", "min_total_s", "perlayer_ms",
                "draw_s", "encode_s", "draw%%");
    std::printf("--------------------------------------------------------------------------------\n");

    struct Result {
        const char *name; double mp; double min_total; double draw; double encode;
    };
    std::vector<Result> results;

    for (const PanelSpec &p : PANELS) {
        double mp = double(p.px) * double(p.py) / 1.0e6;

        // Warmup (discarded).
        rasterize_all(layers, p);

        double best_total = 1e300;
        RunTimes best{};
        for (int i = 0; i < runs; ++i) {
            RunTimes t = rasterize_all(layers, p);
            if (t.total() < best_total) {
                best_total = t.total();
                best = t;
            }
        }

        double perlayer_ms = best_total / double(layers.size()) * 1000.0;
        double drawpct = best.total() > 0 ? 100.0 * best.draw / best.total() : 0.0;

        std::printf("%-5s %9.1f %8zu %12.3f %12.3f %10.3f %10.3f %7.1f\n",
                    p.name, mp, layers.size(), best_total, perlayer_ms,
                    best.draw, best.encode, drawpct);

        results.push_back({p.name, mp, best_total, best.draw, best.encode});
    }

    std::printf("\n");
    for (const Result &r : results) {
        double drawpct = (r.draw + r.encode) > 0 ? 100.0 * r.draw / (r.draw + r.encode) : 0.0;
        double perlayer_ms = r.min_total / double(layers.size()) * 1000.0;
        std::printf("BENCH_RESULT res=%s mp=%.1f layers=%zu min_s=%.3f perlayer_ms=%.3f "
                    "draw_s=%.3f encode_s=%.3f draw_pct=%.1f\n",
                    r.name, r.mp, layers.size(), r.min_total, perlayer_ms,
                    r.draw, r.encode, drawpct);
    }

    return 0;
}
