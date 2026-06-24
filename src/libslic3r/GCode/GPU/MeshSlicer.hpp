#ifndef SLIC3R_GCODE_GPU_MESHSLICER_HPP
#define SLIC3R_GCODE_GPU_MESHSLICER_HPP

// GPU (plain Vulkan compute, NO ray tracing) backend for TriangleMeshSlicer's
// slice_make_lines(): the per-(triangle x layer) plane-intersection that emits
// the IntersectionLine segments later stitched into loops.
//
// Mirrors the SeamRayCaster shape: process-wide singleton (get()), async warmup,
// is_usable(), transparent CPU fallback (get() returns nullptr / compute returns
// false), SLIC3R_VULKAN-gated, inert placeholder when OFF. Unlike SeamRayCaster
// this needs NO ray_query / acceleration structures, so it runs on any Vulkan 1.1
// compute device that exposes shaderInt64 + shaderFloat64 (required to reproduce
// the CPU's int64 coord_t + double interpolation bit-for-bit).
//
// The compute shader (slice_facets.comp) replicates slice_facet() exactly and
// atomic-appends produced segments into one flat buffer; compute_lines() reads it
// back once and returns a flat vector of OutLine records (each carries its layer
// index). The caller buckets them per layer and rebuilds IntersectionLines. This
// is the variable-length copy-back the GPU_OFFLOAD_PLAN flagged as the risk.

#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <cstdint>

namespace Slic3r {
namespace mesh_gpu {

// POD record matching slice_facets.comp's OutLine (64 bytes, std430). Public so
// the caller (and the validation harness) can rebuild IntersectionLines from it.
struct GpuSliceLine {
    int64_t  ax, ay, bx, by;      // segment endpoints, coord_t units (int64)
    int32_t  layer;               // slice plane index (into the zs array)
    int32_t  a_id, b_id;          // mesh vertex ids of endpoints, or -1
    int32_t  edge_a_id, edge_b_id;// source mesh edge ids, or -1
    int32_t  edge_type;           // 0=General, 1=Top, 2=Bottom (matches FacetEdgeType)
    int32_t  pad0, pad1;
};
static_assert(sizeof(GpuSliceLine) == 64, "must match std430 OutLine in slice_facets.comp");

#ifdef SLIC3R_VULKAN

struct MeshSlicerImpl;

class MeshSlicer {
public:
    // Process-wide shared instance, created on first call. Returns nullptr if no
    // usable compute device exists (no Vulkan / no int64+fp64). Never throws.
    static std::shared_ptr<MeshSlicer> get();

    // Kick off device + context creation on a detached background thread so the
    // Vulkan/driver init overlaps with model load / early slicing. No-op when the
    // GPU mesh-slice path is disabled (ORCA_MESH_GPU != 1).
    static void warmup_async();

    ~MeshSlicer();
    MeshSlicer(const MeshSlicer &) = delete;
    MeshSlicer &operator=(const MeshSlicer &) = delete;

    bool is_usable() const { return m_usable; }
    const std::string &device_name() const { return m_device_name; }

    // Slice the given (already XY-scaled, Z-unscaled) vertices against the sorted
    // unscaled zs, replicating slice_make_lines()/slice_facet(). On success returns
    // true and fills `out` with the produced segments (unordered; each carries its
    // layer index). Returns false on any failure or output-buffer overflow (caller
    // falls back to CPU). Optionally returns kernel-only and copyback-only times in
    // ms for benchmarking. Thread-safe (serialized internally).
    bool compute_lines(const std::vector<float>    &verts_xyz, // 3 floats/vertex
                       const std::vector<int32_t>  &indices,   // 3 ids/face
                       const std::vector<int32_t>  &edge_ids,  // 3 ids/face
                       const std::vector<float>    &zs,        // sorted ascending
                       std::vector<GpuSliceLine>   &out,
                       double *kernel_ms = nullptr,
                       double *copyback_ms = nullptr,
                       double *upload_ms = nullptr);

private:
    MeshSlicer() = default;
    bool init();
    void destroy();

    std::unique_ptr<MeshSlicerImpl> m_impl;
    bool        m_usable = false;
    std::string m_device_name;
    std::mutex  m_mutex;
};

#else // !SLIC3R_VULKAN

class MeshSlicer {
public:
    static std::shared_ptr<MeshSlicer> get() { return nullptr; }
    static void warmup_async() {}
    bool is_usable() const { return false; }
};

#endif // SLIC3R_VULKAN

} // namespace mesh_gpu
} // namespace Slic3r

#endif // SLIC3R_GCODE_GPU_MESHSLICER_HPP
