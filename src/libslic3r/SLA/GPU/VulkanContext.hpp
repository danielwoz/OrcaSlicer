#ifndef SLA_GPU_VULKANCONTEXT_HPP
#define SLA_GPU_VULKANCONTEXT_HPP

#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <cstdint>

// Phase 3: real headless Vulkan compute context.
//
//   - VkInstance (no surface/swapchain, compute + offscreen only)
//   - best VkPhysicalDevice (DISCRETE_GPU preferred, else INTEGRATED, else any)
//   - VkDevice with one compute-capable queue
//   - command pool, descriptor set layout, compute pipeline from embedded SPIR-V
//
// Created once and shared across the parallel draw_layers loop. For Phase 3 the
// per-dispatch path is serialized with a mutex (correct, not yet optimal); the
// concurrency redesign (per-thread command buffers / pooled descriptors) is
// noted for Phase 5.
//
// Construction never throws on a no-GPU machine: instead get() returns nullptr
// (or the context reports !is_usable()) so VulkanRaster falls back to AGG.

#ifdef SLIC3R_VULKAN
#include <vulkan/vulkan.h>
#endif

namespace Slic3r {
namespace sla {
namespace gpu {

#ifdef SLIC3R_VULKAN

// One float edge = (x0, y0, x1, y1) in pixel space.
struct Edge { float x0, y0, x1, y1; };

class VulkanContext {
public:
    // Returns the process-wide shared context, creating it on first call.
    // Returns nullptr if no usable Vulkan device exists (caller falls back to
    // AGG). Never throws.
    static std::shared_ptr<VulkanContext> get();

    ~VulkanContext();

    VulkanContext(const VulkanContext &) = delete;
    VulkanContext &operator=(const VulkanContext &) = delete;

    bool is_usable() const { return m_usable; }
    const std::string &device_name() const { return m_device_name; }
    uint32_t device_type() const { return m_device_type; } // VkPhysicalDeviceType

    // Device capability accessors exposed for the Phase-5 runtime selection
    // policy (pick_backend, SlaBackendPolicy.hpp). Populated during init() from
    // VkPhysicalDeviceProperties / VkPhysicalDeviceMemoryProperties.
    //   - max_image_dim_2d():     props.limits.maxImageDimension2D (px)
    //   - device_local_vram_bytes(): sum of DEVICE_LOCAL heaps (bytes)
    uint32_t      max_image_dim_2d() const { return m_max_image_dim_2d; }
    uint64_t      device_local_vram_bytes() const { return m_device_local_vram; }

    // Rasterize an edge list into an R8 grayscale mask of size width*height.
    // edges are in pixel space (top-left origin), already transformed by the
    // caller to match AGGRaster. ss = supersample factor per axis (>=1).
    // Returns true on success and fills `out` with width*height bytes (0/255 or
    // box-averaged coverage). Thread-safe (serialized internally).
    bool rasterize(const std::vector<Edge> &edges,
                   uint32_t width, uint32_t height, uint32_t ss,
                   std::vector<uint8_t> &out);

private:
    VulkanContext() = default;
    bool init();
    void destroy();

    uint32_t find_memory_type(uint32_t type_bits, VkMemoryPropertyFlags props) const;
    bool create_buffer(VkDeviceSize size, VkBufferUsageFlags usage,
                       VkMemoryPropertyFlags props,
                       VkBuffer &buf, VkDeviceMemory &mem) const;

    bool        m_usable = false;
    std::string m_device_name;
    uint32_t    m_device_type = 0;
    uint32_t    m_max_image_dim_2d = 0; // props.limits.maxImageDimension2D
    uint64_t    m_device_local_vram = 0; // sum of DEVICE_LOCAL heap sizes (bytes)

    VkInstance       m_instance = VK_NULL_HANDLE;
    VkPhysicalDevice m_phys     = VK_NULL_HANDLE;
    VkDevice         m_device   = VK_NULL_HANDLE;
    uint32_t         m_queue_family = 0;
    VkQueue          m_queue    = VK_NULL_HANDLE;
    VkCommandPool    m_cmd_pool = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_dset_layout = VK_NULL_HANDLE;
    VkPipelineLayout m_pipe_layout = VK_NULL_HANDLE;
    VkPipeline       m_pipeline = VK_NULL_HANDLE;
    VkShaderModule   m_shader   = VK_NULL_HANDLE;

    VkPhysicalDeviceMemoryProperties m_mem_props{};

    std::mutex m_mutex; // serialize dispatches (Phase 3 simplicity)
};

#else  // !SLIC3R_VULKAN

// Inert placeholder when the option is OFF.
class VulkanContext {
public:
    VulkanContext() = default;
    ~VulkanContext() = default;
    VulkanContext(const VulkanContext &) = delete;
    VulkanContext &operator=(const VulkanContext &) = delete;
};

#endif // SLIC3R_VULKAN

} // namespace gpu
} // namespace sla
} // namespace Slic3r

#endif // SLA_GPU_VULKANCONTEXT_HPP
