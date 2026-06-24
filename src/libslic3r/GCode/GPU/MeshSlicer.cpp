#include "MeshSlicer.hpp"

#ifdef SLIC3R_VULKAN

#include <vulkan/vulkan.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <thread>
#include <mutex>
#include <algorithm>
#include <boost/log/trivial.hpp>

#include "slice_facets_spv.hpp"

namespace Slic3r {
namespace mesh_gpu {

namespace {

// Push-constant block; must match slice_facets.comp.
struct PushConsts {
    uint32_t n_faces;
    uint32_t n_zs;
    uint32_t out_capacity;
};

#define VK_OK(x) do { if ((x) != VK_SUCCESS) return false; } while (0)

using clk = std::chrono::high_resolution_clock;
inline double ms_since(clk::time_point t) {
    return std::chrono::duration<double, std::milli>(clk::now() - t).count();
}

} // namespace

struct MeshSlicerImpl {
    VkInstance       instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys     = VK_NULL_HANDLE;
    VkDevice         device   = VK_NULL_HANDLE;
    uint32_t         queue_family = 0;
    VkQueue          queue    = VK_NULL_HANDLE;
    VkCommandPool    cmd_pool = VK_NULL_HANDLE;

    VkDescriptorSetLayout dset_layout = VK_NULL_HANDLE;
    VkPipelineLayout pipe_layout = VK_NULL_HANDLE;
    VkPipeline       pipeline = VK_NULL_HANDLE;
    VkShaderModule   shader   = VK_NULL_HANDLE;

    VkPhysicalDeviceMemoryProperties mem_props{};

    uint32_t find_memory_type(uint32_t bits, VkMemoryPropertyFlags p) const {
        for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (mem_props.memoryTypes[i].propertyFlags & p) == p)
                return i;
        return UINT32_MAX;
    }
};

namespace {

struct Buf {
    VkBuffer       buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkDeviceSize   size = 0;
};

bool create_buffer(MeshSlicerImpl &d, VkDeviceSize size,
                   VkBufferUsageFlags usage, VkMemoryPropertyFlags props, Buf &out) {
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size; bci.usage = usage; bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(d.device, &bci, nullptr, &out.buf) != VK_SUCCESS) return false;
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(d.device, out.buf, &req);
    uint32_t mt = d.find_memory_type(req.memoryTypeBits, props);
    if (mt == UINT32_MAX) return false;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size; mai.memoryTypeIndex = mt;
    if (vkAllocateMemory(d.device, &mai, nullptr, &out.mem) != VK_SUCCESS) return false;
    vkBindBufferMemory(d.device, out.buf, out.mem, 0);
    out.size = size;
    return true;
}

void destroy_buffer(VkDevice dev, Buf &b) {
    if (b.buf) vkDestroyBuffer(dev, b.buf, nullptr);
    if (b.mem) vkFreeMemory(dev, b.mem, nullptr);
    b.buf = VK_NULL_HANDLE; b.mem = VK_NULL_HANDLE;
}

bool upload(MeshSlicerImpl &d, Buf &b, const void *data, size_t n) {
    if (n == 0) return true;
    void *p = nullptr;
    if (vkMapMemory(d.device, b.mem, 0, n, 0, &p) != VK_SUCCESS) return false;
    std::memcpy(p, data, n);
    vkUnmapMemory(d.device, b.mem);
    return true;
}

template <typename F>
bool run_once(MeshSlicerImpl &d, F rec) {
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = d.cmd_pool; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(d.device, &cbai, &cmd) != VK_SUCCESS) return false;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    rec(cmd);
    vkEndCommandBuffer(cmd);
    VkFence fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCreateFence(d.device, &fci, nullptr, &fence);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
    bool ok = vkQueueSubmit(d.queue, 1, &si, fence) == VK_SUCCESS;
    if (ok) ok = vkWaitForFences(d.device, 1, &fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS;
    vkDestroyFence(d.device, fence, nullptr);
    vkFreeCommandBuffers(d.device, d.cmd_pool, 1, &cmd);
    return ok;
}

} // namespace

