#include "SeamRayCaster.hpp"

#ifdef SLIC3R_VULKAN

#include <vulkan/vulkan.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <mutex>
#include <algorithm>
#include <boost/log/trivial.hpp>

#include "libslic3r/TriangleSetSampling.hpp"
#include "seam_raycast_spv.hpp"

namespace Slic3r {
namespace seam_gpu {

namespace {

// Push-constant block; must match seam_raycast.comp.
struct PushConsts {
    uint32_t sample_count;
    uint32_t sqr_rays;
    float    ray_origin_offset;
};

#define VK_OK(x) do { if ((x) != VK_SUCCESS) return false; } while (0)

} // namespace

// All Vulkan state lives in Impl so the header stays free of vulkan.h.
struct SeamRayCasterImpl {
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
    // Required alignment for acceleration-structure build scratch device
    // addresses (minAccelerationStructureScratchOffsetAlignment). The scratch
    // buffer must be allocated with extra room and its address rounded up to
    // this; otherwise the driver faults during the build.
    VkDeviceSize scratch_align = 256;

    // KHR accel-structure / device-address entry points (loaded via
    // vkGetDeviceProcAddr; the static loader does not export these directly).
    PFN_vkGetBufferDeviceAddressKHR              GetBufferDeviceAddressKHR = nullptr;
    PFN_vkCreateAccelerationStructureKHR         CreateAccelerationStructureKHR = nullptr;
    PFN_vkDestroyAccelerationStructureKHR        DestroyAccelerationStructureKHR = nullptr;
    PFN_vkGetAccelerationStructureBuildSizesKHR  GetAccelerationStructureBuildSizesKHR = nullptr;
    PFN_vkCmdBuildAccelerationStructuresKHR      CmdBuildAccelerationStructuresKHR = nullptr;
    PFN_vkGetAccelerationStructureDeviceAddressKHR GetAccelerationStructureDeviceAddressKHR = nullptr;

