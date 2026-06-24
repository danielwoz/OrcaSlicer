// Validation harness for the GPU seam-visibility ray caster.
//
// Loads a .drc/.stl mesh, replicates SeamPlacer's preprocessing (decimation +
// uniform surface sampling), builds the AABB tree, then computes per-sample
// visibility on BOTH the CPU (the exact non-negative branch of
// raycast_visibility) and the GPU (SeamRayCaster), and reports the per-sample
// delta + timing. Also sweeps GPU ray counts to show the "faster AND more
// accurate" headroom (N=5 vs N up to the requested max).
//
// Build (BUILD_TESTS + SLIC3R_VULKAN ON):
//   cmake --build build --config Release --target seam_gpu_validate -j32
// Run headless:
//   build/src/libslic3r/GCode/GPU/Release/seam_gpu_validate <model_basename> [N]

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <chrono>
#include <vector>
#include <string>
#include <algorithm>
#include <cmath>

#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Format/DRC.hpp"
#include "libslic3r/AABBTreeIndirect.hpp"
#include "libslic3r/TriangleSetSampling.hpp"
#include "libslic3r/ShortEdgeCollapse.hpp"
#include "libslic3r/Point.hpp"
#include "tbb/parallel_for.h"
#include "tbb/blocked_range.h"

#include "SeamRayCaster.hpp"

using namespace Slic3r;

namespace { constexpr size_t SQR_RAYS = 5; constexpr size_t SAMPLES = 30000; constexpr size_t DECIM = 30000; }

// ---- CPU reference: the exact non-negative branch of raycast_visibility -----
class Frame {
public:
    Vec3f mX{1,0,0}, mY{0,1,0}, mZ{0,0,1};
    void set_from_z(const Vec3f &z) {
        mZ = z.normalized();
        Vec3f tmpZ = mZ;
        Vec3f tmpX = (std::abs(tmpZ.x()) > 0.99f) ? Vec3f(0,1,0) : Vec3f(1,0,0);
        mY = (tmpZ.cross(tmpX)).normalized();
        mX = mY.cross(tmpZ);
    }
    Vec3f to_world(const Vec3f &a) const { return a.x()*mX + a.y()*mY + a.z()*mZ; }
};
static Vec3f sample_hemisphere_uniform(const Vec2f &s) {
    float t1 = 2.0f*float(PI)*s.x();
    float t2 = 2.0f*std::sqrt(s.y() - s.y()*s.y());
    return {std::cos(t1)*t2, std::sin(t1)*t2, std::abs(1.0f - 2.0f*s.y())};
}

static std::vector<float> cpu_visibility(const AABBTreeIndirect::Tree<3,float> &tree,
                                         const indexed_triangle_set &tri,
                                         const TriangleSetSamples &samples) {
    float step = 1.0f/SQR_RAYS;
    std::vector<Vec3f> dirs(SQR_RAYS*SQR_RAYS);
    for (size_t xi=0; xi<SQR_RAYS; ++xi){ float sx=xi*step+step/2.0f;
      for (size_t yi=0; yi<SQR_RAYS; ++yi){ float sy=yi*step+step/2.0f;
        dirs[xi*SQR_RAYS+yi]=sample_hemisphere_uniform({sx,sy}); } }
    std::vector<float> res(samples.positions.size());
    constexpr float dec = 1.0f/(SQR_RAYS*SQR_RAYS);
    auto body = [&](size_t i){
        res[i]=1.0f;
        const Vec3f &c=samples.positions[i], &n=samples.normals[i];
        Frame f; f.set_from_z(n);
        for (auto &d:dirs){
            Vec3f wd=f.to_world(d);
            igl::Hit<float> hp;
            Vec3d wdd=wd.cast<double>(), od=(c+n*0.01f).cast<double>();
            bool hit=AABBTreeIndirect::intersect_ray_first_hit(tri.vertices,tri.indices,tree,od,wdd,hp);
            if (hit && its_face_normal(tri,hp.id).dot(wd)<=0) res[i]-=dec;
        }
    };
    if (std::getenv("CPU_SINGLE"))
        for (size_t i=0;i<res.size();++i) body(i);
    else
        tbb::parallel_for(tbb::blocked_range<size_t>(0,res.size()),
            [&](const tbb::blocked_range<size_t>&r){ for(size_t i=r.begin();i<r.end();++i) body(i); });
    return res;
}

