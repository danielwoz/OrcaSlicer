#ifndef SLIC3R_GCODE_GPU_SEAMRAYCASTER_HPP
#define SLIC3R_GCODE_GPU_SEAMRAYCASTER_HPP

// GPU (Vulkan VK_KHR_ray_query / RT-core) backend for SeamPlacer's
// seam-visibility ray casting.
//
// SeamPlacer::raycast_visibility() casts N*N rays from each of ~30k surface
// samples against the static mesh to estimate how "visible" each point is. That
// is ~750k first-hit ray queries per object — the one genuinely RT-core-shaped
// hot path in slicing (~13% of slice time).
//
// This class builds a BLAS+TLAS from the mesh once, then dispatches a single
// compute shader (one invocation per sample) that rebuilds the SAME hemisphere
// directions + Frame as the CPU and applies the SAME first-hit + dot<=0 test,
// producing one visibility float per sample. It replicates ONLY the common
// non-negative-volumes branch; the caller falls back to CPU otherwise.
//
// get() returns nullptr when no ray-query capable device exists (e.g. llvmpipe,
// or SLIC3R_VULKAN OFF), so the caller transparently uses the CPU path.

#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <cstdint>

#include <admesh/stl.h>          // indexed_triangle_set
#include "libslic3r/Point.hpp"   // Vec3f

namespace Slic3r {

struct TriangleSetSamples; // libslic3r/TriangleSetSampling.hpp

namespace seam_gpu {

#ifdef SLIC3R_VULKAN

struct SeamRayCasterImpl; // defined in SeamRayCaster.cpp (holds all Vulkan state)

class SeamRayCaster {
public:
    // Process-wide shared instance, created on first call. Returns nullptr if no
    // usable ray-query device exists. Never throws.
    static std::shared_ptr<SeamRayCaster> get();

    // Kick off device + context creation on a detached background thread so the
    // (potentially several-hundred-ms) Vulkan/driver init overlaps with model load
    // and the early slicing stages and is hidden by the time seam visibility runs.
    // Safe to call multiple times / when GPU seam is disabled (then it's a no-op).
    static void warmup_async();

    ~SeamRayCaster();
    SeamRayCaster(const SeamRayCaster &) = delete;
    SeamRayCaster &operator=(const SeamRayCaster &) = delete;

    bool is_usable() const { return m_usable; }
    const std::string &device_name() const { return m_device_name; }

    // Compute per-sample visibility on the GPU, matching the non-negative branch
    // of raycast_visibility(). sqr_rays = N (total rays per sample = N*N).
    // ray_origin_offset matches the CPU's 0.01 surface bump. Returns true and
    // fills `out` (one float per sample) on success; false on any failure (the
    // caller then falls back to CPU). Thread-safe (serialized internally).
    bool compute_visibility(const indexed_triangle_set &mesh,
                            const TriangleSetSamples    &samples,
                            uint32_t                     sqr_rays,
                            float                        ray_origin_offset,
                            std::vector<float>          &out);

private:
    SeamRayCaster() = default;
    bool init();
    void destroy();

    std::unique_ptr<SeamRayCasterImpl> m_impl;

    bool        m_usable = false;
    std::string m_device_name;
    std::mutex  m_mutex;
};

#else // !SLIC3R_VULKAN

// Inert placeholder when the option is OFF.
class SeamRayCaster {
public:
    static std::shared_ptr<SeamRayCaster> get() { return nullptr; }
    static void warmup_async() {}
    bool is_usable() const { return false; }
};

#endif // SLIC3R_VULKAN

} // namespace seam_gpu
} // namespace Slic3r

#endif // SLIC3R_GCODE_GPU_SEAMRAYCASTER_HPP
