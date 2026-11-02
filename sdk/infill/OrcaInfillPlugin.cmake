# Builds an OrcaSlicer infill plugin: a module that exports only its entry point and links nothing of OrcaSlicer.
#   include(<OrcaSlicer>/sdk/infill/OrcaInfillPlugin.cmake)
#   orca_add_infill_plugin(my_pattern my_pattern.cpp)
set(ORCA_INFILL_SDK_DIR ${CMAKE_CURRENT_LIST_DIR})

function(orca_add_infill_plugin target)
    add_library(${target} MODULE ${ARGN})
    target_include_directories(${target} PRIVATE ${ORCA_INFILL_SDK_DIR}/include)
    target_compile_features(${target} PRIVATE cxx_std_17)
    set_target_properties(${target} PROPERTIES PREFIX "" C_VISIBILITY_PRESET hidden CXX_VISIBILITY_PRESET hidden
                                               VISIBILITY_INLINES_HIDDEN ON)
    # The C++ runtime is linked in, so a plugin does not depend on the version OrcaSlicer ships. Nothing allocated
    # by one side is freed by the other, so each may have its own. Takes effect with policy CMP0091 set to NEW.
    if (MSVC)
        set_target_properties(${target} PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif ()
    # A plugin must not reach for symbols of the application. A sanitizer's runtime lives in the executable, so the
    # check is left out when one is on.
    get_directory_property(_options COMPILE_OPTIONS)
    get_directory_property(_link_options LINK_OPTIONS)
    set(_flags "${CMAKE_C_FLAGS} ${CMAKE_CXX_FLAGS} ${CMAKE_MODULE_LINKER_FLAGS} ${_options} ${_link_options}")
    if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT _flags MATCHES "-fsanitize")
        target_link_options(${target} PRIVATE "LINKER:--no-undefined")
    endif ()
endfunction()
