get_filename_component(_TEST_NAME ${CMAKE_CURRENT_LIST_DIR} NAME)
add_executable(${_TEST_NAME}_tests ${_TEST_NAME}_tests_main.cpp
    sla_print_tests.cpp
    sla_test_utils.hpp sla_test_utils.cpp
    sla_supptgen_tests.cpp
    sla_raycast_tests.cpp)
target_link_libraries(${_TEST_NAME}_tests test_common libslic3r Catch2::Catch2WithMain)
set_property(TARGET ${_TEST_NAME}_tests PROPERTY FOLDER "tests")

# GPU SLA rasterizer: pure-decision policy tests always compile (no GPU
# needed); the parity gate additionally needs libslic3r_sla_gpu (self-skips at
# runtime when no usable GPU is present). Both only exist when SLIC3R_VULKAN.
if (SLIC3R_VULKAN)
    target_sources(${_TEST_NAME}_tests PRIVATE
        sla_backend_policy_tests.cpp
        sla_vulkan_parity_tests.cpp)
    target_link_libraries(${_TEST_NAME}_tests libslic3r_sla_gpu)
endif()

orcaslicer_copy_test_dlls()

orcaslicer_discover_tests(${_TEST_NAME}_tests)

# --------------------------------------------------------------------------
# Standalone benchmarks (EXCLUDE_FROM_ALL, not part of ctest). AGG baseline
# always available; the GPU-vs-AGG comparison needs SLIC3R_VULKAN.
#   cmake --build build --config Release --target sla_raster_bench -j32
#   cmake --build build --config Release --target sla_gpu_bench -j32   # needs SLIC3R_VULKAN=ON
# --------------------------------------------------------------------------
add_executable(sla_raster_bench EXCLUDE_FROM_ALL sla_raster_bench.cpp sla_raster_bench_nanosvg.cpp)
target_include_directories(sla_raster_bench PRIVATE
    $<TARGET_PROPERTY:nanosvg,INTERFACE_INCLUDE_DIRECTORIES>)
target_link_libraries(sla_raster_bench PRIVATE libslic3r boost_libs)
set_property(TARGET sla_raster_bench PROPERTY FOLDER "tests")

if (SLIC3R_VULKAN)
    add_executable(sla_gpu_bench EXCLUDE_FROM_ALL sla_gpu_bench.cpp sla_raster_bench_nanosvg.cpp)
    target_include_directories(sla_gpu_bench PRIVATE
        $<TARGET_PROPERTY:nanosvg,INTERFACE_INCLUDE_DIRECTORIES>)
    target_compile_definitions(sla_gpu_bench PRIVATE SLIC3R_VULKAN)
    target_link_libraries(sla_gpu_bench PRIVATE libslic3r libslic3r_sla_gpu boost_libs)
    set_property(TARGET sla_gpu_bench PROPERTY FOLDER "tests")
endif()