std::shared_ptr<MeshSlicer> MeshSlicer::get() {
    static std::shared_ptr<MeshSlicer> instance = [] {
        std::shared_ptr<MeshSlicer> ctx(new MeshSlicer());
        if (!ctx->init()) {
            ctx->destroy();
            BOOST_LOG_TRIVIAL(info) << "MeshSlicer: no usable compute GPU; using CPU.";
        } else {
            BOOST_LOG_TRIVIAL(info) << "MeshSlicer: using GPU compute on " << ctx->m_device_name;
        }
        return ctx;
    }();
    return instance;
}

void MeshSlicer::warmup_async() {
    const char *on = std::getenv("ORCA_MESH_GPU");
    if (on == nullptr || std::strcmp(on, "1") != 0)
        return; // default OFF until proven
    static std::once_flag once;
    std::call_once(once, [] {
        try { std::thread([] { (void) MeshSlicer::get(); }).detach(); }
        catch (...) {}
    });
}

MeshSlicer::~MeshSlicer() { destroy(); }

bool MeshSlicer::init() {
    m_impl = std::make_unique<MeshSlicerImpl>();
    MeshSlicerImpl &d = *m_impl;

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "OrcaSlicer-Mesh-GPU";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VK_OK(vkCreateInstance(&ici, nullptr, &d.instance));

    uint32_t n = 0; vkEnumeratePhysicalDevices(d.instance, &n, nullptr);
    if (n == 0) return false;
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(d.instance, &n, devs.data());

    // Pick the first device that is a real GPU (not llvmpipe/CPU) with a compute
    // queue and shaderInt64 + shaderFloat64 (needed to reproduce the CPU's int64
    // coord_t + double interpolation exactly). ORCA_MESH_GPU_DEV=<index> forces a
    // specific physical device (benchmarking only).
    int force_dev = -1;
    if (const char *fd = std::getenv("ORCA_MESH_GPU_DEV")) force_dev = std::atoi(fd);
    int picked = -1;
    for (uint32_t i = 0; i < n; ++i) {
        if (force_dev >= 0 && (int)i != force_dev) continue;
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(devs[i], &props);
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) continue; // skip llvmpipe
        VkPhysicalDeviceFeatures f{};
        vkGetPhysicalDeviceFeatures(devs[i], &f);
        if (!f.shaderInt64 || !f.shaderFloat64) continue;
        uint32_t qn = 0; vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qf(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &qn, qf.data());
        bool has_compute = false;
        for (uint32_t q = 0; q < qn; ++q)
            if (qf[q].queueFlags & VK_QUEUE_COMPUTE_BIT) { has_compute = true; break; }
        if (!has_compute) continue;
        picked = (int)i;
        break;
    }
    if (picked < 0) return false;

    d.phys = devs[picked];
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(d.phys, &props);
    m_device_name = props.deviceName;
    vkGetPhysicalDeviceMemoryProperties(d.phys, &d.mem_props);

    uint32_t qn = 0; vkGetPhysicalDeviceQueueFamilyProperties(d.phys, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(d.phys, &qn, qf.data());
    int qfi = -1;
    for (uint32_t q = 0; q < qn; ++q)
        if (qf[q].queueFlags & VK_QUEUE_COMPUTE_BIT) { qfi = (int)q; break; }
    if (qfi < 0) return false;
    d.queue_family = (uint32_t)qfi;

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = d.queue_family; qci.queueCount = 1; qci.pQueuePriorities = &prio;

    VkPhysicalDeviceFeatures want{};
    want.shaderInt64 = VK_TRUE;
    want.shaderFloat64 = VK_TRUE;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    dci.pEnabledFeatures = &want;
    VK_OK(vkCreateDevice(d.phys, &dci, nullptr, &d.device));
    vkGetDeviceQueue(d.device, d.queue_family, 0, &d.queue);

    VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpci.queueFamilyIndex = d.queue_family;
    VK_OK(vkCreateCommandPool(d.device, &cpci, nullptr, &d.cmd_pool));

    VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = slice_facets_spv_size * sizeof(uint32_t);
    smci.pCode = slice_facets_spv;
    VK_OK(vkCreateShaderModule(d.device, &smci, nullptr, &d.shader));

    // 6 storage buffers: verts, indices, edge_ids, zs, outlines, counter.
    VkDescriptorSetLayoutBinding binds[6]{};
    for (uint32_t i = 0; i < 6; ++i)
        binds[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dslci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dslci.bindingCount = 6; dslci.pBindings = binds;
    VK_OK(vkCreateDescriptorSetLayout(d.device, &dslci, nullptr, &d.dset_layout));

    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushConsts)};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1; plci.pSetLayouts = &d.dset_layout;
    plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
    VK_OK(vkCreatePipelineLayout(d.device, &plci, nullptr, &d.pipe_layout));

    VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; stage.module = d.shader; stage.pName = "main";
    VkComputePipelineCreateInfo cpci2{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpci2.stage = stage; cpci2.layout = d.pipe_layout;
    VK_OK(vkCreateComputePipelines(d.device, VK_NULL_HANDLE, 1, &cpci2, nullptr, &d.pipeline));

    m_usable = true;
    return true;
}

