// Validation + benchmark harness for the GPU mesh slicer (MeshSlicer).
//
// Loads a .drc, builds the EXACT inputs the multi-z CPU slice_make_lines() path
// uses (XY-scaled vertices with unscaled Z, sorted unscaled zs, its_face_edge_ids),
// runs the CPU reference (a verbatim copy of slice_facet/slice_make_lines, since
// those are file-local statics) and the GPU path, and reports:
//   * geometric equivalence: per-layer segment counts, total segment length, XY
//     bbox, and the fraction of segments that match the CPU bit-for-bit.
//   * timing: upload, kernel, copy-back (GPU-internal) and end-to-end CPU vs GPU.
//
// Build (BUILD_TESTS + SLIC3R_VULKAN):
//   cmake --build build --config Release --target mesh_slice_validate -j32
// Run:
//   build/src/libslic3r/GCode/GPU/Release/mesh_slice_validate <model> [layer_h] [reps]

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <chrono>
#include <vector>
#include <string>
#include <array>
#include <algorithm>
#include <cmath>
#include <mutex>

#include "libslic3r/libslic3r.h"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Format/DRC.hpp"
#include "libslic3r/Point.hpp"
#include "tbb/parallel_for.h"
#include "tbb/blocked_range.h"
#include <boost/thread/mutex.hpp>
#include <boost/thread/lock_guard.hpp>

#include "MeshSlicer.hpp"

using namespace Slic3r;

