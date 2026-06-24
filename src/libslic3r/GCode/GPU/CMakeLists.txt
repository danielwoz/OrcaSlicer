# Optional GPU seam-visibility ray caster (SLIC3R_VULKAN).
#
# Built only when SLIC3R_VULKAN is ON, linked PRIVATE into libslic3r. When the
# option is OFF this directory is never added and none of this code compiles in
# (SeamPlacer.cpp's GPU path is guarded by the same #ifdef + a header that is
# inert when OFF).
#
# Uses VK_KHR_ray_query + VK_KHR_acceleration_structure to first-hit-trace the
# 750k seam-visibility rays SeamPlacer casts per object. The compute shader is
# precompiled to embedded SPIR-V (seam_raycast_spv.hpp), so no runtime/buildtime
# external shader compiler is needed. Regenerate via gen_seam_spv.cpp (a dev tool
# that is NOT part of the build; see seam_raycast.comp).

add_library(libslic3r_seam_gpu STATIC
    SeamRayCaster.hpp
    SeamRayCaster.cpp
    seam_raycast_spv.hpp
    # GPU mesh slicing (plain Vulkan compute, NO ray query / accel structures).
    # One invocation per facet ports slice_facet(); embedded SPIR-V regenerated
    # from slice_facets.comp via the same gen_seam_spv.cpp dev tool.
    MeshSlicer.hpp
    MeshSlicer.cpp
    slice_facets_spv.hpp
)

# Reuse libslic3r's resolved include set (admesh, eigen, boost, ...) so this
# sees exactly what libslic3r sees. libslic3r is defined before this is added.
target_include_directories(libslic3r_seam_gpu PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    $<TARGET_PROPERTY:libslic3r,INCLUDE_DIRECTORIES>
)

target_compile_definitions(libslic3r_seam_gpu PUBLIC SLIC3R_VULKAN)

# Static Vulkan loader (target "vulkan") + header-only Vulkan-Headers interface.
target_link_libraries(libslic3r_seam_gpu
    PUBLIC
        Vulkan::Headers
    PRIVATE
        vulkan
        boost_libs
)

set_property(TARGET libslic3r_seam_gpu PROPERTY FOLDER "libslic3r")

# --------------------------------------------------------------------------
# Validation harness (EXCLUDE_FROM_ALL, not part of ctest). Loads a real mesh,
# replicates SeamPlacer's sampling, and compares CPU vs GPU per-sample
# visibility at N=5 + a GPU ray-count sweep. Build:
#   cmake --build build --config Release --target seam_gpu_validate -j32
# Run: build/src/libslic3r/GCode/GPU/Release/seam_gpu_validate 3DBenchy
# --------------------------------------------------------------------------
if (BUILD_TESTS)
    add_executable(seam_gpu_validate EXCLUDE_FROM_ALL
        seam_gpu_validate.cpp
        # nanosvg impl TU: load_drc / slicing path pull in nsvg* symbols normally
        # provided by the GUI lib.
        ${CMAKE_SOURCE_DIR}/tests/sla_print/sla_raster_bench_nanosvg.cpp)
    target_include_directories(seam_gpu_validate PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}
        $<TARGET_PROPERTY:libslic3r,INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:nanosvg,INTERFACE_INCLUDE_DIRECTORIES>)
    target_compile_definitions(seam_gpu_validate PRIVATE
        SLIC3R_VULKAN
        MODELS_DIR=R"\(${CMAKE_SOURCE_DIR}/resources/handy_models\)")
    target_link_libraries(seam_gpu_validate PRIVATE
        libslic3r libslic3r_seam_gpu boost_libs)
    set_property(TARGET seam_gpu_validate PROPERTY FOLDER "libslic3r")

    # ----------------------------------------------------------------------
    # GPU mesh-slicing validation + benchmark harness. Loads a .drc, builds the
    # exact CPU slice_make_lines() inputs (XY-scaled vertices, unscaled sorted
    # zs, face_edge_ids), runs CPU vs GPU, checks geometric equivalence per
    # layer, and reports upload / kernel / copy-back / end-to-end timings.
    #   cmake --build build --config Release --target mesh_slice_validate -j32
    #   build/src/libslic3r/GCode/GPU/Release/mesh_slice_validate 3DBenchy 0.12
    # ----------------------------------------------------------------------
    add_executable(mesh_slice_validate EXCLUDE_FROM_ALL
        mesh_slice_validate.cpp
        ${CMAKE_SOURCE_DIR}/tests/sla_print/sla_raster_bench_nanosvg.cpp)
    target_include_directories(mesh_slice_validate PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}
        $<TARGET_PROPERTY:libslic3r,INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:nanosvg,INTERFACE_INCLUDE_DIRECTORIES>)
    target_compile_definitions(mesh_slice_validate PRIVATE
        SLIC3R_VULKAN
        MODELS_DIR=R"\(${CMAKE_SOURCE_DIR}/resources/handy_models\)")
    target_link_libraries(mesh_slice_validate PRIVATE
        libslic3r libslic3r_seam_gpu boost_libs)
    # libslic3r is a ThinLTO archive (LLVM bitcode) when SLIC3R_LTO=ON; the default
    # GNU ld can't read it, so this dev harness must link with lld (which carries
    # the LTO plugin). Harmless when LTO is off.
    target_link_options(mesh_slice_validate PRIVATE -fuse-ld=lld)
    set_property(TARGET mesh_slice_validate PROPERTY FOLDER "libslic3r")
endif()
