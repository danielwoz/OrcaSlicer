#include "SlaBackendPolicy.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#ifdef SLIC3R_VULKAN
#include "VulkanContext.hpp"
#endif

namespace Slic3r {
namespace sla {

// VkPhysicalDeviceType values, hard-coded here so the PURE core does not pull
// in vulkan.h (it must build in SLIC3R_VULKAN=OFF unit tests). These match the
// stable Vulkan enum and are the same values VulkanContext stores in
// device_type().
namespace {
constexpr uint32_t DEVICE_TYPE_INTEGRATED_GPU = 1; // VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU
constexpr uint32_t DEVICE_TYPE_DISCRETE_GPU   = 2; // VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU

bool is_gpu_device_type(uint32_t t)
{
    // Prefer DISCRETE, accept INTEGRATED. VIRTUAL/CPU/OTHER are not treated as
    // "a GPU worth offloading to" for this gate (they self-exclude), matching
    // the scope's "prefer DISCRETE_GPU; else INTEGRATED".
    return t == DEVICE_TYPE_DISCRETE_GPU || t == DEVICE_TYPE_INTEGRATED_GPU;
}
} // namespace

const char *to_string(BackendReason r)
{
    switch (r) {
    case BackendReason::DisabledByConfig: return "disabled by config/env";
    case BackendReason::ForcedByConfig:   return "forced by config/env (SLA_GPU=1)";
    case BackendReason::NoUsableDevice:   return "no usable Vulkan device";
    case BackendReason::PanelTooLarge:    return "panel exceeds maxImageDimension2D";
    case BackendReason::InsufficientVram: return "device-local VRAM below working-set estimate";
    case BackendReason::BelowThreshold:   return "work below THRESHOLD";
    case BackendReason::AboveThreshold:   return "work at/above THRESHOLD";
    }
    return "?";
}

uint64_t estimate_working_set_bytes(const Resolution &res, const PolicyConfig &cfg)
{
    const double px = double(res.width_px) * double(res.height_px);
    double bytes = px * cfg.bytes_per_pixel_working_set;
    return uint64_t(bytes) + cfg.fixed_overhead_bytes;
}

BackendChoice pick_backend_core(const DeviceCaps  &caps,
                                const Resolution  &res,
                                size_t             layer_count,
                                const PolicyConfig &cfg,
                                BackendReason     *out_reason)
{
    auto done = [&](BackendChoice c, BackendReason r) {
        if (out_reason) *out_reason = r;
        return c;
    };

    // 1. Disabled by config/env -> AGG.
    if (cfg.enable == GpuEnable::Off)
        return done(BackendChoice::AGG, BackendReason::DisabledByConfig);

    // 2. Loader missing / 0 devices / unusable / not a GPU type -> AGG.
    if (!caps.usable || !is_gpu_device_type(caps.device_type))
        return done(BackendChoice::AGG, BackendReason::NoUsableDevice);

    // 3. Best device already selected upstream (DISCRETE preferred); caps reflect
    //    it. (The selection itself lives in VulkanContext::init().)

    // 4. maxImageDimension2D < panel dims -> AGG (can't fit a mask). HARD gate:
    //    applies even in Force mode.
    if (caps.max_image_dim_2d < res.width_px ||
        caps.max_image_dim_2d < res.height_px)
        return done(BackendChoice::AGG, BackendReason::PanelTooLarge);

    // 5. device-local VRAM < working-set estimate -> AGG. HARD gate (Force too).
    {
        const uint64_t need = estimate_working_set_bytes(res, cfg);
        const uint64_t have = uint64_t(double(caps.device_local_vram) *
                                       cfg.vram_safety_fraction);
        if (have < need)
            return done(BackendChoice::AGG, BackendReason::InsufficientVram);
    }

    // Force mode bypasses ONLY the economic work>=THRESHOLD gate (the hard caps
    // above already passed).
    if (cfg.enable == GpuEnable::Force)
        return done(BackendChoice::Vulkan, BackendReason::ForcedByConfig);

    // 6. work = panel_megapixels * layer_count; below THRESHOLD -> AGG.
    const double megapixels = (double(res.width_px) * double(res.height_px)) / 1.0e6;
    const double work = megapixels * double(layer_count);
    if (work < cfg.work_threshold_mp_layers)
        return done(BackendChoice::AGG, BackendReason::BelowThreshold);

    return done(BackendChoice::Vulkan, BackendReason::AboveThreshold);
}

#ifdef SLIC3R_VULKAN

namespace {
// Parse SLA_GPU env: "0"/"off"/"false" -> Off, "1"/"force"/"on" -> Force,
// "auto"/unset/other -> Auto.
GpuEnable parse_env_enable(GpuEnable fallback)
{
    const char *e = std::getenv("SLA_GPU");
    if (!e || !*e) return fallback;
    if (!std::strcmp(e, "0") || !std::strcmp(e, "off") || !std::strcmp(e, "false"))
        return GpuEnable::Off;
    if (!std::strcmp(e, "1") || !std::strcmp(e, "on") || !std::strcmp(e, "force"))
        return GpuEnable::Force;
    return GpuEnable::Auto; // "auto" or anything else
}
} // namespace

BackendChoice pick_backend(const Resolution   &res,
                           size_t              layer_count,
                           const PolicyConfig &cfg_in,
                           BackendReason      *out_reason,
                           std::string        *out_device_name)
{
    PolicyConfig cfg = cfg_in;
    // Env override takes precedence over the passed config's enable field.
    cfg.enable = parse_env_enable(cfg_in.enable);

    DeviceCaps caps;
    // Query the shared context only if not already disabled (avoid spinning up
    // Vulkan when the user turned it off).
    if (cfg.enable != GpuEnable::Off) {
        auto ctx = gpu::VulkanContext::get();
        if (ctx && ctx->is_usable()) {
            caps.usable            = true;
            caps.device_type       = ctx->device_type();
            caps.max_image_dim_2d  = ctx->max_image_dim_2d();
            caps.device_local_vram = ctx->device_local_vram_bytes();
            caps.device_name       = ctx->device_name();
        }
    }
    if (out_device_name) *out_device_name = caps.device_name;

    return pick_backend_core(caps, res, layer_count, cfg, out_reason);
}

#endif // SLIC3R_VULKAN

} // namespace sla
} // namespace Slic3r
