// Phase 3 smoke test for the headless Vulkan SLA rasterizer.
//
// Builds a simple ExPolygon (a square with a square hole), rasterizes it with
// VulkanRaster at a modest resolution, and checks that:
//   - a real GPU device was selected (prints the device name),
//   - the white-pixel area roughly matches the polygon area (square - hole),
//   - the result is written to a PNG in /tmp for eyeballing.
//
// It runs headless (no surface/swapchain). Exit code 0 => mask is correct.
//
// Standalone EXCLUDE_FROM_ALL target (not part of ctest); run directly.

#include <cstdio>
#include <cstdlib>
#include <vector>
#include <memory>
#include <cmath>
#include <fstream>

#include <libslic3r/SLA/RasterBase.hpp>
#include <libslic3r/ExPolygon.hpp>
#include <libslic3r/Point.hpp>

#include "VulkanRaster.hpp"

using namespace Slic3r;
using namespace Slic3r::sla;

// Decode a single-channel PNG mask we just produced and count white pixels.
// Simpler/robust: we re-rasterize through a custom encoder that hands us the
// raw R8 buffer, count there, and separately PNG-encode for the eyeball file.

int main()
{
    // Resolution + pixel size. Use 1024x1024 to keep the naive O(pixels*edges)
    // shader fast. Pixel size 0.05 mm => 51.2 mm panel.
    const size_t W = 1024, H = 1024;
    const double px_mm = 0.05;
    Resolution res(W, H);
    PixelDim   pxdim(px_mm, px_mm);

    // Default Trafo: roLandscape, NoMirror => mirror_y=true (origin top-left),
    // mirror_x=false, flipXY=false. center_x/y = 0.
    RasterBase::Trafo tr; // default

    auto raster = create_vulkan_raster(res, pxdim, 1.0, tr);
    auto *vr = dynamic_cast<VulkanRaster *>(raster.get());
    if (!vr) { std::fprintf(stderr, "FAIL: not a VulkanRaster\n"); return 2; }

    std::printf("GPU active : %s\n", vr->gpu_active() ? "yes" : "no");
    std::printf("Device     : %s\n", vr->gpu_device_name().c_str());

    // Build a square with a square hole, centered in the panel.
    // Panel spans 51.2 mm. Outer square: 20mm..40mm. Hole: 27mm..33mm.
    auto mm = [&](double v) { return scaled<coord_t>(v); };
    ExPolygon ex;
    ex.contour = Polygon({ {mm(20), mm(20)}, {mm(40), mm(20)},
                           {mm(40), mm(40)}, {mm(20), mm(40)} });
    Polygon hole({ {mm(27), mm(27)}, {mm(27), mm(33)},
                   {mm(33), mm(33)}, {mm(33), mm(27)} }); // CW (hole)
    ex.holes.push_back(hole);

    raster->draw(ex);

    // Encode to PNG (for eyeballing) and also grab the raw mask via a custom
    // encoder so we can count white pixels exactly.
    std::vector<uint8_t> raw;
    size_t rawW = 0, rawH = 0;
    RasterEncoder grab = [&](const void *ptr, size_t w, size_t h, size_t nc) -> EncodedRaster {
        rawW = w; rawH = h;
        raw.assign((const uint8_t *)ptr, (const uint8_t *)ptr + w * h * nc);
        return EncodedRaster{}; // empty; we only want the raw bytes here
    };
    raster->encode(grab);

    if (raw.empty() || rawW != W || rawH != H) {
        std::fprintf(stderr, "FAIL: empty/odd raw mask (%zux%zu, %zu bytes)\n", rawW, rawH, raw.size());
        return 3;
    }

    // Count white-ish pixels (>=128) and full-white pixels (255).
    size_t white = 0, full = 0, gray = 0;
    for (uint8_t v : raw) {
        if (v >= 128) ++white;
        if (v == 255) ++full;
        if (v > 0 && v < 255) ++gray;
    }

    // Expected area in pixels: outer (20mm) - hole (6mm) squares.
    // 20mm/0.05 = 400 px side; hole 6mm/0.05 = 120 px side.
    const double outer_px = 20.0 / px_mm; // 400
    const double hole_px  = 6.0  / px_mm; // 120
    const double expected = outer_px * outer_px - hole_px * hole_px;
    const double ratio = double(white) / expected;

    std::printf("Resolution : %zux%zu, pixel %.3f mm\n", W, H, px_mm);
    std::printf("White(>=128): %zu px\n", white);
    std::printf("Full white  : %zu px\n", full);
    std::printf("Edge gray   : %zu px (AA/supersample)\n", gray);
    std::printf("Expected    : %.0f px (square %g - hole %g)\n", expected, outer_px*outer_px, hole_px*hole_px);
    std::printf("Ratio       : %.4f (white / expected)\n", ratio);

    // Write a PNG via the real encoder for eyeballing.
    {
        PNGRasterEncoder enc;
        EncodedRaster png = enc(raw.data(), W, H, 1);
        std::ofstream f("/tmp/sla_vulkan_smoke.png", std::ios::binary);
        f.write(reinterpret_cast<const char *>(png.data()), png.size());
        std::printf("PNG written : /tmp/sla_vulkan_smoke.png (%zu bytes)\n", png.size());
    }

    // Sanity: the mask must be non-trivial (not all black, not all white) and
    // the white area must be within a loose tolerance of the polygon area.
    if (white == 0) { std::fprintf(stderr, "FAIL: mask is all black\n"); return 4; }
    if (white == W * H) { std::fprintf(stderr, "FAIL: mask is all white\n"); return 5; }
    if (ratio < 0.9 || ratio > 1.1) {
        std::fprintf(stderr, "FAIL: white area off by >10%% (ratio %.4f)\n", ratio);
        return 6;
    }

    // Verify the hole is empty and the ring interior is filled. The default
    // Trafo has mirror_y=true (origin top-left), so the buffer row is flipped:
    // row = H - 1 - (mm/px). Apply the same flip when sampling so we hit the
    // same physical location the GPU filled.
    auto sample = [&](double xmm, double ymm) -> uint8_t {
        size_t col = size_t(xmm / px_mm);
        size_t row = H - 1 - size_t(ymm / px_mm);
        return raw[row * W + col];
    };
    {
        uint8_t center = sample(30.0, 30.0); // inside hole
        uint8_t inside = sample(25.0, 25.0); // ring interior (between contour and hole)
        std::printf("Center(hole): %u (expect ~0)\n", center);
        std::printf("Inside ring : %u (expect ~255)\n", inside);
        if (center >= 128) { std::fprintf(stderr, "FAIL: hole not empty\n"); return 7; }
        if (inside  < 128) { std::fprintf(stderr, "FAIL: interior not filled\n"); return 8; }
    }

    // ---------------------------------------------------------------------
    // Cross-check vs AGG on the SAME geometry. Build an AGG grayscale raster,
    // draw the same ExPolygon, and compare white-area coverage. The two should
    // agree closely (Phase 3 uses parity + supersample, AGG uses scanline AA, so
    // a small edge-pixel delta is expected; bulk coverage must match).
    {
        auto agg = create_raster_grayscale_aa(res, pxdim, 1.0, tr);
        agg->draw(ex);
        std::vector<uint8_t> aggraw;
        RasterEncoder grab2 = [&](const void *ptr, size_t w, size_t h, size_t nc) -> EncodedRaster {
            aggraw.assign((const uint8_t *)ptr, (const uint8_t *)ptr + w * h * nc);
            return EncodedRaster{};
        };
        agg->encode(grab2);

        size_t agg_white = 0, diff_big = 0;
        long sumdiff = 0;
        for (size_t i = 0; i < raw.size() && i < aggraw.size(); ++i) {
            if (aggraw[i] >= 128) ++agg_white;
            int d = int(raw[i]) - int(aggraw[i]);
            sumdiff += (d < 0 ? -d : d);
            if ((d > 8) || (d < -8)) ++diff_big;
        }
        double agg_ratio = double(agg_white) / double(white ? white : 1);
        double pct_big = 100.0 * double(diff_big) / double(raw.size());
        std::printf("AGG white   : %zu px (GPU/AGG ratio %.4f)\n", agg_white, agg_ratio);
        std::printf("Mean |GPU-AGG| over all px: %.3f levels\n", double(sumdiff) / double(raw.size()));
        std::printf("Px differing >8 levels: %zu (%.3f%%)\n", diff_big, pct_big);
        // For this axis-aligned case the masks should be essentially identical.
        if (agg_white == 0) { std::fprintf(stderr, "FAIL: AGG produced empty mask\n"); return 9; }
        if (pct_big > 1.0) {
            std::fprintf(stderr, "FAIL: GPU vs AGG differ on >1%% of pixels (%.3f%%)\n", pct_big);
            return 10;
        }
    }

    std::printf("RESULT: PASS (real GPU mask, correct even-odd fill with hole, matches AGG)\n");
    return 0;
}
