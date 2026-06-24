#include "VulkanContext.hpp"

#ifdef SLIC3R_VULKAN

#include <cstring>
#include <cstdio>
#include <algorithm>

#include "fill_evenodd_spv.hpp"

namespace Slic3r {
namespace sla {
namespace gpu {

namespace {

// Push constant block, must match fill_evenodd.comp.
struct PushConsts {
    uint32_t width;
    uint32_t height;
    uint32_t edge_count;
    uint32_t ss;
};

// Score a physical device for selection: DISCRETE > INTEGRATED > VIRTUAL > CPU.
int device_score(VkPhysicalDeviceType t)
{
    switch (t) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return 4;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 3;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return 2;
    case VK_PHYSICAL_DEVICE_TYPE_CPU:            return 1;
    default:                                     return 0;
    }
}

} // namespace

std::shared_ptr<VulkanContext> VulkanContext::get()
{
    // Process-wide shared singleton. Created on first use, reused across the
    // parallel per-layer loop. Thread-safe static init.
    static std::shared_ptr<VulkanContext> instance = [] {
        std::shared_ptr<VulkanContext> ctx(new VulkanContext());
        if (!ctx->init()) {
            ctx->destroy();
            // keep the (unusable) object so callers can query is_usable();
            // but return it as-is — is_usable() is false -> AGG fallback.
        }
        return ctx;
    }();
    return instance;
}

VulkanContext::~VulkanContext() { destroy(); }

bool VulkanContext::init()
{
    // ---- Instance (no surface/swapchain) --------------------------------
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "OrcaSlicer-SLA-GPU";
    app.apiVersion = VK_API_VERSION_1_1; // safe on lavapipe 1.3 + NVIDIA

    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;

    if (vkCreateInstance(&ici, nullptr, &m_instance) != VK_SUCCESS) {
        m_instance = VK_NULL_HANDLE;
        return false;
    }

    // ---- Physical device selection --------------------------------------
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(m_instance, &n, nullptr);
    if (n == 0)
        return false;
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(m_instance, &n, devs.data());

    int best = -1, best_score = -1;
    uint32_t best_qf = 0;
    for (uint32_t i = 0; i < n; ++i) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(devs[i], &props);

        // need a compute-capable queue family
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qf(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &qn, qf.data());
        int qfi = -1;
        for (uint32_t q = 0; q < qn; ++q) {
            if (qf[q].queueFlags & VK_QUEUE_COMPUTE_BIT) { qfi = (int)q; break; }
        }
        if (qfi < 0)
            continue;

        int sc = device_score(props.deviceType);
        if (sc > best_score) {
            best_score = sc;
            best = (int)i;
            best_qf = (uint32_t)qfi;
        }
    }
    if (best < 0)
        return false;

    m_phys = devs[best];
    m_queue_family = best_qf;

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(m_phys, &props);
    m_device_name = props.deviceName;
    m_device_type = (uint32_t)props.deviceType;
    m_max_image_dim_2d = props.limits.maxImageDimension2D;
    vkGetPhysicalDeviceMemoryProperties(m_phys, &m_mem_props);

