// Phase 5: unit tests for the SLA GPU-vs-CPU runtime selection policy
// (pick_backend_core, SlaBackendPolicy.hpp). These exercise the PURE decision
// function with SYNTHETIC DeviceCaps, so they require NO GPU and always run
// (they are compiled into sla_print_tests only when SLIC3R_VULKAN is ON, since
// that is when libslic3r_sla_gpu -- which provides the policy -- is linked).
//
// Covers, matching the scope pseudocode order:
//   disabled -> AGG, no-device -> AGG, panel-too-big -> AGG,
//   insufficient-VRAM -> AGG, below-threshold -> AGG, above-threshold -> Vulkan,
//   plus the SLA_GPU=force override and the hard-gate precedence.

#include <catch2/catch_all.hpp>

#ifdef SLIC3R_VULKAN

#include "libslic3r/SLA/GPU/SlaBackendPolicy.hpp"

using namespace Slic3r;
using namespace Slic3r::sla;

namespace {
// A capable discrete GPU (RTX-3090-ish): big image limit, 24 GiB VRAM.
DeviceCaps good_gpu()
{
    DeviceCaps c;
    c.usable            = true;
    c.device_type       = 2;                      // VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU
    c.max_image_dim_2d  = 32768;                  // NVIDIA
    c.device_local_vram = 24ull * 1024 * 1024 * 1024;
    c.device_name       = "Test Discrete GPU";
    return c;
}

// A config whose threshold is finite & small, so we can test the economic gate
// (the production default threshold is a "never" sentinel; tests set their own).
PolicyConfig cfg_with_threshold(double thr)
{
    PolicyConfig cfg;
    cfg.enable = GpuEnable::Auto;
    cfg.work_threshold_mp_layers = thr;
    return cfg;
}
} // namespace

TEST_CASE("pick_backend: disabled by config -> AGG", "[SLA][Vulkan][policy]")
{
    PolicyConfig cfg = cfg_with_threshold(1.0);
    cfg.enable = GpuEnable::Off;
    Resolution res(7680, 4320);
    BackendReason why;
    REQUIRE(pick_backend_core(good_gpu(), res, 1000, cfg, &why) == BackendChoice::AGG);
    REQUIRE(why == BackendReason::DisabledByConfig);
}

TEST_CASE("pick_backend: no usable device -> AGG", "[SLA][Vulkan][policy]")
{
    PolicyConfig cfg = cfg_with_threshold(1.0); // small threshold: would pass otherwise
    Resolution res(7680, 4320);

    SECTION("loader missing / 0 devices (usable=false)") {
        DeviceCaps c = good_gpu(); c.usable = false;
        BackendReason why;
        REQUIRE(pick_backend_core(c, res, 1000, cfg, &why) == BackendChoice::AGG);
        REQUIRE(why == BackendReason::NoUsableDevice);
    }
    SECTION("usable but CPU/SOFTWARE device type self-excludes") {
        DeviceCaps c = good_gpu(); c.device_type = 4; // VK_PHYSICAL_DEVICE_TYPE_CPU
        BackendReason why;
        REQUIRE(pick_backend_core(c, res, 1000, cfg, &why) == BackendChoice::AGG);
        REQUIRE(why == BackendReason::NoUsableDevice);
    }
}

TEST_CASE("pick_backend: panel exceeds maxImageDimension2D -> AGG", "[SLA][Vulkan][policy]")
{
    PolicyConfig cfg = cfg_with_threshold(1.0);
    DeviceCaps c = good_gpu();
    c.max_image_dim_2d = 8192; // too small for an 11520-wide 12K panel
    Resolution res(11520, 5120);
    BackendReason why;
    REQUIRE(pick_backend_core(c, res, 1000, cfg, &why) == BackendChoice::AGG);
    REQUIRE(why == BackendReason::PanelTooLarge);
}

