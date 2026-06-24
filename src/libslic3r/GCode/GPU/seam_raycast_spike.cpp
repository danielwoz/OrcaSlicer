// Standalone spike: build a BLAS+TLAS from a tiny mesh, run the seam-raycast
// ray-query compute shader for a handful of samples, and compare GPU visibility
// to a CPU reference computed with the same algorithm. De-risks the
// accel-structure / ray-query path before integrating into OrcaSlicer.
//
// Build (from repo root):
//   g++ -std=c++17 -O2 -Ideps_src/Vulkan-Headers/include /tmp/seam_spike.cpp \
//       build/deps_src/Vulkan-Loader/loader/Release/libvulkan.a \
//       -lpthread -lm -ldl -o /tmp/seam_spike
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <array>
#include <algorithm>

#include "src/libslic3r/GCode/GPU/seam_raycast_spv.hpp"

#define VKCHECK(x) do { VkResult r__=(x); if(r__!=VK_SUCCESS){ \
    std::fprintf(stderr,"VK fail %d at %s:%d\n",(int)r__,__FILE__,__LINE__); std::exit(1);} } while(0)

// ---- minimal vec3 ----------------------------------------------------------
struct V3 { float x,y,z; };
static V3 operator-(V3 a, V3 b){ return {a.x-b.x,a.y-b.y,a.z-b.z}; }
static V3 operator+(V3 a, V3 b){ return {a.x+b.x,a.y+b.y,a.z+b.z}; }
static V3 operator*(V3 a, float s){ return {a.x*s,a.y*s,a.z*s}; }
static float dot(V3 a, V3 b){ return a.x*b.x+a.y*b.y+a.z*b.z; }
static V3 cross(V3 a, V3 b){ return {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x}; }
static V3 norm(V3 a){ float l=std::sqrt(dot(a,a)); return {a.x/l,a.y/l,a.z/l}; }

static const float PI = 3.14159265358979323846f;

// ---- CPU reference (mirror of SeamPlacer.cpp) ------------------------------
static V3 sample_hemisphere_uniform(float sx, float sy) {
    float t1 = 2.0f*PI*sx;
    float t2 = 2.0f*std::sqrt(sy - sy*sy);
    return { std::cos(t1)*t2, std::sin(t1)*t2, std::fabs(1.0f - 2.0f*sy) };
}
static V3 frame_to_world(V3 normal, V3 local) {
    V3 mZ = norm(normal);
    V3 tmpX = (std::fabs(mZ.x) > 0.99f) ? V3{0,1,0} : V3{1,0,0};
    V3 mY = norm(cross(mZ, tmpX));
    V3 mX = cross(mY, mZ);
    return mX*local.x + mY*local.y + mZ*local.z;
}

struct Mesh { std::vector<V3> verts; std::vector<uint32_t> idx; };

// Moller-Trumbore, first hit; returns t>0 hit + face id.
static bool ray_first_hit(const Mesh& m, V3 o, V3 d, float& out_t, int& out_face) {
    bool hit=false; float best=1e30f; int bf=-1;
    for (size_t f=0; f<m.idx.size(); f+=3) {
        V3 v0=m.verts[m.idx[f]], v1=m.verts[m.idx[f+1]], v2=m.verts[m.idx[f+2]];
        V3 e1=v1-v0, e2=v2-v0;
        V3 p=cross(d,e2); float det=dot(e1,p);
        if (std::fabs(det)<1e-9f) continue;
        float inv=1.0f/det;
        V3 tv=o-v0; float u=dot(tv,p)*inv; if(u<0||u>1) continue;
        V3 q=cross(tv,e1); float v=dot(d,q)*inv; if(v<0||u+v>1) continue;
        float t=dot(e2,q)*inv; if(t>1e-6f && t<best){ best=t; bf=(int)(f/3); hit=true; }
    }
    out_t=best; out_face=bf; return hit;
}