    // Total DEVICE_LOCAL VRAM = sum of heaps flagged VK_MEMORY_HEAP_DEVICE_LOCAL_BIT.
    // On a discrete GPU this is the card's VRAM; on an integrated GPU it is the
    // device-local portion of system RAM. Used by pick_backend() to reject jobs
    // whose working-set estimate would not fit.
    m_device_local_vram = 0;
    for (uint32_t h = 0; h < m_mem_props.memoryHeapCount; ++h) {
        if (m_mem_props.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            m_device_local_vram += m_mem_props.memoryHeaps[h].size;
    }

    // ---- Logical device + compute queue ---------------------------------
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = m_queue_family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;

    if (vkCreateDevice(m_phys, &dci, nullptr, &m_device) != VK_SUCCESS) {
        m_device = VK_NULL_HANDLE;
        return false;
    }
    vkGetDeviceQueue(m_device, m_queue_family, 0, &m_queue);

    // ---- Command pool ---------------------------------------------------
    VkCommandPoolCreateInfo cpci{};
    cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpci.queueFamilyIndex = m_queue_family;
    if (vkCreateCommandPool(m_device, &cpci, nullptr, &m_cmd_pool) != VK_SUCCESS)
        return false;

    // ---- Shader module from embedded SPIR-V -----------------------------
    VkShaderModuleCreateInfo smci{};
    smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.codeSize = fill_evenodd_spv_size * sizeof(uint32_t);
    smci.pCode = fill_evenodd_spv;
    if (vkCreateShaderModule(m_device, &smci, nullptr, &m_shader) != VK_SUCCESS)
        return false;

    // ---- Descriptor set layout: 2 storage buffers -----------------------
    VkDescriptorSetLayoutBinding binds[2]{};
    binds[0].binding = 0;
    binds[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    binds[0].descriptorCount = 1;
    binds[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    binds[1].binding = 1;
    binds[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    binds[1].descriptorCount = 1;
    binds[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo dslci{};
    dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslci.bindingCount = 2;
    dslci.pBindings = binds;
    if (vkCreateDescriptorSetLayout(m_device, &dslci, nullptr, &m_dset_layout) != VK_SUCCESS)
        return false;

    // ---- Pipeline layout (descriptor set + push constants) --------------
    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcr.offset = 0;
    pcr.size = sizeof(PushConsts);

    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &m_dset_layout;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(m_device, &plci, nullptr, &m_pipe_layout) != VK_SUCCESS)
        return false;

    // ---- Compute pipeline -----------------------------------------------
    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = m_shader;
    stage.pName = "main";

    VkComputePipelineCreateInfo cpci2{};
    cpci2.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpci2.stage = stage;
    cpci2.layout = m_pipe_layout;
    if (vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &cpci2, nullptr, &m_pipeline) != VK_SUCCESS)
        return false;

    m_usable = true;
    return true;
}

void VulkanContext::destroy()
{
    if (m_device) {
        if (m_pipeline)     vkDestroyPipeline(m_device, m_pipeline, nullptr);
        if (m_pipe_layout)  vkDestroyPipelineLayout(m_device, m_pipe_layout, nullptr);
        if (m_dset_layout)  vkDestroyDescriptorSetLayout(m_device, m_dset_layout, nullptr);
        if (m_shader)       vkDestroyShaderModule(m_device, m_shader, nullptr);
        if (m_cmd_pool)     vkDestroyCommandPool(m_device, m_cmd_pool, nullptr);
        vkDestroyDevice(m_device, nullptr);
    }
    if (m_instance) vkDestroyInstance(m_instance, nullptr);
    m_pipeline = VK_NULL_HANDLE; m_pipe_layout = VK_NULL_HANDLE;
    m_dset_layout = VK_NULL_HANDLE; m_shader = VK_NULL_HANDLE;
    m_cmd_pool = VK_NULL_HANDLE; m_device = VK_NULL_HANDLE;
    m_instance = VK_NULL_HANDLE;
    m_usable = false;
}

uint32_t VulkanContext::find_memory_type(uint32_t type_bits, VkMemoryPropertyFlags props) const
{
    for (uint32_t i = 0; i < m_mem_props.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) &&
            (m_mem_props.memoryTypes[i].propertyFlags & props) == props)
            return i;
    }
    return UINT32_MAX;
}

bool VulkanContext::create_buffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                  VkMemoryPropertyFlags props,
                                  VkBuffer &buf, VkDeviceMemory &mem) const
{
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = size;
    bci.usage = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(m_device, &bci, nullptr, &buf) != VK_SUCCESS)
        return false;

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(m_device, buf, &req);
    uint32_t mt = find_memory_type(req.memoryTypeBits, props);
    if (mt == UINT32_MAX)
        return false;

    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = mt;
    if (vkAllocateMemory(m_device, &mai, nullptr, &mem) != VK_SUCCESS)
        return false;
    vkBindBufferMemory(m_device, buf, mem, 0);
    return true;
}

