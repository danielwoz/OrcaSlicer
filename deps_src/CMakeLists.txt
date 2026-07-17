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
add_subdirectory(clipper)
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
    # Optional vendored static Vulkan GPU SLA rasterizer dependencies. Only added
    # when SLIC3R_VULKAN is ON; the default OFF build is byte-for-byte unaffected.
    #
    #  - Vulkan-Headers : header-only INTERFACE target "Vulkan::Headers".
    #  - Vulkan-Loader  : built STATIC ("libvulkan.a", target "vulkan"). The static
    #                     loader still does runtime ICD discovery (dlopen's the
    #                     driver) — static linking removes the loader dependency,
    #                     not the driver, by design.
    #  - glslang        : SPIR-V shader compiler (used in Phase 3 to embed shaders).

    # Vulkan-Headers: defines target Vulkan::Headers. Also expose its install dir
    # so the loader's find_package(VulkanHeaders) / registry lookups resolve.
    set(VULKAN_HEADERS_INSTALL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Vulkan-Headers" CACHE PATH "" FORCE)
    add_subdirectory(Vulkan-Headers EXCLUDE_FROM_ALL)

    # Vulkan-Loader: force a STATIC archive (see deps_src/Vulkan-Loader patch that
    # adds the BUILD_STATIC_LOADER branch on non-Apple). Disable its tests; keep
    # ICD discovery intact.
    #
    # The loader does find_package(VulkanHeaders CONFIG); our add_subdirectory of
    # Vulkan-Headers already provides the Vulkan::Headers target, and there is no
    # build-tree config package. The loader CMake is patched to skip that
    # find_package when the target already exists (its only other use,
    # VulkanHeaders_VERSION, is behind LOADER_CODEGEN which is OFF).
    # NOTE: do NOT set the generic BUILD_TESTS / BUILD_TESTING / ENABLE_CTEST
    # cache vars here — they collide with OrcaSlicer's own global options. The
    # vendored loader is patched to gate its tests on VULKAN_LOADER_BUILD_TESTS
    # (default OFF) instead of BUILD_TESTS.
    set(BUILD_STATIC_LOADER       ON  CACHE BOOL "" FORCE)
    set(VULKAN_LOADER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(ENABLE_WERROR             OFF CACHE BOOL "" FORCE)
    set(UPDATE_DEPS               OFF CACHE BOOL "" FORCE)
    add_subdirectory(Vulkan-Loader EXCLUDE_FROM_ALL)

    # glslang: vendor the shader compiler. Phase 2 only needs it to CONFIGURE and
    # build its libs; shader invocation is deferred to Phase 3. Disable the SPIR-V
    # optimizer (needs SPIRV-Tools, not vendored) and the test/external machinery.
    # glslang-specific options only (avoid clobbering OrcaSlicer's global
    # BUILD_TESTING / ENABLE_CTEST). GLSLANG_TESTS gates its own test tree.
    set(ENABLE_OPT                OFF CACHE BOOL "" FORCE)  # no SPIRV-Tools dep
    set(BUILD_EXTERNAL            OFF CACHE BOOL "" FORCE)
    set(GLSLANG_TESTS             OFF CACHE BOOL "" FORCE)
    set(GLSLANG_ENABLE_INSTALL    OFF CACHE BOOL "" FORCE)
    set(ENABLE_GLSLANG_BINARIES   OFF CACHE BOOL "" FORCE)  # not needed until Phase 3 build-time codegen
    set(ENABLE_SPVREMAPPER        OFF CACHE BOOL "" FORCE)
    add_subdirectory(glslang EXCLUDE_FROM_ALL)
endif ()

if (ORCA_MIMALLOC)
    # Whole-process mimalloc via the SHARED override + Windows redirect DLL, so
    # BambuStudio.dll AND the network plugin (a separate /MD-CRT DLL) share ONE
    # mimalloc heap. That makes std::string-by-value across the plugin ABI safe
    # -- the reason static mimalloc had to be disabled when a plugin is bundled.
    # mimalloc-redirect.dll patches the shared UCRT malloc/free for every module
    # at load time; it is copied next to mimalloc.dll by mimalloc's own CMake
    # (MI_WIN_REDIRECT). We ship both DLLs (see src/CMakeLists.txt) and force an
    # early load via mi_version() in OrcaSlicer.cpp.
    set(MI_BUILD_SHARED ON  CACHE BOOL "" FORCE)
    set(MI_BUILD_STATIC OFF CACHE BOOL "" FORCE)
    set(MI_BUILD_OBJECT OFF CACHE BOOL "" FORCE)
    set(MI_BUILD_TESTS  OFF CACHE BOOL "" FORCE)
    # ON: the shared lib defines the malloc/new/delete entry points; combined
    # with MI_WIN_REDIRECT this redirects the whole process. Do NOT also include
    # mimalloc-new-delete.h in the host (that is a static-link mechanism and
    # would give the host a second, separate set of operators).
    set(MI_OVERRIDE     ON  CACHE BOOL "" FORCE)
    set(MI_WIN_REDIRECT ON  CACHE BOOL "" FORCE)
    set(MI_INSTALL_TOPLEVEL OFF CACHE BOOL "" FORCE)
    add_subdirectory(mimalloc EXCLUDE_FROM_ALL)
endif ()
