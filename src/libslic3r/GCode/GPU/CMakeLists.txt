# GPU seam visibility raycast (SLIC3R_VULKAN), linked PRIVATE into libslic3r. When the option is OFF this
# directory is not added; SeamRayCaster.hpp then declares an inert placeholder.
#
# Uses VK_KHR_ray_query and VK_KHR_acceleration_structure to cast the seam visibility rays of
# SeamPlacer. The compute shader seam_raycast.comp is embedded as SPIR-V in seam_raycast_spv.hpp, so the
# build needs no shader compiler. After changing the shader, compile it for Vulkan 1.2 / SPIR-V 1.4
# (e.g. glslangValidator --target-env vulkan1.2 -o seam_raycast.spv seam_raycast.comp) and replace the
# words of seam_raycast_spv and its seam_raycast_spv_size.

add_library(libslic3r_seam_gpu STATIC
    SeamRayCaster.hpp
    SeamRayCaster.cpp
    seam_raycast_spv.hpp
)

# The include directories libslic3r resolved (admesh, eigen, boost, ...).
target_include_directories(libslic3r_seam_gpu PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    $<TARGET_PROPERTY:libslic3r,INCLUDE_DIRECTORIES>
)

target_compile_definitions(libslic3r_seam_gpu PUBLIC SLIC3R_VULKAN)

# The static Vulkan loader (target "vulkan") and the header-only Vulkan-Headers.
target_link_libraries(libslic3r_seam_gpu
    PUBLIC
        Vulkan::Headers
    PRIVATE
        vulkan
        boost_libs
)

set_property(TARGET libslic3r_seam_gpu PROPERTY FOLDER "libslic3r")
