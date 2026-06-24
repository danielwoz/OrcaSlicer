cmake_minimum_required(VERSION 3.13)
project(deps_src)

# Include all dependency subdirectories

# Header-only libraries (INTERFACE)
add_subdirectory(agg)
add_subdirectory(ankerl)
add_subdirectory(earcut)
add_subdirectory(fast_float)
add_subdirectory(nanosvg)
add_subdirectory(nlohmann)
add_subdirectory(stb_dxt)     # Header-only STB DXT compression library

# Static libraries
add_subdirectory(Shiny)
add_subdirectory(admesh)
add_subdirectory(clipper2)
add_subdirectory(expat)
add_subdirectory(glu-libtess)
add_subdirectory(hidapi)
add_subdirectory(hints)        # Hints library with utility executable
add_subdirectory(imgui)
add_subdirectory(imguizmo)
add_subdirectory(libigl)
add_subdirectory(libnest2d)
add_subdirectory(mcut)
add_subdirectory(md4c)
add_subdirectory(mdns)
add_subdirectory(miniz)
add_subdirectory(minilzo)
add_subdirectory(qhull)
add_subdirectory(qoi)
add_subdirectory(semver)      # Semver static library

if (SLIC3R_VULKAN)
    # Vulkan for the GPU seam visibility raycast (src/libslic3r/GCode/GPU). Vulkan-Headers is the
    # header-only target Vulkan::Headers. The loader is built as the static library "vulkan"; it still
    # finds the GPU driver at runtime. The danielwoz/Vulkan-Loader orca-vulkan-static branch adds the
    # static build, skips its find_package(VulkanHeaders) when the target exists and gates its tests on
    # VULKAN_LOADER_BUILD_TESTS instead of the BUILD_TESTS option OrcaSlicer uses.
    set(VULKAN_HEADERS_INSTALL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Vulkan-Headers" CACHE PATH "" FORCE)
    add_subdirectory(Vulkan-Headers EXCLUDE_FROM_ALL)
    set(BUILD_STATIC_LOADER       ON  CACHE BOOL "" FORCE)
    set(VULKAN_LOADER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(ENABLE_WERROR             OFF CACHE BOOL "" FORCE)
    set(UPDATE_DEPS               OFF CACHE BOOL "" FORCE)
    add_subdirectory(Vulkan-Loader EXCLUDE_FROM_ALL)
endif ()

if (ORCA_MIMALLOC)
    # Process-wide mimalloc: the shared library overrides malloc/free and operator new/delete,
    # and on Windows mimalloc-redirect.dll patches the CRT allocator of every module, so the
    # app and the separately built network plugin share one heap.
    set(MI_BUILD_SHARED ON  CACHE BOOL "" FORCE)
    set(MI_BUILD_STATIC OFF CACHE BOOL "" FORCE)
    set(MI_BUILD_OBJECT OFF CACHE BOOL "" FORCE)
    set(MI_BUILD_TESTS  OFF CACHE BOOL "" FORCE)
    set(MI_OVERRIDE     ON  CACHE BOOL "" FORCE)
    set(MI_WIN_REDIRECT ON  CACHE BOOL "" FORCE)
    set(MI_INSTALL_TOPLEVEL OFF CACHE BOOL "" FORCE)
    add_subdirectory(mimalloc EXCLUDE_FROM_ALL)
endif ()
