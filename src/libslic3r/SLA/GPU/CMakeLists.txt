# Optional GPU SLA rasterizer backend (SLIC3R_VULKAN).
#
# This static library is only built when SLIC3R_VULKAN is ON. It is linked
# PRIVATE into libslic3r. When the option is OFF, this directory is never added
# and none of this code compiles into the slicer.
#
# Phase 2: the backend just delegates to the CPU AGG rasterizer (proves the
# seam + static Vulkan loader link). Phase 3 grows the real VulkanContext +
# fill shader here.

add_library(libslic3r_sla_gpu STATIC
    VulkanRaster.hpp
    VulkanRaster.cpp
    VulkanContext.hpp
    VulkanContext.cpp
    # Phase 5: runtime GPU-vs-CPU selection policy (pick_backend). The core
    # decision is pure (no Vulkan headers); the live wrapper queries VulkanContext.
    SlaBackendPolicy.hpp
    SlaBackendPolicy.cpp
    # Embedded SPIR-V (precompiled from fill_evenodd.comp via glslang in-process
    # at dev time by a tool that is NOT part of the build). No runtime/buildtime
    # external shader compiler is needed.
    fill_evenodd_spv.hpp
)

# The delegate includes AGGRaster.hpp which transitively pulls in a large chunk
# of libslic3r's headers (libslic3r_version.h, semver, boost, eigen, ...). Reuse
# libslic3r's own resolved include set so the seam sees exactly what libslic3r
# sees. (libslic3r is defined before this subdirectory is added.)
target_include_directories(libslic3r_sla_gpu PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    $<TARGET_PROPERTY:libslic3r,INCLUDE_DIRECTORIES>
)

# SLIC3R_VULKAN gates the inert #ifdef blocks in the sources/headers.
target_compile_definitions(libslic3r_sla_gpu PUBLIC SLIC3R_VULKAN)

# Static Vulkan loader (target name "vulkan", built static via BUILD_STATIC_LOADER)
# + header-only Vulkan-Headers interface (target "Vulkan::Headers"). The static
# loader still performs runtime ICD discovery / dlopen of the GPU driver.
target_link_libraries(libslic3r_sla_gpu
    PUBLIC
        Vulkan::Headers
    PRIVATE
        vulkan
)

# The AGG delegate needs the same compile defs/includes libslic3r uses for AGG.
target_link_libraries(libslic3r_sla_gpu PRIVATE boost_libs)

set_property(TARGET libslic3r_sla_gpu PROPERTY FOLDER "libslic3r")

# --------------------------------------------------------------------------
# Phase 3 standalone smoke test (EXCLUDE_FROM_ALL, not part of ctest).
# Rasterizes a square-with-hole on the GPU and checks the mask. Build with:
#   cmake --build build --config Release --target sla_vulkan_smoke -j32
# Run headless (no DISPLAY needed). Only built when BUILD_TESTS is ON.
# --------------------------------------------------------------------------
if (BUILD_TESTS)
    add_executable(sla_vulkan_smoke EXCLUDE_FROM_ALL sla_vulkan_smoke.cpp)
    target_include_directories(sla_vulkan_smoke PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        $<TARGET_PROPERTY:libslic3r,INCLUDE_DIRECTORIES>
    )
    target_compile_definitions(sla_vulkan_smoke PRIVATE SLIC3R_VULKAN)
    # libslic3r provides ExPolygon/Point/PNG encoder; libslic3r_sla_gpu provides
    # VulkanRaster + the vendored static Vulkan loader.
    target_link_libraries(sla_vulkan_smoke PRIVATE
        libslic3r libslic3r_sla_gpu boost_libs)
    set_property(TARGET sla_vulkan_smoke PROPERTY FOLDER "libslic3r")

    # ----------------------------------------------------------------------
    # Phase 4 parity harness (EXCLUDE_FROM_ALL, not part of ctest). Slices real
    # meshes, rasterizes each layer with AGG and Vulkan, reports pixel parity +
    # ss sweep, gates on tolerance, and does a .sl1 round-trip + determinism
    # check. Build:
    #   cmake --build build --config Release --target sla_vulkan_parity -j32
    # Run headless (RTX 3090 / lavapipe):
    #   build/src/libslic3r/SLA/GPU/Release/sla_vulkan_parity
    # ----------------------------------------------------------------------
    add_executable(sla_vulkan_parity EXCLUDE_FROM_ALL
        sla_vulkan_parity.cpp
        # nanosvg implementation TU: libslic3r references nsvg* symbols
        # (svg.cpp / NSVGUtils.cpp) that are normally provided by the GUI lib;
        # the slicing path (load_drc / slice_mesh_ex) pulls them in here too.
        ${CMAKE_SOURCE_DIR}/tests/sla_print/sla_raster_bench_nanosvg.cpp)
    target_include_directories(sla_vulkan_parity PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        $<TARGET_PROPERTY:libslic3r,INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:nanosvg,INTERFACE_INCLUDE_DIRECTORIES>)
    target_compile_definitions(sla_vulkan_parity PRIVATE
        SLIC3R_VULKAN
        PARITY_MODELS_DIR=R"\(${CMAKE_SOURCE_DIR}/resources/handy_models\)")
    target_link_libraries(sla_vulkan_parity PRIVATE
        libslic3r libslic3r_sla_gpu boost_libs)
    set_property(TARGET sla_vulkan_parity PROPERTY FOLDER "libslic3r")
endif()