    uint32_t find_memory_type(uint32_t bits, VkMemoryPropertyFlags p) const {
        for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (mem_props.memoryTypes[i].propertyFlags & p) == p)
                return i;
        return UINT32_MAX;
    }
};

namespace {

// A device buffer + its memory.
struct Buf {
    VkBuffer       buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
};

bool create_buffer(SeamRayCasterImpl &d, VkDeviceSize size,
                   VkBufferUsageFlags usage, VkMemoryPropertyFlags props,
                   bool device_address, Buf &out) {
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size; bci.usage = usage; bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(d.device, &bci, nullptr, &out.buf) != VK_SUCCESS) return false;
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(d.device, out.buf, &req);
    uint32_t mt = d.find_memory_type(req.memoryTypeBits, props);
    if (mt == UINT32_MAX) return false;
    VkMemoryAllocateFlagsInfo fi{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    fi.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    if (device_address) mai.pNext = &fi;
    mai.allocationSize = req.size; mai.memoryTypeIndex = mt;
    if (vkAllocateMemory(d.device, &mai, nullptr, &out.mem) != VK_SUCCESS) return false;
    vkBindBufferMemory(d.device, out.buf, out.mem, 0);
    return true;
}

void destroy_buffer(VkDevice dev, Buf &b) {
    if (b.buf) vkDestroyBuffer(dev, b.buf, nullptr);
    if (b.mem) vkFreeMemory(dev, b.mem, nullptr);
    b.buf = VK_NULL_HANDLE; b.mem = VK_NULL_HANDLE;
}

VkDeviceAddress buffer_address(SeamRayCasterImpl &d, VkBuffer b) {
    VkBufferDeviceAddressInfo i{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO}; i.buffer = b;
    return d.GetBufferDeviceAddressKHR(d.device, &i);
}

bool upload(SeamRayCasterImpl &d, Buf &b, const void *data, size_t n) {
    void *p = nullptr;
    if (vkMapMemory(d.device, b.mem, 0, n, 0, &p) != VK_SUCCESS) return false;
    std::memcpy(p, data, n);
    vkUnmapMemory(d.device, b.mem);
    return true;
}

// Submit a one-time command buffer recorded by `rec` and wait for completion.
template <typename F>
bool run_once(SeamRayCasterImpl &d, F rec) {
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

std::shared_ptr<SeamRayCaster> SeamRayCaster::get() {
    static std::shared_ptr<SeamRayCaster> instance = [] {
        std::shared_ptr<SeamRayCaster> ctx(new SeamRayCaster());
        if (!ctx->init()) {
            ctx->destroy();
            BOOST_LOG_TRIVIAL(info) << "SeamRayCaster: no ray-query GPU; using CPU.";
        } else {
            BOOST_LOG_TRIVIAL(info) << "SeamRayCaster: using GPU ray-query on " << ctx->m_device_name;
        }
        return ctx;
    }();
    return instance;
}

void SeamRayCaster::warmup_async() {
    // Skip if the user explicitly disabled the GPU seam path; otherwise create the
    // device/context on a detached thread so its init (Vulkan instance + driver +
    // device, a few hundred ms) overlaps with model load and the early slicing
    // stages and is already warm by the time raycast_visibility() calls get().
    // get()'s underlying static-local init is thread-safe, so a later caller simply
    // reuses (or briefly waits on) this in-flight initialization.
    const char *off = std::getenv("ORCA_SEAM_GPU");
    if (off != nullptr && std::strcmp(off, "0") == 0)
        return;
    // Launch exactly one warmup thread no matter how many times this is called --
    // it is invoked from app startup (GUI) / CLI entry (earliest, to overlap the
    // init with model load and user setup) and again from Print::process() as a
    // backstop. The earliest caller wins; the rest are no-ops.
    static std::once_flag once;
    std::call_once(once, [] {
        try {
            std::thread([] { (void) SeamRayCaster::get(); }).detach();
        } catch (...) {
            // Thread creation failed -> get() will init lazily on first use.
        }
    });
}

SeamRayCaster::~SeamRayCaster() { destroy(); }

bool SeamRayCaster::init() {
    m_impl = std::make_unique<SeamRayCasterImpl>();
    SeamRayCasterImpl &d = *m_impl;

    // ---- instance (Vulkan 1.2) ----
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "OrcaSlicer-Seam-GPU";
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VK_OK(vkCreateInstance(&ici, nullptr, &d.instance));

    // ---- pick first device with rayQuery + accelerationStructure ----
    uint32_t n = 0; vkEnumeratePhysicalDevices(d.instance, &n, nullptr);
    if (n == 0) return false;
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(d.instance, &n, devs.data());

    auto has_ext = [](VkPhysicalDevice pd, const char *name) {
        uint32_t ec = 0; vkEnumerateDeviceExtensionProperties(pd, nullptr, &ec, nullptr);
        std::vector<VkExtensionProperties> ex(ec);
        vkEnumerateDeviceExtensionProperties(pd, nullptr, &ec, ex.data());
        for (auto &e : ex) if (std::strcmp(e.extensionName, name) == 0) return true;
        return false;
    };

    int picked = -1;
    for (uint32_t i = 0; i < n; ++i) {
        VkPhysicalDeviceRayQueryFeaturesKHR rq{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
        VkPhysicalDeviceAccelerationStructureFeaturesKHR as{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
        as.pNext = &rq;
        VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        f2.pNext = &as;
        vkGetPhysicalDeviceFeatures2(devs[i], &f2);
        if (!rq.rayQuery || !as.accelerationStructure) continue;
        // The shader uses GL_EXT_ray_tracing_position_fetch to recover hit
        // vertices, which requires VK_KHR_ray_tracing_position_fetch.
        if (!has_ext(devs[i], "VK_KHR_ray_tracing_position_fetch")) continue;
        picked = (int)i;
        break;
    }
    if (picked < 0) return false;

    d.phys = devs[picked];
    VkPhysicalDeviceAccelerationStructurePropertiesKHR asprops{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
    VkPhysicalDeviceProperties2 props2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    props2.pNext = &asprops;
    vkGetPhysicalDeviceProperties2(d.phys, &props2);
    m_device_name = props2.properties.deviceName;
    if (asprops.minAccelerationStructureScratchOffsetAlignment != 0)
        d.scratch_align = asprops.minAccelerationStructureScratchOffsetAlignment;
    vkGetPhysicalDeviceMemoryProperties(d.phys, &d.mem_props);

    uint32_t qn = 0; vkGetPhysicalDeviceQueueFamilyProperties(d.phys, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(d.phys, &qn, qf.data());
    int qfi = -1;
    for (uint32_t q = 0; q < qn; ++q)
        if (qf[q].queueFlags & VK_QUEUE_COMPUTE_BIT) { qfi = (int)q; break; }
    if (qfi < 0) return false;
    d.queue_family = (uint32_t)qfi;

    // ---- logical device with RT feature chain ----
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = d.queue_family; qci.queueCount = 1; qci.pQueuePriorities = &prio;

    std::vector<const char *> exts = {
        VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
        VK_KHR_RAY_QUERY_EXTENSION_NAME,
        VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
        VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
        VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME,
        VK_KHR_SPIRV_1_4_EXTENSION_NAME,
        VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME,
        "VK_KHR_ray_tracing_position_fetch",
    };

    VkPhysicalDeviceRayTracingPositionFetchFeaturesKHR pf{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_POSITION_FETCH_FEATURES_KHR};
    pf.rayTracingPositionFetch = VK_TRUE;
    VkPhysicalDeviceRayQueryFeaturesKHR rq{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    rq.rayQuery = VK_TRUE; rq.pNext = &pf;
    VkPhysicalDeviceAccelerationStructureFeaturesKHR asf{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    asf.accelerationStructure = VK_TRUE; asf.pNext = &rq;
    VkPhysicalDeviceBufferDeviceAddressFeatures bda{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
    bda.bufferDeviceAddress = VK_TRUE; bda.pNext = &asf;

    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &bda;
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = (uint32_t)exts.size();
    dci.ppEnabledExtensionNames = exts.data();
    VK_OK(vkCreateDevice(d.phys, &dci, nullptr, &d.device));
    vkGetDeviceQueue(d.device, d.queue_family, 0, &d.queue);

    // ---- load KHR entry points ----
    #define LOAD(name) d.name = (PFN_vk##name)vkGetDeviceProcAddr(d.device, "vk"#name); if (!d.name) return false;
    LOAD(GetBufferDeviceAddressKHR);
    LOAD(CreateAccelerationStructureKHR);
    LOAD(DestroyAccelerationStructureKHR);
    LOAD(GetAccelerationStructureBuildSizesKHR);
    LOAD(CmdBuildAccelerationStructuresKHR);
    LOAD(GetAccelerationStructureDeviceAddressKHR);
    #undef LOAD

    // ---- command pool ----
    VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpci.queueFamilyIndex = d.queue_family;
    VK_OK(vkCreateCommandPool(d.device, &cpci, nullptr, &d.cmd_pool));

    // ---- pipeline: TLAS + 3 storage buffers + push consts ----
    VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = seam_raycast_spv_size * sizeof(uint32_t);
    smci.pCode = seam_raycast_spv;
    VK_OK(vkCreateShaderModule(d.device, &smci, nullptr, &d.shader));

    VkDescriptorSetLayoutBinding binds[4]{};
    binds[0] = {0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    binds[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    binds[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    binds[3] = {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dslci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dslci.bindingCount = 4; dslci.pBindings = binds;
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

void SeamRayCaster::destroy() {
    if (!m_impl) { m_usable = false; return; }
    SeamRayCasterImpl &d = *m_impl;
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

bool SeamRayCaster::compute_visibility(const indexed_triangle_set &mesh,
                                       const TriangleSetSamples    &samples,
                                       uint32_t                     sqr_rays,
                                       float                        ray_origin_offset,
                                       std::vector<float>          &out) {
    if (!m_usable || mesh.indices.empty() || samples.positions.empty())
        return false;

    std::lock_guard<std::mutex> lock(m_mutex);
    SeamRayCasterImpl &d = *m_impl;
    const bool DBG = std::getenv("ORCA_SEAM_GPU_DEBUG") != nullptr;
    if (DBG) std::fprintf(stderr, "[seam-gpu] %zu verts, %zu prims, %zu samples\n",
                          mesh.vertices.size(), mesh.indices.size(), samples.positions.size());

    const uint32_t n_verts = (uint32_t)mesh.vertices.size();
    const uint32_t n_prims = (uint32_t)mesh.indices.size();
    const uint32_t n_samples = (uint32_t)samples.positions.size();

    const VkMemoryPropertyFlags HOST =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    const VkMemoryPropertyFlags LOCAL = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

    // Track all resources for cleanup. Reserve up front: new_buf hands out
    // pointers into this vector that stay live across later new_buf calls, so
    // the storage must NOT reallocate (a grow would dangle vb/ib/... pointers).
    // We create at most 11 buffers (vb, ib, blas, scratch, inst, tlas, tscratch,
    // pb, nb, rb); 16 leaves headroom.
    std::vector<Buf> bufs;
    bufs.reserve(16);
    VkAccelerationStructureKHR blas = VK_NULL_HANDLE, tlas = VK_NULL_HANDLE;
    VkDescriptorPool dpool = VK_NULL_HANDLE;
    auto cleanup = [&]() {
        if (tlas) d.DestroyAccelerationStructureKHR(d.device, tlas, nullptr);
        if (blas) d.DestroyAccelerationStructureKHR(d.device, blas, nullptr);
        if (dpool) vkDestroyDescriptorPool(d.device, dpool, nullptr);
        for (auto &b : bufs) destroy_buffer(d.device, b);
    };
    auto fail = [&]() { cleanup(); return false; };
    auto new_buf = [&](VkDeviceSize size, VkBufferUsageFlags usage,
                       VkMemoryPropertyFlags props, bool addr) -> Buf * {
        bufs.emplace_back();
        if (!create_buffer(d, size, usage, props, addr, bufs.back())) return nullptr;
        return &bufs.back();
    };

    // ---- mesh buffers (vertices: vec3 R32G32B32; indices: uint32) ----
    const VkBufferUsageFlags GEOM_USAGE =
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

    Buf *vb = new_buf((VkDeviceSize)n_verts * 3 * sizeof(float), GEOM_USAGE, HOST, true);
    if (!vb) return fail();
    {
        std::vector<float> v(n_verts * 3);
        for (uint32_t i = 0; i < n_verts; ++i) {
            v[i * 3 + 0] = mesh.vertices[i].x();
            v[i * 3 + 1] = mesh.vertices[i].y();
            v[i * 3 + 2] = mesh.vertices[i].z();
        }
        if (!upload(d, *vb, v.data(), v.size() * sizeof(float))) return fail();
    }
    Buf *ib = new_buf((VkDeviceSize)n_prims * 3 * sizeof(uint32_t), GEOM_USAGE, HOST, true);
    if (!ib) return fail();
    {
        std::vector<uint32_t> idx(n_prims * 3);
        for (uint32_t f = 0; f < n_prims; ++f) {
            idx[f * 3 + 0] = (uint32_t)mesh.indices[f][0];
            idx[f * 3 + 1] = (uint32_t)mesh.indices[f][1];
            idx[f * 3 + 2] = (uint32_t)mesh.indices[f][2];
        }
        if (!upload(d, *ib, idx.data(), idx.size() * sizeof(uint32_t))) return fail();
    }

    // ---- BLAS ----
    VkAccelerationStructureGeometryKHR geom{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geom.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    geom.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    geom.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    geom.geometry.triangles.vertexData.deviceAddress = buffer_address(d, vb->buf);
    geom.geometry.triangles.vertexStride = 3 * sizeof(float);
    geom.geometry.triangles.maxVertex = n_verts - 1;
    geom.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
    geom.geometry.triangles.indexData.deviceAddress = buffer_address(d, ib->buf);

    VkAccelerationStructureBuildGeometryInfoKHR bgi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    bgi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    bgi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    bgi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    bgi.geometryCount = 1; bgi.pGeometries = &geom;
    VkAccelerationStructureBuildSizesInfoKHR sz{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    d.GetAccelerationStructureBuildSizesKHR(d.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                            &bgi, &n_prims, &sz);

    Buf *blasBuf = new_buf(sz.accelerationStructureSize,
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        LOCAL, true);
    if (!blasBuf) return fail();
    VkAccelerationStructureCreateInfoKHR aci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    aci.buffer = blasBuf->buf; aci.size = sz.accelerationStructureSize;
    aci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    if (d.CreateAccelerationStructureKHR(d.device, &aci, nullptr, &blas) != VK_SUCCESS) return fail();
    Buf *scratch = new_buf(sz.buildScratchSize + d.scratch_align,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, LOCAL, true);
    if (!scratch) return fail();
    bgi.dstAccelerationStructure = blas;
    bgi.scratchData.deviceAddress =
        (buffer_address(d, scratch->buf) + d.scratch_align - 1) & ~(d.scratch_align - 1);
    VkAccelerationStructureBuildRangeInfoKHR range{}; range.primitiveCount = n_prims;
    const VkAccelerationStructureBuildRangeInfoKHR *pr = &range;
    if (!run_once(d, [&](VkCommandBuffer cmd) { d.CmdBuildAccelerationStructuresKHR(cmd, 1, &bgi, &pr); }))
        return fail();
    VkAccelerationStructureDeviceAddressInfoKHR dai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
    dai.accelerationStructure = blas;
    VkDeviceAddress blasAddr = d.GetAccelerationStructureDeviceAddressKHR(d.device, &dai);

    // ---- TLAS (single identity instance) ----
    VkAccelerationStructureInstanceKHR inst{};
    inst.transform.matrix[0][0] = inst.transform.matrix[1][1] = inst.transform.matrix[2][2] = 1.0f;
    inst.mask = 0xFF;
    inst.accelerationStructureReference = blasAddr;
    Buf *instBuf = new_buf(sizeof(inst),
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        HOST, true);
    if (!instBuf) return fail();
    if (!upload(d, *instBuf, &inst, sizeof(inst))) return fail();

    VkAccelerationStructureGeometryKHR tgeom{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    tgeom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR; tgeom.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    tgeom.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    tgeom.geometry.instances.data.deviceAddress = buffer_address(d, instBuf->buf);
    VkAccelerationStructureBuildGeometryInfoKHR tbgi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    tbgi.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    tbgi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    tbgi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    tbgi.geometryCount = 1; tbgi.pGeometries = &tgeom;
    uint32_t tprim = 1;
    VkAccelerationStructureBuildSizesInfoKHR tsz{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    d.GetAccelerationStructureBuildSizesKHR(d.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                            &tbgi, &tprim, &tsz);
    Buf *tlasBuf = new_buf(tsz.accelerationStructureSize,
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        LOCAL, true);
    if (!tlasBuf) return fail();
    VkAccelerationStructureCreateInfoKHR taci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    taci.buffer = tlasBuf->buf; taci.size = tsz.accelerationStructureSize;
    taci.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    if (d.CreateAccelerationStructureKHR(d.device, &taci, nullptr, &tlas) != VK_SUCCESS) return fail();
    Buf *tscratch = new_buf(tsz.buildScratchSize + d.scratch_align,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, LOCAL, true);
    if (!tscratch) return fail();
    tbgi.dstAccelerationStructure = tlas;
    tbgi.scratchData.deviceAddress =
        (buffer_address(d, tscratch->buf) + d.scratch_align - 1) & ~(d.scratch_align - 1);
    VkAccelerationStructureBuildRangeInfoKHR trange{}; trange.primitiveCount = 1;
    const VkAccelerationStructureBuildRangeInfoKHR *tpr = &trange;
    if (!run_once(d, [&](VkCommandBuffer cmd) { d.CmdBuildAccelerationStructuresKHR(cmd, 1, &tbgi, &tpr); }))
        return fail();
    // ---- sample buffers (vec4 padded) + result ----
    Buf *pb = new_buf((VkDeviceSize)n_samples * 4 * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, HOST, false);
    Buf *nb = new_buf((VkDeviceSize)n_samples * 4 * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, HOST, false);
    Buf *rb = new_buf((VkDeviceSize)n_samples * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, HOST, false);
    if (!pb || !nb || !rb) return fail();
    {
        std::vector<float> p(n_samples * 4), nrm(n_samples * 4);
        for (uint32_t i = 0; i < n_samples; ++i) {
            const Vec3f &c = samples.positions[i];
            const Vec3f &nn = samples.normals[i];
            p[i * 4 + 0] = c.x(); p[i * 4 + 1] = c.y(); p[i * 4 + 2] = c.z(); p[i * 4 + 3] = 0;
            nrm[i * 4 + 0] = nn.x(); nrm[i * 4 + 1] = nn.y(); nrm[i * 4 + 2] = nn.z(); nrm[i * 4 + 3] = 0;
        }
        if (!upload(d, *pb, p.data(), p.size() * sizeof(float))) return fail();
        if (!upload(d, *nb, nrm.data(), nrm.size() * sizeof(float))) return fail();
    }

    // ---- descriptor set ----
    VkDescriptorPoolSize psz[2] = {
        {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3}};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = 1; dpci.poolSizeCount = 2; dpci.pPoolSizes = psz;
    if (vkCreateDescriptorPool(d.device, &dpci, nullptr, &dpool) != VK_SUCCESS) return fail();
    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = dpool; dsai.descriptorSetCount = 1; dsai.pSetLayouts = &d.dset_layout;
    VkDescriptorSet dset = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(d.device, &dsai, &dset) != VK_SUCCESS) return fail();

    VkWriteDescriptorSetAccelerationStructureKHR asw{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
    asw.accelerationStructureCount = 1; asw.pAccelerationStructures = &tlas;
    VkDescriptorBufferInfo pbi{pb->buf, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo nbi{nb->buf, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo rbi{rb->buf, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet w[4]{};
    w[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, &asw, dset, 0, 0, 1, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR};
    w[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, dset, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &pbi};
    w[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, dset, 2, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &nbi};
    w[3] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, dset, 3, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &rbi};
    vkUpdateDescriptorSets(d.device, 4, w, 0, nullptr);

    // ---- dispatch ----
    PushConsts pc{n_samples, sqr_rays, ray_origin_offset};
    if (!run_once(d, [&](VkCommandBuffer cmd) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d.pipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d.pipe_layout, 0, 1, &dset, 0, nullptr);
            vkCmdPushConstants(cmd, d.pipe_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
            vkCmdDispatch(cmd, (n_samples + 63) / 64, 1, 1);
            VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                                 0, 1, &mb, 0, nullptr, 0, nullptr);
        }))
        return fail();

    // ---- read back ----
    out.resize(n_samples);
    {
        void *p = nullptr;
        if (vkMapMemory(d.device, rb->mem, 0, (VkDeviceSize)n_samples * sizeof(float), 0, &p) != VK_SUCCESS)
            return fail();
        std::memcpy(out.data(), p, n_samples * sizeof(float));
        vkUnmapMemory(d.device, rb->mem);
    }

    cleanup();
    return true;
}

} // namespace seam_gpu
} // namespace Slic3r

#endif // SLIC3R_VULKAN