// ----------------------------------------------------------------------------
// CPU reference: verbatim copy of the relevant slice_facet / slice_make_lines
// from TriangleMeshSlicer.cpp (those are file-local static, not exported), trimmed
// to the multi-z, identity-transform path used for the bench.
// ----------------------------------------------------------------------------
namespace ref {

struct RLine { coord_t ax, ay, bx, by; int a_id, b_id, edge_a_id, edge_b_id, edge_type; };
using RLines = std::vector<RLine>;

enum class FacetSliceType { NoSlice = 0, Slicing = 1, Cutting = 2 };
// edge_type ints: 0 General, 1 Top, 2 Bottom, 4 Horizontal

static FacetSliceType slice_facet(
    float slice_z, const stl_vertex *vertices,
    const stl_triangle_vertex_indices &indices, const Vec3i32 &edge_ids,
    const int idx_vertex_lowest, const bool horizontal, RLine &line_out) {
    struct IP { coord_t x, y; int point_id = -1, edge_id = -1; };
    IP points[3];
    size_t num_points = 0;
    auto   point_on_layer = size_t(-1);

    for (int j = 0; j < 3; ++j) {
        int edge_id; const stl_vertex *a, *b, *c; int a_id, b_id;
        {
            int k = (idx_vertex_lowest + j) % 3;
            int l = (k + 1) % 3;
            edge_id = edge_ids(k);
            a_id = indices[k]; a = vertices + k;
            b_id = indices[l]; b = vertices + l;
            c = vertices + (k + 2) % 3;
        }
        if (a->z() == slice_z && b->z() == slice_z) {
            const stl_vertex &v0 = vertices[0]; const stl_vertex &v1 = vertices[1]; const stl_vertex &v2 = vertices[2];
            FacetSliceType result = FacetSliceType::Slicing;
            if (horizontal) {
                line_out.edge_type = 4; result = FacetSliceType::Cutting;
                double normal = (v1.x()-v0.x())*(v2.y()-v1.y()) - (v1.y()-v0.y())*(v2.x()-v1.x());
                if (normal < 0) { std::swap(a, b); std::swap(a_id, b_id); }
            } else {
                bool third_below = v0.z() < slice_z || v1.z() < slice_z || v2.z() < slice_z;
                result = third_below ? FacetSliceType::Slicing : FacetSliceType::Cutting;
                if (third_below) { line_out.edge_type = 1; std::swap(a, b); std::swap(a_id, b_id); }
                else line_out.edge_type = 2;
            }
            line_out.ax = (coord_t)a->x(); line_out.ay = (coord_t)a->y();
            line_out.bx = (coord_t)b->x(); line_out.by = (coord_t)b->y();
            line_out.a_id = a_id; line_out.b_id = b_id;
            line_out.edge_a_id = -1; line_out.edge_b_id = -1;
            return result;
        }
        if (a->z() == slice_z) {
            if (point_on_layer == size_t(-1) || points[point_on_layer].point_id != a_id) {
                point_on_layer = num_points; IP &p = points[num_points++];
                p.x = (coord_t)a->x(); p.y = (coord_t)a->y(); p.point_id = a_id;
            }
        } else if (b->z() == slice_z) {
            if (point_on_layer == size_t(-1) || points[point_on_layer].point_id != b_id) {
                point_on_layer = num_points; IP &p = points[num_points++];
                p.x = (coord_t)b->x(); p.y = (coord_t)b->y(); p.point_id = b_id;
            }
        } else if ((a->z() < slice_z && b->z() > slice_z) || (b->z() < slice_z && a->z() > slice_z)) {
            if (a_id > b_id) { std::swap(a_id, b_id); std::swap(a, b); }
            IP &p = points[num_points];
            double t = (double(slice_z) - double(b->z())) / (double(a->z()) - double(b->z()));
            if (t <= 0.) {
                if (point_on_layer == size_t(-1) || points[point_on_layer].point_id != a_id) {
                    p.x = (coord_t)a->x(); p.y = (coord_t)a->y(); point_on_layer = num_points++; p.point_id = a_id; }
            } else if (t >= 1.) {
                if (point_on_layer == size_t(-1) || points[point_on_layer].point_id != b_id) {
                    p.x = (coord_t)b->x(); p.y = (coord_t)b->y(); point_on_layer = num_points++; p.point_id = b_id; }
            } else {
                p.x = coord_t(floor(double(b->x()) + (double(a->x()) - double(b->x())) * t + 0.5));
                p.y = coord_t(floor(double(b->y()) + (double(a->y()) - double(b->y())) * t + 0.5));
                p.edge_id = edge_id; ++num_points;
            }
        }
    }
    if (num_points == 2) {
        line_out.edge_type = 0;
        line_out.ax = points[1].x; line_out.ay = points[1].y;
        line_out.bx = points[0].x; line_out.by = points[0].y;
        line_out.a_id = points[1].point_id; line_out.b_id = points[0].point_id;
        line_out.edge_a_id = points[1].edge_id; line_out.edge_b_id = points[0].edge_id;
        return FacetSliceType::Slicing;
    }
    return FacetSliceType::NoSlice;
}

static void slice_facet_at_zs(
    const std::vector<Vec3f> &mesh_vertices,
    const stl_triangle_vertex_indices &indices, const Vec3i32 &edge_ids,
    const std::vector<float> &zs, std::vector<RLines> &lines,
    std::array<std::mutex, 64> &lines_mutex) {
    stl_vertex vertices[3] { mesh_vertices[indices(0)], mesh_vertices[indices(1)], mesh_vertices[indices(2)] };
    const float min_z = fminf(vertices[0].z(), fminf(vertices[1].z(), vertices[2].z()));
    const float max_z = fmaxf(vertices[0].z(), fmaxf(vertices[1].z(), vertices[2].z()));
    auto min_layer = std::lower_bound(zs.begin(), zs.end(), min_z);
    auto max_layer = std::upper_bound(min_layer, zs.end(), max_z);
    int  idx_vertex_lowest = (vertices[1].z() == min_z) ? 1 : ((vertices[2].z() == min_z) ? 2 : 0);
    for (auto it = min_layer; it != max_layer; ++it) {
        RLine il{};
        if (min_z != max_z && slice_facet(*it, vertices, indices, edge_ids, idx_vertex_lowest, false, il) == FacetSliceType::Slicing) {
            size_t slice_id = it - zs.begin();
            std::lock_guard<std::mutex> l(lines_mutex[slice_id % lines_mutex.size()]);
            lines[slice_id].emplace_back(il);
        }
    }
}

static std::vector<RLines> slice_make_lines(
    const std::vector<stl_vertex> &vertices,
    const std::vector<stl_triangle_vertex_indices> &indices,
    const std::vector<Vec3i32> &face_edge_ids, const std::vector<float> &zs) {
    std::vector<RLines> lines(zs.size());
    std::array<std::mutex, 64> lines_mutex;
    tbb::parallel_for(tbb::blocked_range<int>(0, int(indices.size())),
        [&](const tbb::blocked_range<int> &range) {
            for (int f = range.begin(); f < range.end(); ++f)
                slice_facet_at_zs(vertices, indices[f], face_edge_ids[f], zs, lines, lines_mutex);
        });
    return lines;
}

} // namespace ref