TEST_CASE("pick_backend: insufficient VRAM -> AGG", "[SLA][Vulkan][policy]")
{
    PolicyConfig cfg = cfg_with_threshold(1.0);
    DeviceCaps c = good_gpu();
    c.device_local_vram = 16ull * 1024 * 1024; // 16 MiB -- below a 12K working set
    Resolution res(11520, 5120);
    BackendReason why;
    REQUIRE(pick_backend_core(c, res, 1000, cfg, &why) == BackendChoice::AGG);
    REQUIRE(why == BackendReason::InsufficientVram);
}

TEST_CASE("pick_backend: below threshold -> AGG", "[SLA][Vulkan][policy]")
{
    // 4K = 9.216 MP; 5 layers -> work = 46.08. Threshold 100 -> below.
    PolicyConfig cfg = cfg_with_threshold(100.0);
    Resolution res(3840, 2400);
    BackendReason why;
    REQUIRE(pick_backend_core(good_gpu(), res, 5, cfg, &why) == BackendChoice::AGG);
    REQUIRE(why == BackendReason::BelowThreshold);
}

TEST_CASE("pick_backend: at/above threshold -> Vulkan", "[SLA][Vulkan][policy]")
{
    // 8K = 33.18 MP; 1000 layers -> work = 33177.6. Threshold 100 -> above.
    PolicyConfig cfg = cfg_with_threshold(100.0);
    Resolution res(7680, 4320);
    BackendReason why;
    REQUIRE(pick_backend_core(good_gpu(), res, 1000, cfg, &why) == BackendChoice::Vulkan);
    REQUIRE(why == BackendReason::AboveThreshold);
}

TEST_CASE("pick_backend: production default threshold keeps Auto on AGG", "[SLA][Vulkan][policy]")
{
    // The shipped default (work_threshold_mp_layers = 1e18 sentinel) means the
    // naive fill-only path never wins end-to-end, so Auto stays on AGG even for
    // a huge 12K / 2000-layer job (~118000 MP-layers << 1e18).
    PolicyConfig cfg; // defaults
    Resolution res(11520, 5120);
    BackendReason why;
    REQUIRE(pick_backend_core(good_gpu(), res, 2000, cfg, &why) == BackendChoice::AGG);
    REQUIRE(why == BackendReason::BelowThreshold);
}

TEST_CASE("pick_backend: SLA_GPU=force bypasses the work gate but not hard caps",
          "[SLA][Vulkan][policy]")
{
    PolicyConfig cfg; // default (sentinel) threshold -- Auto would pick AGG
    cfg.enable = GpuEnable::Force;
    Resolution res(3840, 2400);

    SECTION("force + capable device -> Vulkan despite tiny job") {
        BackendReason why;
        REQUIRE(pick_backend_core(good_gpu(), res, 1, cfg, &why) == BackendChoice::Vulkan);
        REQUIRE(why == BackendReason::ForcedByConfig);
    }
    SECTION("force still rejected if panel too large") {
        DeviceCaps c = good_gpu(); c.max_image_dim_2d = 2048;
        BackendReason why;
        REQUIRE(pick_backend_core(c, res, 1, cfg, &why) == BackendChoice::AGG);
        REQUIRE(why == BackendReason::PanelTooLarge);
    }
    SECTION("force still rejected if VRAM too small") {
        DeviceCaps c = good_gpu(); c.device_local_vram = 1024 * 1024;
        BackendReason why;
        REQUIRE(pick_backend_core(c, res, 1, cfg, &why) == BackendChoice::AGG);
        REQUIRE(why == BackendReason::InsufficientVram);
    }
}

TEST_CASE("pick_backend: working-set estimate scales with panel", "[SLA][Vulkan][policy]")
{
    PolicyConfig cfg; // defaults
    // 12K panel: 11520*5120 = 58.98 Mpx * 8 B/px = ~472 MB + 64 MB overhead.
    uint64_t ws12k = estimate_working_set_bytes(Resolution(11520, 5120), cfg);
    uint64_t ws4k  = estimate_working_set_bytes(Resolution(3840, 2400), cfg);
    REQUIRE(ws12k > ws4k);
    REQUIRE(ws12k > 400ull * 1024 * 1024);
    REQUIRE(ws12k < 1024ull * 1024 * 1024); // well under a GB -- fits any real GPU
}

#endif // SLIC3R_VULKAN