static float cpu_visibility(const Mesh& m, V3 center, V3 normal, int N, float off) {
    float vis=1.0f, dec=1.0f/float(N*N), step=1.0f/float(N);
    V3 origin = center + normal*off;
    for (int xi=0; xi<N; ++xi){ float sx=xi*step+step*0.5f;
      for (int yi=0; yi<N; ++yi){ float sy=yi*step+step*0.5f;
        V3 ld=sample_hemisphere_uniform(sx,sy);
        V3 wd=frame_to_world(normal,ld);
        float t; int face;
        if (ray_first_hit(m,origin,wd,t,face)) {
            V3 v0=m.verts[m.idx[face*3]], v1=m.verts[m.idx[face*3+1]], v2=m.verts[m.idx[face*3+2]];
            V3 fn = norm(cross(v1-v0, v2-v1));
            if (dot(fn,wd) <= 0.0f) vis -= dec;
        }
      }
    }
    return vis;
}

// ---- Vulkan helpers --------------------------------------------------------
struct Ctx {
    VkInstance inst; VkPhysicalDevice phys; VkDevice dev; uint32_t qf; VkQueue q;
    VkPhysicalDeviceMemoryProperties memprops;
    VkCommandPool pool;
    PFN_vkGetBufferDeviceAddressKHR pGetBufferDeviceAddressKHR;
    PFN_vkCreateAccelerationStructureKHR pCreateAccelerationStructureKHR;
    PFN_vkDestroyAccelerationStructureKHR pDestroyAccelerationStructureKHR;
    PFN_vkGetAccelerationStructureBuildSizesKHR pGetAccelerationStructureBuildSizesKHR;
    PFN_vkCmdBuildAccelerationStructuresKHR pCmdBuildAccelerationStructuresKHR;
    PFN_vkGetAccelerationStructureDeviceAddressKHR pGetAccelerationStructureDeviceAddressKHR;
};

static uint32_t find_mem(Ctx& c, uint32_t bits, VkMemoryPropertyFlags p) {
    for (uint32_t i=0;i<c.memprops.memoryTypeCount;i++)
        if ((bits&(1u<<i)) && (c.memprops.memoryTypes[i].propertyFlags&p)==p) return i;
    return UINT32_MAX;
}

struct Buf { VkBuffer buf=VK_NULL_HANDLE; VkDeviceMemory mem=VK_NULL_HANDLE; VkDeviceSize size=0; };