void MeshSlicer::destroy() {
    if (!m_impl) { m_usable = false; return; }
    MeshSlicerImpl &d = *m_impl;
    if (d.device) {
        if (d.pipeline)    vkDestroyPipeline(d.device, d.pipeline, nullptr);
        if (d.pipe_layout) vkDestroyPipelineLayout(d.device, d.pipe_layout, nullptr);
        if (d.dset_layout) vkDestroyDescriptorSetLayout(d.device, d.dset_layout, nullptr);
        if (d.shader)      vkDestroyShaderModule(d.device, d.shader, nullptr);
        if (d.cmd_pool)    vkDestroyCommandPool(d.device, d.cmd_pool, nullptr);
        vkDestroyDevice(d.device, nullptr);
    }
    if (d.instance) vkDestroyInstance(d.instance, nullptr);
    m_impl.reset();
    m_usable = false;
}

bool MeshSlicer::compute_lines(const std::vector<float>   &verts_xyz,
                               const std::vector<int32_t> &indices,
                               const std::vector<int32_t> &edge_ids,
                               const std::vector<float>   &zs,
                               std::vector<GpuSliceLine>  &out,
                               double *kernel_ms, double *copyback_ms, double *upload_ms) {
    if (!m_usable || indices.empty() || zs.empty())
        return false;

    std::lock_guard<std::mutex> lock(m_mutex);
    MeshSlicerImpl &d = *m_impl;

    const uint32_t n_faces = (uint32_t)(indices.size() / 3);
    const uint32_t n_zs    = (uint32_t)zs.size();

    // Output capacity: each facet emits at most one segment per spanned layer.
    // A safe (generous) bound is n_faces * n_zs, but that can be huge; in practice
    // total segments ~= n_faces * (avg spanned layers). Heuristic: cap at the worst
    // realistic case but clamp to a memory budget. We pick max(4M, n_faces*2) and
    // detect overflow (counter > capacity) to fall back to CPU.
    uint64_t cap64 = (uint64_t)n_faces * 16u;
    if (cap64 < 4u << 20) cap64 = 4u << 20;
    const uint64_t HARD_CAP = 64u << 20; // 64M records * 64B = 4GB ceiling guard
    if (cap64 > HARD_CAP) cap64 = HARD_CAP;
    const uint32_t out_capacity = (uint32_t)cap64;

    const VkMemoryPropertyFlags HOST =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    std::vector<Buf> bufs;
    bufs.reserve(8);
    VkDescriptorPool dpool = VK_NULL_HANDLE;
    auto cleanup = [&]() {
        if (dpool) vkDestroyDescriptorPool(d.device, dpool, nullptr);
        for (auto &b : bufs) destroy_buffer(d.device, b);
    };
    auto fail = [&]() { cleanup(); return false; };
    auto new_buf = [&](VkDeviceSize size, VkBufferUsageFlags usage) -> Buf * {
        bufs.emplace_back();
        if (!create_buffer(d, size, usage, HOST, bufs.back())) return nullptr;
        return &bufs.back();
    };
    const VkBufferUsageFlags SB = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

    auto t_up = clk::now();
    Buf *vb = new_buf(verts_xyz.size() * sizeof(float), SB);
    Buf *ib = new_buf(indices.size()   * sizeof(int32_t), SB);
    Buf *eb = new_buf(edge_ids.size()  * sizeof(int32_t), SB);
    Buf *zb = new_buf(zs.size()        * sizeof(float), SB);
    Buf *ob = new_buf((VkDeviceSize)out_capacity * sizeof(GpuSliceLine), SB);
    Buf *cb = new_buf(sizeof(uint32_t), SB);
    if (!vb || !ib || !eb || !zb || !ob || !cb) return fail();
    if (!upload(d, *vb, verts_xyz.data(), verts_xyz.size() * sizeof(float))) return fail();
    if (!upload(d, *ib, indices.data(),   indices.size()   * sizeof(int32_t))) return fail();
    if (!upload(d, *eb, edge_ids.data(),  edge_ids.size()  * sizeof(int32_t))) return fail();
    if (!upload(d, *zb, zs.data(),        zs.size()        * sizeof(float))) return fail();
    uint32_t zero = 0;
    if (!upload(d, *cb, &zero, sizeof(zero))) return fail();
    if (upload_ms) *upload_ms = ms_since(t_up);

    VkDescriptorPoolSize psz{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 6};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = 1; dpci.poolSizeCount = 1; dpci.pPoolSizes = &psz;
    if (vkCreateDescriptorPool(d.device, &dpci, nullptr, &dpool) != VK_SUCCESS) return fail();
    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = dpool; dsai.descriptorSetCount = 1; dsai.pSetLayouts = &d.dset_layout;
    VkDescriptorSet dset = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(d.device, &dsai, &dset) != VK_SUCCESS) return fail();

    VkDescriptorBufferInfo dbi[6] = {
        {vb->buf, 0, VK_WHOLE_SIZE}, {ib->buf, 0, VK_WHOLE_SIZE},
        {eb->buf, 0, VK_WHOLE_SIZE}, {zb->buf, 0, VK_WHOLE_SIZE},
        {ob->buf, 0, VK_WHOLE_SIZE}, {cb->buf, 0, VK_WHOLE_SIZE}};
    VkWriteDescriptorSet w[6]{};
    for (uint32_t i = 0; i < 6; ++i)
        w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, dset, i, 0, 1,
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &dbi[i]};
    vkUpdateDescriptorSets(d.device, 6, w, 0, nullptr);

    PushConsts pc{n_faces, n_zs, out_capacity};
    auto t_k = clk::now();
    if (!run_once(d, [&](VkCommandBuffer cmd) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d.pipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d.pipe_layout, 0, 1, &dset, 0, nullptr);
            vkCmdPushConstants(cmd, d.pipe_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
            vkCmdDispatch(cmd, (n_faces + 63) / 64, 1, 1);
            VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                                 0, 1, &mb, 0, nullptr, 0, nullptr);
        }))
        return fail();
    if (kernel_ms) *kernel_ms = ms_since(t_k);

    // ---- read back: count first, then the records ----
    auto t_cb = clk::now();
    uint32_t produced = 0;
    {
        void *p = nullptr;
        if (vkMapMemory(d.device, cb->mem, 0, sizeof(uint32_t), 0, &p) != VK_SUCCESS) return fail();
        std::memcpy(&produced, p, sizeof(uint32_t));
        vkUnmapMemory(d.device, cb->mem);
    }
    if (produced > out_capacity) { // overflow -> CPU fallback
        BOOST_LOG_TRIVIAL(info) << "MeshSlicer: output overflow (" << produced << " > " << out_capacity << "), falling back to CPU.";
        return fail();
    }
    out.resize(produced);
    if (produced > 0) {
        void *p = nullptr;
        VkDeviceSize bytes = (VkDeviceSize)produced * sizeof(GpuSliceLine);
        if (vkMapMemory(d.device, ob->mem, 0, bytes, 0, &p) != VK_SUCCESS) return fail();
        std::memcpy(out.data(), p, bytes);
        vkUnmapMemory(d.device, ob->mem);
    }
    if (copyback_ms) *copyback_ms = ms_since(t_cb);

    cleanup();
    return true;
}

} // namespace mesh_gpu
} // namespace Slic3r

#endif // SLIC3R_VULKAN