bool VulkanContext::rasterize(const std::vector<Edge> &edges,
                              uint32_t width, uint32_t height, uint32_t ss,
                              std::vector<uint8_t> &out)
{
    if (!m_usable || width == 0 || height == 0)
        return false;
    if (ss < 1) ss = 1;

    std::lock_guard<std::mutex> lock(m_mutex); // Phase 3: serialize dispatches

    const VkDeviceSize n_pixels = (VkDeviceSize)width * height;
    // Always allocate at least 1 edge to keep buffer sizes nonzero.
    const size_t edge_count = edges.size();
    const VkDeviceSize edge_bytes = std::max<VkDeviceSize>(sizeof(Edge), (VkDeviceSize)edge_count * sizeof(Edge));
    const VkDeviceSize out_bytes  = n_pixels * sizeof(uint32_t);

    const VkMemoryPropertyFlags host_visible =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    // Host-visible storage buffers (simplest path; no separate staging copies).
    VkBuffer edge_buf = VK_NULL_HANDLE, out_buf = VK_NULL_HANDLE;
    VkDeviceMemory edge_mem = VK_NULL_HANDLE, out_mem = VK_NULL_HANDLE;
    bool ok = true;
    ok = ok && create_buffer(edge_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, host_visible, edge_buf, edge_mem);
    ok = ok && create_buffer(out_bytes,  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, host_visible, out_buf,  out_mem);

    VkDescriptorPool dpool = VK_NULL_HANDLE;
    VkCommandBuffer  cmd = VK_NULL_HANDLE;
    VkFence          fence = VK_NULL_HANDLE;

    auto cleanup = [&]() {
        if (fence)    vkDestroyFence(m_device, fence, nullptr);
        if (cmd)      vkFreeCommandBuffers(m_device, m_cmd_pool, 1, &cmd);
        if (dpool)    vkDestroyDescriptorPool(m_device, dpool, nullptr);
        if (edge_buf) vkDestroyBuffer(m_device, edge_buf, nullptr);
        if (edge_mem) vkFreeMemory(m_device, edge_mem, nullptr);
        if (out_buf)  vkDestroyBuffer(m_device, out_buf, nullptr);
        if (out_mem)  vkFreeMemory(m_device, out_mem, nullptr);
    };

    if (!ok) { cleanup(); return false; }

    // Upload edges.
    {
        void *p = nullptr;
        if (vkMapMemory(m_device, edge_mem, 0, edge_bytes, 0, &p) != VK_SUCCESS) { cleanup(); return false; }
        if (edge_count > 0)
            std::memcpy(p, edges.data(), edge_count * sizeof(Edge));
        else
            std::memset(p, 0, sizeof(Edge));
        vkUnmapMemory(m_device, edge_mem);
    }

    // Descriptor pool + set.
    VkDescriptorPoolSize psize{};
    psize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    psize.descriptorCount = 2;
    VkDescriptorPoolCreateInfo dpci{};
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = 1;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &psize;
    if (vkCreateDescriptorPool(m_device, &dpci, nullptr, &dpool) != VK_SUCCESS) { cleanup(); return false; }

    VkDescriptorSet dset = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo dsai{};
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = dpool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &m_dset_layout;
    if (vkAllocateDescriptorSets(m_device, &dsai, &dset) != VK_SUCCESS) { cleanup(); return false; }

    VkDescriptorBufferInfo ebi{ edge_buf, 0, edge_bytes };
    VkDescriptorBufferInfo obi{ out_buf, 0, out_bytes };
    VkWriteDescriptorSet writes[2]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = dset; writes[0].dstBinding = 0; writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[0].pBufferInfo = &ebi;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = dset; writes[1].dstBinding = 1; writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[1].pBufferInfo = &obi;
    vkUpdateDescriptorSets(m_device, 2, writes, 0, nullptr);

    // Command buffer.
    VkCommandBufferAllocateInfo cbai{};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = m_cmd_pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(m_device, &cbai, &cmd) != VK_SUCCESS) { cleanup(); return false; }

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipe_layout,
                            0, 1, &dset, 0, nullptr);
    PushConsts pcs{ width, height, (uint32_t)edge_count, ss };
    vkCmdPushConstants(cmd, m_pipe_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pcs), &pcs);

    const uint32_t gx = (width  + 7) / 8;
    const uint32_t gy = (height + 7) / 8;
    vkCmdDispatch(cmd, gx, gy, 1);

    // Make compute writes visible to the host map.
    VkMemoryBarrier mb{};
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);

    vkEndCommandBuffer(cmd);

    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    vkCreateFence(m_device, &fci, nullptr, &fence);

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    if (vkQueueSubmit(m_queue, 1, &si, fence) != VK_SUCCESS) { cleanup(); return false; }
    vkWaitForFences(m_device, 1, &fence, VK_TRUE, UINT64_MAX);

    // Read back: shader wrote one uint per pixel; pack down to R8.
    out.resize(n_pixels);
    {
        void *p = nullptr;
        if (vkMapMemory(m_device, out_mem, 0, out_bytes, 0, &p) != VK_SUCCESS) { cleanup(); return false; }
        const uint32_t *src = reinterpret_cast<const uint32_t *>(p);
        for (VkDeviceSize i = 0; i < n_pixels; ++i)
            out[i] = (uint8_t)(src[i] & 0xffu);
        vkUnmapMemory(m_device, out_mem);
    }

    cleanup();
    return true;
}

} // namespace gpu
} // namespace sla
} // namespace Slic3r

#endif // SLIC3R_VULKAN