static Buf make_buf(Ctx& c, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props, bool addr=false) {
    Buf b; b.size=size;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size=size; bci.usage=usage; bci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    VKCHECK(vkCreateBuffer(c.dev,&bci,nullptr,&b.buf));
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(c.dev,b.buf,&req);
    VkMemoryAllocateFlagsInfo fi{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    fi.flags=VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    if(addr) mai.pNext=&fi;
    mai.allocationSize=req.size; mai.memoryTypeIndex=find_mem(c,req.memoryTypeBits,props);
    VKCHECK(vkAllocateMemory(c.dev,&mai,nullptr,&b.mem));
    VKCHECK(vkBindBufferMemory(c.dev,b.buf,b.mem,0));
    return b;
}
static VkDeviceAddress buf_addr(Ctx& c, VkBuffer b){
    VkBufferDeviceAddressInfo i{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO}; i.buffer=b;
    return c.pGetBufferDeviceAddressKHR(c.dev,&i);
}
static void upload(Ctx& c, Buf& b, const void* data, size_t n){
    void* p; VKCHECK(vkMapMemory(c.dev,b.mem,0,n,0,&p)); std::memcpy(p,data,n); vkUnmapMemory(c.dev,b.mem);
}

static VkCommandBuffer begin_cmd(Ctx& c){
    VkCommandBufferAllocateInfo a{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    a.commandPool=c.pool; a.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; a.commandBufferCount=1;
    VkCommandBuffer cmd; VKCHECK(vkAllocateCommandBuffers(c.dev,&a,&cmd));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd,&bi); return cmd;
}
static void end_cmd(Ctx& c, VkCommandBuffer cmd){
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount=1; si.pCommandBuffers=&cmd;
    VkFence fence; VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VKCHECK(vkCreateFence(c.dev,&fci,nullptr,&fence));
    VKCHECK(vkQueueSubmit(c.q,1,&si,fence));
    VKCHECK(vkWaitForFences(c.dev,1,&fence,VK_TRUE,UINT64_MAX));
    vkDestroyFence(c.dev,fence,nullptr);
    vkFreeCommandBuffers(c.dev,c.pool,1,&cmd);
}

int main(){
    // ---- instance (1.2) ----
    Ctx c{};
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion=VK_API_VERSION_1_2;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo=&app;
    VKCHECK(vkCreateInstance(&ici,nullptr,&c.inst));

    uint32_t n=0; vkEnumeratePhysicalDevices(c.inst,&n,nullptr);
    std::vector<VkPhysicalDevice> devs(n); vkEnumeratePhysicalDevices(c.inst,&n,devs.data());
    c.phys=VK_NULL_HANDLE;
    for (auto d:devs){
        VkPhysicalDeviceRayQueryFeaturesKHR rq{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
        VkPhysicalDeviceAccelerationStructureFeaturesKHR as{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR}; as.pNext=&rq;
        VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2}; f2.pNext=&as;
        vkGetPhysicalDeviceFeatures2(d,&f2);
        if (rq.rayQuery && as.accelerationStructure){ c.phys=d; break; }
    }
    if (!c.phys){ std::fprintf(stderr,"no ray-query device\n"); return 1; }
    VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(c.phys,&pp);
    std::printf("device: %s\n", pp.deviceName);
    vkGetPhysicalDeviceMemoryProperties(c.phys,&c.memprops);

    uint32_t qn=0; vkGetPhysicalDeviceQueueFamilyProperties(c.phys,&qn,nullptr);
    std::vector<VkQueueFamilyProperties> qf(qn); vkGetPhysicalDeviceQueueFamilyProperties(c.phys,&qn,qf.data());
    c.qf=0; for(uint32_t i=0;i<qn;i++) if(qf[i].queueFlags&VK_QUEUE_COMPUTE_BIT){c.qf=i;break;}

    // ---- device with RT features ----
    float prio=1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex=c.qf; qci.queueCount=1; qci.pQueuePriorities=&prio;
    const char* exts[]={
        VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
        VK_KHR_RAY_QUERY_EXTENSION_NAME,
        VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
        VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
        "VK_KHR_ray_tracing_position_fetch",
        VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME,
        VK_KHR_SPIRV_1_4_EXTENSION_NAME,
        VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME,
    };
    VkPhysicalDeviceRayTracingPositionFetchFeaturesKHR pf{(VkStructureType)1000481000};
    pf.pNext=nullptr;
    // set rayTracingPositionFetch=true via the proper field:
    pf.rayTracingPositionFetch=VK_TRUE;
    VkPhysicalDeviceRayQueryFeaturesKHR rq{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR}; rq.rayQuery=VK_TRUE; rq.pNext=&pf;
    VkPhysicalDeviceAccelerationStructureFeaturesKHR as{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR}; as.accelerationStructure=VK_TRUE; as.pNext=&rq;
    VkPhysicalDeviceBufferDeviceAddressFeatures bda{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES}; bda.bufferDeviceAddress=VK_TRUE; bda.pNext=&as;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dci.pNext=&bda;
    dci.queueCreateInfoCount=1; dci.pQueueCreateInfos=&qci;
    dci.enabledExtensionCount=sizeof(exts)/sizeof(exts[0]); dci.ppEnabledExtensionNames=exts;
    VKCHECK(vkCreateDevice(c.phys,&dci,nullptr,&c.dev));
    vkGetDeviceQueue(c.dev,c.qf,0,&c.q);

    #define LOAD(name) c.p##name=(PFN_vk##name)vkGetDeviceProcAddr(c.dev,"vk"#name); if(!c.p##name){std::fprintf(stderr,"missing vk"#name"\n");return 1;}
    LOAD(GetBufferDeviceAddressKHR);
    LOAD(CreateAccelerationStructureKHR);
    LOAD(DestroyAccelerationStructureKHR);
    LOAD(GetAccelerationStructureBuildSizesKHR);
    LOAD(CmdBuildAccelerationStructuresKHR);
    LOAD(GetAccelerationStructureDeviceAddressKHR);
    #undef LOAD

    VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpci.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; cpci.queueFamilyIndex=c.qf;
    VKCHECK(vkCreateCommandPool(c.dev,&cpci,nullptr,&c.pool));

    // ---- mesh: an open box-ish scene. Use a unit cube (12 tris) so rays from a
    // point on one face that go inward hit the opposite/adjacent faces. ----
    Mesh m;
    auto add=[&](V3 a,V3 b,V3 cc){ uint32_t base=(uint32_t)m.verts.size(); m.verts.push_back(a);m.verts.push_back(b);m.verts.push_back(cc); m.idx.push_back(base);m.idx.push_back(base+1);m.idx.push_back(base+2); };
    // cube corners
    V3 p000{0,0,0},p100{1,0,0},p010{0,1,0},p110{1,1,0},p001{0,0,1},p101{1,0,1},p011{0,1,1},p111{1,1,1};
    // 6 faces, outward winding (CCW seen from outside)
    add(p000,p010,p110); add(p000,p110,p100); // bottom z=0
    add(p001,p101,p111); add(p001,p111,p011); // top z=1
    add(p000,p100,p101); add(p000,p101,p001); // y=0
    add(p010,p011,p111); add(p010,p111,p110); // y=1
    add(p000,p001,p011); add(p000,p011,p010); // x=0
    add(p100,p110,p111); add(p100,p111,p101); // x=1
    // A large occluder plate at z=2, normal pointing DOWN (toward the cube, -Z).
    // A sample on the cube top (z=1, normal +Z) casts rays up; many hit this
    // plate whose front-face normal (-Z) opposes the ray dir -> dot<=0 ->
    // decrease. This exercises the visibility-reduction path on convex geometry.
    V3 q0{-5,-5,2},q1{5,-5,2},q2{5,5,2},q3{-5,5,2};
    add(q0,q2,q1); add(q0,q3,q2); // reversed winding -> geometric normal = -Z

    // ---- samples: a few points on faces with their outward normals ----
    // Mix: outward-facing samples (rays escape -> vis~1) AND inward-facing
    // samples on interior faces (rays hit opposite walls -> vis<1), exercising
    // the decrease_step path and the face-normal dot<=0 test.
    std::vector<V3> spos = {
        {0.5f,0.5f,0.0f}, {0.5f,0.5f,1.0f}, {0.0f,0.5f,0.5f}, {0.5f,0.0f,0.5f}, {0.3f,0.7f,0.0f},
        {0.5f,0.5f,0.0f}, {0.5f,0.5f,1.0f}, {0.0f,0.5f,0.5f}, {0.5f,0.5f,0.5f},
    };
    std::vector<V3> snrm = {
        {0,0,-1}, {0,0,1}, {-1,0,0}, {0,-1,0}, {0,0,-1},
        {0,0,1}, {0,0,-1}, {1,0,0}, {0.3f,0.4f,0.6f},
    };
    const int N=5; const float off=0.01f;
    int S=(int)spos.size();

    // CPU reference
    std::vector<float> cpu(S);
    for(int i=0;i<S;i++) cpu[i]=cpu_visibility(m,spos[i],snrm[i],N,off);

    // ---- GPU buffers ----
    // vertices as vec3 (R32G32B32_SFLOAT), indices uint32
    std::vector<float> vbuf; for(auto&v:m.verts){vbuf.push_back(v.x);vbuf.push_back(v.y);vbuf.push_back(v.z);}
    Buf vb=make_buf(c,vbuf.size()*4,
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR|VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,true);
    upload(c,vb,vbuf.data(),vbuf.size()*4);
    Buf ib=make_buf(c,m.idx.size()*4,
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR|VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,true);
    upload(c,ib,m.idx.data(),m.idx.size()*4);

    // ---- BLAS ----
    VkAccelerationStructureGeometryKHR geom{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geom.geometryType=VK_GEOMETRY_TYPE_TRIANGLES_KHR; geom.flags=VK_GEOMETRY_OPAQUE_BIT_KHR;
    geom.geometry.triangles.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    geom.geometry.triangles.vertexFormat=VK_FORMAT_R32G32B32_SFLOAT;
    geom.geometry.triangles.vertexData.deviceAddress=buf_addr(c,vb.buf);
    geom.geometry.triangles.vertexStride=12;
    geom.geometry.triangles.maxVertex=(uint32_t)m.verts.size()-1;
    geom.geometry.triangles.indexType=VK_INDEX_TYPE_UINT32;
    geom.geometry.triangles.indexData.deviceAddress=buf_addr(c,ib.buf);

    uint32_t prim=(uint32_t)(m.idx.size()/3);
    VkAccelerationStructureBuildGeometryInfoKHR bgi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    bgi.type=VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    bgi.flags=VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    bgi.mode=VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    bgi.geometryCount=1; bgi.pGeometries=&geom;
    VkAccelerationStructureBuildSizesInfoKHR sz{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    c.pGetAccelerationStructureBuildSizesKHR(c.dev,VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,&bgi,&prim,&sz);

    Buf blasBuf=make_buf(c,sz.accelerationStructureSize,
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR|VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,true);
    VkAccelerationStructureCreateInfoKHR aci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    aci.buffer=blasBuf.buf; aci.size=sz.accelerationStructureSize; aci.type=VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    VkAccelerationStructureKHR blas; VKCHECK(c.pCreateAccelerationStructureKHR(c.dev,&aci,nullptr,&blas));
    Buf scratch=make_buf(c,sz.buildScratchSize,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,true);
    bgi.dstAccelerationStructure=blas; bgi.scratchData.deviceAddress=buf_addr(c,scratch.buf);
    VkAccelerationStructureBuildRangeInfoKHR range{}; range.primitiveCount=prim;
    const VkAccelerationStructureBuildRangeInfoKHR* pr=&range;
    { VkCommandBuffer cmd=begin_cmd(c); c.pCmdBuildAccelerationStructuresKHR(cmd,1,&bgi,&pr); end_cmd(c,cmd); }
    VkAccelerationStructureDeviceAddressInfoKHR dai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
    dai.accelerationStructure=blas; VkDeviceAddress blasAddr=c.pGetAccelerationStructureDeviceAddressKHR(c.dev,&dai);

    // ---- TLAS (1 instance, identity) ----
    VkAccelerationStructureInstanceKHR inst{};
    inst.transform.matrix[0][0]=inst.transform.matrix[1][1]=inst.transform.matrix[2][2]=1.0f;
    inst.mask=0xFF; inst.accelerationStructureReference=blasAddr;
    Buf instBuf=make_buf(c,sizeof(inst),
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR|VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,true);
    upload(c,instBuf,&inst,sizeof(inst));

    VkAccelerationStructureGeometryKHR tgeom{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    tgeom.geometryType=VK_GEOMETRY_TYPE_INSTANCES_KHR; tgeom.flags=VK_GEOMETRY_OPAQUE_BIT_KHR;
    tgeom.geometry.instances.sType=VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    tgeom.geometry.instances.data.deviceAddress=buf_addr(c,instBuf.buf);
    VkAccelerationStructureBuildGeometryInfoKHR tbgi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    tbgi.type=VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    tbgi.flags=VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    tbgi.mode=VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    tbgi.geometryCount=1; tbgi.pGeometries=&tgeom;
    uint32_t tprim=1;
    VkAccelerationStructureBuildSizesInfoKHR tsz{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    c.pGetAccelerationStructureBuildSizesKHR(c.dev,VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,&tbgi,&tprim,&tsz);
    Buf tlasBuf=make_buf(c,tsz.accelerationStructureSize,
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR|VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,true);
    VkAccelerationStructureCreateInfoKHR taci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    taci.buffer=tlasBuf.buf; taci.size=tsz.accelerationStructureSize; taci.type=VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    VkAccelerationStructureKHR tlas; VKCHECK(c.pCreateAccelerationStructureKHR(c.dev,&taci,nullptr,&tlas));
    Buf tscratch=make_buf(c,tsz.buildScratchSize,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,true);
    tbgi.dstAccelerationStructure=tlas; tbgi.scratchData.deviceAddress=buf_addr(c,tscratch.buf);
    VkAccelerationStructureBuildRangeInfoKHR trange{}; trange.primitiveCount=1;
    const VkAccelerationStructureBuildRangeInfoKHR* tpr=&trange;
    { VkCommandBuffer cmd=begin_cmd(c); c.pCmdBuildAccelerationStructuresKHR(cmd,1,&tbgi,&tpr); end_cmd(c,cmd); }

    // ---- sample buffers (vec4 padded) ----
    std::vector<float> posb,nrmb;
    for(int i=0;i<S;i++){ posb.push_back(spos[i].x);posb.push_back(spos[i].y);posb.push_back(spos[i].z);posb.push_back(0);
                          nrmb.push_back(snrm[i].x);nrmb.push_back(snrm[i].y);nrmb.push_back(snrm[i].z);nrmb.push_back(0);}
    Buf pb=make_buf(c,posb.size()*4,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    upload(c,pb,posb.data(),posb.size()*4);
    Buf nb=make_buf(c,nrmb.size()*4,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    upload(c,nb,nrmb.data(),nrmb.size()*4);
    Buf rb=make_buf(c,S*4,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    // ---- pipeline ----
    VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize=seam_raycast_spv_size*4; smci.pCode=seam_raycast_spv;
    VkShaderModule sm; VKCHECK(vkCreateShaderModule(c.dev,&smci,nullptr,&sm));

    VkDescriptorSetLayoutBinding binds[4]{};
    binds[0]={0,VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    binds[1]={1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    binds[2]={2,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    binds[3]={3,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    VkDescriptorSetLayoutCreateInfo dslci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dslci.bindingCount=4; dslci.pBindings=binds;
    VkDescriptorSetLayout dsl; VKCHECK(vkCreateDescriptorSetLayout(c.dev,&dslci,nullptr,&dsl));

    struct PC{ uint32_t sc, sqr; float off; } pc{(uint32_t)S,(uint32_t)N,off};
    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(PC)};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount=1; plci.pSetLayouts=&dsl; plci.pushConstantRangeCount=1; plci.pPushConstantRanges=&pcr;
    VkPipelineLayout pl; VKCHECK(vkCreatePipelineLayout(c.dev,&plci,nullptr,&pl));
    VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.stage=VK_SHADER_STAGE_COMPUTE_BIT; stage.module=sm; stage.pName="main";
    VkComputePipelineCreateInfo cpci2{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpci2.stage=stage; cpci2.layout=pl;
    VkPipeline pipe; VKCHECK(vkCreateComputePipelines(c.dev,VK_NULL_HANDLE,1,&cpci2,nullptr,&pipe));

    VkDescriptorPoolSize psz[2]={{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,1},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,3}};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets=1; dpci.poolSizeCount=2; dpci.pPoolSizes=psz;
    VkDescriptorPool dpool; VKCHECK(vkCreateDescriptorPool(c.dev,&dpci,nullptr,&dpool));
    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool=dpool; dsai.descriptorSetCount=1; dsai.pSetLayouts=&dsl;
    VkDescriptorSet dset; VKCHECK(vkAllocateDescriptorSets(c.dev,&dsai,&dset));

    VkWriteDescriptorSetAccelerationStructureKHR asw{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
    asw.accelerationStructureCount=1; asw.pAccelerationStructures=&tlas;
    VkDescriptorBufferInfo pbi{pb.buf,0,VK_WHOLE_SIZE},nbi{nb.buf,0,VK_WHOLE_SIZE},rbi{rb.buf,0,VK_WHOLE_SIZE};
    VkWriteDescriptorSet w[4]{};
    w[0]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,&asw,dset,0,0,1,VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR};
    w[1]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,dset,1,0,1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,nullptr,&pbi};
    w[2]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,dset,2,0,1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,nullptr,&nbi};
    w[3]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,dset,3,0,1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,nullptr,&rbi};
    vkUpdateDescriptorSets(c.dev,4,w,0,nullptr);

    { VkCommandBuffer cmd=begin_cmd(c);
      vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipe);
      vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pl,0,1,&dset,0,nullptr);
      vkCmdPushConstants(cmd,pl,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(pc),&pc);
      vkCmdDispatch(cmd,(S+63)/64,1,1);
      VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; mb.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT; mb.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
      vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&mb,0,nullptr,0,nullptr);
      end_cmd(c,cmd);
    }

    std::vector<float> gpu(S);
    { void* p; VKCHECK(vkMapMemory(c.dev,rb.mem,0,S*4,0,&p)); std::memcpy(gpu.data(),p,S*4); vkUnmapMemory(c.dev,rb.mem); }

    std::printf("\n sample      CPU       GPU      |diff|\n");
    float maxd=0;
    for(int i=0;i<S;i++){ float d=std::fabs(cpu[i]-gpu[i]); maxd=std::max(maxd,d);
        std::printf("  [%d]    %8.5f  %8.5f   %.6f\n", i, cpu[i], gpu[i], d); }
    std::printf("\n max |diff| = %.6f  -> %s\n", maxd, maxd<1e-4f?"MATCH":"MISMATCH");
    return maxd<1e-4f?0:2;
}