// ----------------------------------------------------------------------------
using clk = std::chrono::high_resolution_clock;
static double ms(clk::time_point a, clk::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

// Canonical key for an undirected-but-oriented segment match: we compare the
// segment endpoints + ids + edge_type exactly (geometry must be bit-identical to
// keep make_loops stable). Returns a sortable tuple-string.
struct SegKey {
    coord_t ax, ay, bx, by; int a_id, b_id, ea, eb, et;
    bool operator<(const SegKey &o) const {
        return std::tie(ax,ay,bx,by,a_id,b_id,ea,eb,et) <
               std::tie(o.ax,o.ay,o.bx,o.by,o.a_id,o.b_id,o.ea,o.eb,o.et);
    }
    bool operator==(const SegKey &o) const {
        return ax==o.ax&&ay==o.ay&&bx==o.bx&&by==o.by&&a_id==o.a_id&&b_id==o.b_id&&ea==o.ea&&eb==o.eb&&et==o.et;
    }
};

int main(int argc, char **argv) {
    std::string base = argc > 1 ? argv[1] : "3DBenchy";
    double layer_h = argc > 2 ? std::atof(argv[2]) : 0.12;
    int reps = argc > 3 ? std::atoi(argv[3]) : 5;
    std::string path = std::string(MODELS_DIR) + "/" + base + ".drc";

    TriangleMesh tmesh;
    if (!load_drc(path.c_str(), &tmesh) || tmesh.empty()) {
        std::fprintf(stderr, "failed to load %s\n", path.c_str()); return 1;
    }
    const indexed_triangle_set &mesh = tmesh.its;
    std::printf("model %s: %zu verts, %zu tris\n", base.c_str(), mesh.vertices.size(), mesh.indices.size());

    // Build zs the way the pipeline does: from min Z to max Z, step = layer_h,
    // first layer at layer_h/2 (initial layer); unscaled, sorted ascending.
    float minz = 1e30f, maxz = -1e30f;
    for (auto &v : mesh.vertices) { minz = std::min(minz, v.z()); maxz = std::max(maxz, v.z()); }
    std::vector<float> zs;
    for (float z = minz + float(layer_h) * 0.5f; z < maxz; z += float(layer_h)) zs.push_back(z);
    std::sort(zs.begin(), zs.end());
    std::printf("layers: %zu (h=%.3f, z in [%.2f, %.2f])\n", zs.size(), layer_h, minz, maxz);

    // CPU inputs: face_edge_ids, XY-scaled vertices (Z unscaled), identity transform.
    std::vector<Vec3i32> face_edge_ids = its_face_edge_ids(mesh);
    std::vector<stl_vertex> verts(mesh.vertices);
    const double s = 1. / SCALING_FACTOR;
    for (auto &v : verts) { v.x() *= float(s); v.y() *= float(s); }

    // ---- CPU reference (end-to-end of the slice_make_lines stage) ----
    double cpu_best = 1e30;
    std::vector<ref::RLines> cpu_lines;
    for (int r = 0; r < reps; ++r) {
        auto t0 = clk::now();
        auto l = ref::slice_make_lines(verts, mesh.indices, face_edge_ids, zs);
        double t = ms(t0, clk::now());
        cpu_best = std::min(cpu_best, t);
        if (r == 0) cpu_lines = std::move(l);
    }
    size_t cpu_total = 0; for (auto &v : cpu_lines) cpu_total += v.size();
    std::printf("CPU slice_make_lines: %.2f ms (best of %d), %zu segments\n", cpu_best, reps, cpu_total);

    // ---- GPU ----
    // Flatten inputs once (this is part of the offered-to-GPU work; counted under upload).
    std::vector<float> vflat(verts.size() * 3);
    for (size_t i = 0; i < verts.size(); ++i) { vflat[i*3]=verts[i].x(); vflat[i*3+1]=verts[i].y(); vflat[i*3+2]=verts[i].z(); }
    std::vector<int32_t> iflat(mesh.indices.size() * 3), eflat(face_edge_ids.size() * 3);
    for (size_t f = 0; f < mesh.indices.size(); ++f)
        for (int k = 0; k < 3; ++k) { iflat[f*3+k] = mesh.indices[f][k]; eflat[f*3+k] = face_edge_ids[f][k]; }

    auto i0 = clk::now();
    auto gpu = mesh_gpu::MeshSlicer::get();
    std::printf("GPU context init: %.1f ms\n", ms(i0, clk::now()));
    if (!gpu || !gpu->is_usable()) { std::fprintf(stderr, "no usable GPU compute device\n"); return 2; }
    std::printf("GPU device: %s\n", gpu->device_name().c_str());

    std::vector<mesh_gpu::GpuSliceLine> gout;
    double up_ms = 0, k_ms = 0, cb_ms = 0, gpu_best = 1e30, up_best=0, k_best=0, cb_best=0;
    for (int r = 0; r < reps; ++r) {
        auto t0 = clk::now();
        bool ok = gpu->compute_lines(vflat, iflat, eflat, zs, gout, &k_ms, &cb_ms, &up_ms);
        double t = ms(t0, clk::now());
        if (!ok) { std::fprintf(stderr, "GPU compute_lines failed\n"); return 3; }
        if (t < gpu_best) { gpu_best = t; up_best = up_ms; k_best = k_ms; cb_best = cb_ms; }
    }
    std::printf("GPU compute_lines: %.2f ms end-to-end (best of %d), %zu segments\n", gpu_best, reps, gout.size());
    std::printf("   breakdown: upload=%.2f  kernel=%.2f  copyback=%.2f  (other=%.2f)\n",
                up_best, k_best, cb_best, gpu_best - up_best - k_best - cb_best);

    // ---- geometric equivalence ----
    // Build per-layer counts + total length + bbox for both, and exact segment match.
    auto seg_len = [](coord_t ax, coord_t ay, coord_t bx, coord_t by) {
        double dx = double(bx-ax), dy = double(by-ay); return std::sqrt(dx*dx+dy*dy);
    };
    std::vector<size_t> cpu_count(zs.size(), 0), gpu_count(zs.size(), 0);
    double cpu_len = 0, gpu_len = 0;
    coord_t cminx=INT64_MAX,cminy=INT64_MAX,cmaxx=INT64_MIN,cmaxy=INT64_MIN;
    coord_t gminx=INT64_MAX,gminy=INT64_MAX,gmaxx=INT64_MIN,gmaxy=INT64_MIN;
    std::vector<SegKey> cpu_keys, gpu_keys;
    cpu_keys.reserve(cpu_total); gpu_keys.reserve(gout.size());
    for (size_t L = 0; L < cpu_lines.size(); ++L)
        for (auto &l : cpu_lines[L]) {
            cpu_count[L]++; cpu_len += seg_len(l.ax,l.ay,l.bx,l.by);
            cminx=std::min({cminx,l.ax,l.bx}); cmaxx=std::max({cmaxx,l.ax,l.bx});
            cminy=std::min({cminy,l.ay,l.by}); cmaxy=std::max({cmaxy,l.ay,l.by});
            cpu_keys.push_back({l.ax,l.ay,l.bx,l.by,l.a_id,l.b_id,l.edge_a_id,l.edge_b_id,l.edge_type});
        }
    for (auto &g : gout) {
        if (g.layer >= 0 && (size_t)g.layer < gpu_count.size()) gpu_count[g.layer]++;
        gpu_len += seg_len(g.ax,g.ay,g.bx,g.by);
        gminx=std::min<coord_t>({gminx,g.ax,g.bx}); gmaxx=std::max<coord_t>({gmaxx,g.ax,g.bx});
        gminy=std::min<coord_t>({gminy,g.ay,g.by}); gmaxy=std::max<coord_t>({gmaxy,g.ay,g.by});
        gpu_keys.push_back({g.ax,g.ay,g.bx,g.by,g.a_id,g.b_id,g.edge_a_id,g.edge_b_id,g.edge_type});
    }

    size_t layers_mismatch = 0;
    for (size_t L = 0; L < zs.size(); ++L) if (cpu_count[L] != gpu_count[L]) layers_mismatch++;

    std::sort(cpu_keys.begin(), cpu_keys.end());
    std::sort(gpu_keys.begin(), gpu_keys.end());
    size_t exact = 0;
    {
        size_t i = 0, j = 0;
        while (i < cpu_keys.size() && j < gpu_keys.size()) {
            if (cpu_keys[i] == gpu_keys[j]) { exact++; ++i; ++j; }
            else if (cpu_keys[i] < gpu_keys[j]) ++i; else ++j;
        }
    }

    std::printf("\n=== geometric equivalence ===\n");
    std::printf("segments: CPU=%zu  GPU=%zu  (delta=%+lld)\n", cpu_total, gout.size(),
                (long long)gout.size() - (long long)cpu_total);
    std::printf("exact-matching segments: %zu  (%.4f%% of CPU)\n", exact,
                cpu_total ? 100.0*exact/cpu_total : 0.0);
    std::printf("layers with differing segment count: %zu / %zu\n", layers_mismatch, zs.size());
    double len_dev = cpu_len > 0 ? 100.0*std::fabs(gpu_len-cpu_len)/cpu_len : 0.0;
    std::printf("total segment length: CPU=%.1f  GPU=%.1f  dev=%.4f%%\n", cpu_len, gpu_len, len_dev);
    std::printf("bbox CPU: [%lld,%lld,%lld,%lld]\n", (long long)cminx,(long long)cminy,(long long)cmaxx,(long long)cmaxy);
    std::printf("bbox GPU: [%lld,%lld,%lld,%lld]\n", (long long)gminx,(long long)gminy,(long long)gmaxx,(long long)gmaxy);

    bool pass = (exact == cpu_total) && (gout.size() == cpu_total) && (layers_mismatch == 0);
    std::printf("\nVERDICT: %s\n", pass ? "EXACT MATCH (bit-for-bit)"
                : (len_dev < 0.2 && layers_mismatch == 0 ? "GEOMETRICALLY EQUIVALENT (len dev < 0.2%%, no missing loops)"
                : "MISMATCH — investigate"));
    std::printf("TIMING: CPU=%.2f ms  GPU=%.2f ms (kernel=%.2f, copyback=%.2f, upload=%.2f)  speedup=%.2fx\n",
                cpu_best, gpu_best, k_best, cb_best, up_best, cpu_best / gpu_best);
    return pass ? 0 : 4;
}