int main(int argc, char **argv) {
    std::string base = argc>1 ? argv[1] : "3DBenchy";
    int gpu_max_N = argc>2 ? std::atoi(argv[2]) : 5;
    std::string path = std::string(MODELS_DIR) + "/" + base + ".drc";

    TriangleMesh mesh;
    if (!load_drc(path.c_str(), &mesh) || mesh.empty()) {
        std::fprintf(stderr, "failed to load %s\n", path.c_str());
        return 1;
    }
    indexed_triangle_set tri = mesh.its;
    its_short_edge_collpase(tri, DECIM);
    std::printf("model %s: %zu tris after decimation\n", base.c_str(), tri.indices.size());

    TriangleSetSamples samples = sample_its_uniform_parallel(SAMPLES, tri);
    std::printf("samples: %zu\n", samples.positions.size());

    auto tree = AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(tri.vertices, tri.indices);

    // CPU reference (N=5).
    auto t0 = std::chrono::high_resolution_clock::now();
    std::vector<float> cpu = cpu_visibility(tree, tri, samples);
    auto t1 = std::chrono::high_resolution_clock::now();
    double cpu_ms = std::chrono::duration<double,std::milli>(t1-t0).count();
    std::printf("CPU (N=5): %.1f ms (%s)\n", cpu_ms,
                std::getenv("CPU_SINGLE") ? "single-threaded" : "TBB-parallel, matches slicer");

    auto i0 = std::chrono::high_resolution_clock::now();
    auto rc = seam_gpu::SeamRayCaster::get();
    auto i1 = std::chrono::high_resolution_clock::now();
    std::printf("GPU context init (one-time, instance+device+pipeline): %.1f ms\n",
                std::chrono::duration<double,std::milli>(i1-i0).count());
    if (!rc || !rc->is_usable()) { std::fprintf(stderr, "no GPU ray-query device\n"); return 2; }
    std::printf("GPU device: %s\n", rc->device_name().c_str());

    // GPU at N=5 (validation): compare to CPU.
    std::vector<float> gpu5;
    auto g0 = std::chrono::high_resolution_clock::now();
    bool ok = rc->compute_visibility(tri, samples, 5, 0.01f, gpu5);
    auto g1 = std::chrono::high_resolution_clock::now();
    if (!ok) { std::fprintf(stderr, "GPU compute failed\n"); return 3; }
    double gpu5_ms = std::chrono::duration<double,std::milli>(g1-g0).count();

    double sum=0, mx=0; size_t over=0;
    for (size_t i=0;i<cpu.size();++i){ double d=std::fabs(cpu[i]-gpu5[i]); sum+=d; mx=std::max(mx,d); if(d>1e-4) over++; }
    std::printf("\n=== N=5 GPU vs CPU validation ===\n");
    std::printf("mean |diff| = %.6f   max |diff| = %.6f   samples>1e-4: %zu / %zu\n",
                sum/cpu.size(), mx, over, cpu.size());
    std::printf("GPU N=5 incl. BLAS/TLAS build + readback: %.1f ms   (CPU 1-thread ref: %.1f ms)\n",
                gpu5_ms, cpu_ms);

    // GPU ray-count sweep (the "more accurate" headroom). Time only.
    std::printf("\n=== GPU ray-count sweep (build+trace+readback) ===\n");
    for (int N : {5, 10, 16}) {
        if (N > gpu_max_N && N != 5) continue;
        std::vector<float> g;
        auto a=std::chrono::high_resolution_clock::now();
        rc->compute_visibility(tri, samples, (uint32_t)N, 0.01f, g);
        auto b=std::chrono::high_resolution_clock::now();
        double ms=std::chrono::duration<double,std::milli>(b-a).count();
        std::printf("  N=%2d (%4d rays/sample): %.1f ms\n", N, N*N, ms);
    }
    return 0;
}
