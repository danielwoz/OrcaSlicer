# The libraries that carry the built-in infill patterns, one per row of the pattern table. A library links the pattern
# objects of its row and libslic3r_fillkit, the object files libslic3r links, so that it runs the row's Fill class
# exactly as libslic3r does, and exports only orca_infill_builtin_entry_v1 (src/libslic3r/Fill/Library/
# orca_infill_builtin.h). Each <family>.cmake adds the rows of one family with orca_add_builtin_infill_library().

set(ORCA_INFILL_PATTERN_LINKAGE "MODULE" CACHE STRING
    "How the built-in infill pattern libraries are linked: MODULE, a loadable library per pattern next to the application; STATIC, into the application")
set_property(CACHE ORCA_INFILL_PATTERN_LINKAGE PROPERTY STRINGS MODULE STATIC)
if (NOT ORCA_INFILL_PATTERN_LINKAGE MATCHES "^(MODULE|STATIC)$")
    message(FATAL_ERROR "ORCA_INFILL_PATTERN_LINKAGE is MODULE or STATIC, not '${ORCA_INFILL_PATTERN_LINKAGE}'")
endif ()

set(ORCA_INFILL_RUNTIME_DIR ${CMAKE_CURRENT_SOURCE_DIR}/runtime)

# The folders a built library is copied to: next to the application (src/CMakeLists.txt gives OrcaSlicer no output
# directory of its own), where the application looks for it, and next to the tests, which CI runs from the tests folder.
if (CMAKE_CONFIGURATION_TYPES)
    set(_config_dir "/$<CONFIG>")
else ()
    set(_config_dir "")
endif ()
if (APPLE AND CMAKE_MACOSX_BUNDLE)
    set(_app_dir "${CMAKE_BINARY_DIR}/src${_config_dir}/OrcaSlicer.app/Contents/PlugIns/infill_patterns")
elseif (APPLE)
    set(_app_dir "${CMAKE_BINARY_DIR}/src${_config_dir}/../PlugIns/infill_patterns")
else ()
    set(_app_dir "${CMAKE_BINARY_DIR}/src${_config_dir}/infill_patterns")
endif ()
set(_test_dir "${CMAKE_BINARY_DIR}/tests/infill_patterns${_config_dir}")
set_property(GLOBAL PROPERTY ORCA_INFILL_PATTERN_APP_DIR "${_app_dir}")
set_property(GLOBAL PROPERTY ORCA_INFILL_PATTERN_TEST_DIR "${_test_dir}")
set(ORCA_INFILL_PATTERN_STAGE_DIRS "${_app_dir}")
if (BUILD_TESTS)
    list(APPEND ORCA_INFILL_PATTERN_STAGE_DIRS "${_test_dir}")
endif ()

# The link of a pattern library: only the entry is exported, the sections its row does not reach are dropped, and every
# symbol must resolve inside the library. A sanitizer's runtime lives in the executable, so that check is left out when
# one is on, as for infill plugins (sdk/infill/OrcaInfillPlugin.cmake).
get_directory_property(_options COMPILE_OPTIONS)
set(_flags "${CMAKE_C_FLAGS} ${CMAKE_CXX_FLAGS} ${CMAKE_MODULE_LINKER_FLAGS} ${_options}")
if (APPLE)
    set(ORCA_INFILL_LINK_OPTIONS "LINKER:-exported_symbols_list,${ORCA_INFILL_RUNTIME_DIR}/exports.txt" "LINKER:-dead_strip"
                                 "LINKER:-dead_strip_dylibs")
    set(ORCA_INFILL_LINK_DEPENDS "${ORCA_INFILL_RUNTIME_DIR}/exports.txt")
elseif (MSVC)
    # A PDB in every configuration, as for the application; /DEBUG would turn REF and ICF off unless they are named.
    set(ORCA_INFILL_LINK_OPTIONS "$<$<CONFIG:Release>:/DEBUG>" "/OPT:REF" "/OPT:ICF")
    set(ORCA_INFILL_LINK_DEPENDS "")
else ()
    set(ORCA_INFILL_LINK_OPTIONS "LINKER:--version-script=${ORCA_INFILL_RUNTIME_DIR}/exports.map" "LINKER:--exclude-libs,ALL"
                                 "LINKER:--as-needed" "LINKER:--gc-sections")
    if (NOT _flags MATCHES "-fsanitize")
        list(APPEND ORCA_INFILL_LINK_OPTIONS "LINKER:--no-undefined")
    endif ()
    set(ORCA_INFILL_LINK_DEPENDS "${ORCA_INFILL_RUNTIME_DIR}/exports.map")
endif ()

# Definitions a pattern library has in place of libslic3r's (runtime/ModuleSeams.cpp). Its objects are linked as
# objects, never as archive members, so that they come before any library that defines the same symbols.
if (ORCA_INFILL_PATTERN_LINKAGE STREQUAL "MODULE")
    add_library(orca_infill_module_rt OBJECT runtime/ModuleSeams.cpp runtime/PatternEntry.hpp)
    target_link_libraries(orca_infill_module_rt PRIVATE libslic3r_compile_settings)
    target_compile_definitions(orca_infill_module_rt PRIVATE ORCA_INFILL_PATTERN_MODULE=1)
    encoding_check(orca_infill_module_rt)
else ()
    # The entries of every library, under names of their own, for the application to bind without files.
    add_library(orca_infill_patterns STATIC runtime/PatternEntry.hpp)
    target_link_libraries(orca_infill_patterns PRIVATE libslic3r_compile_settings)
    target_compile_definitions(orca_infill_patterns PRIVATE ORCA_INFILL_PATTERN_MODULE=0)
    target_include_directories(orca_infill_patterns PRIVATE ${ORCA_INFILL_RUNTIME_DIR})
    orca_use_infill_build_id(orca_infill_patterns)
endif ()

# orca_add_builtin_infill_library(<token> ROW <ipX> CLASS <FillX> HEADER <header> OBJECTS <objects>...
#                                 [CALLS <call>...] [STATE <kind>...] [NO_SORT] [INTERNAL])
# Adds the library orca_infill_<token> serving the row <ipX> of the pattern table with the Fill class <FillX>, declared
# in <header> and compiled into the pattern objects <objects>. CALLS names the ORCA_INFILL_CALL_* run() accepts
# (FILL_SURFACE when left out), STATE the ORCA_INFILL_STATE_* prepare_state() builds; NO_SORT sets
# ORCA_INFILL_TRAIT_NO_SORT, and INTERNAL marks a row that has no token in configurations.
function(orca_add_builtin_infill_library token)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "NO_SORT;INTERNAL" "ROW;CLASS;HEADER" "OBJECTS;CALLS;STATE")
    if (ARG_UNPARSED_ARGUMENTS OR NOT ARG_ROW OR NOT ARG_CLASS OR NOT ARG_HEADER OR NOT ARG_OBJECTS)
        message(FATAL_ERROR "orca_add_builtin_infill_library(${token}): ROW, CLASS, HEADER and OBJECTS are required; "
                            "unexpected: ${ARG_UNPARSED_ARGUMENTS}")
    endif ()
    if (NOT ARG_CALLS)
        set(ARG_CALLS FILL_SURFACE)
    endif ()
    set(_name orca_infill_${token})
    string(MAKE_C_IDENTIFIER "${token}" _id)
    string(TOUPPER "${token}" _define)
    string(REGEX REPLACE "[^A-Z0-9]" "" _define "${_define}")

    set(ORCA_INFILL_LIBRARY "${_name}")
    set(ORCA_INFILL_ROW "${ARG_ROW}")
    set(ORCA_INFILL_CLASS "${ARG_CLASS}")
    set(ORCA_INFILL_HEADER "${ARG_HEADER}")
    if (ARG_INTERNAL)
        set(ORCA_INFILL_KEY "nullptr")
    else ()
        set(ORCA_INFILL_KEY "\"${token}\"")
    endif ()
    set(_calls "")
    foreach (_call IN LISTS ARG_CALLS)
        list(APPEND _calls "(1u << ORCA_INFILL_CALL_${_call})")
    endforeach ()
    string(REPLACE ";" " | " ORCA_INFILL_CALLS "${_calls}")
    set(_kinds "0u")
    foreach (_kind IN LISTS ARG_STATE)
        list(APPEND _kinds "(1u << ORCA_INFILL_STATE_${_kind})")
    endforeach ()
    string(REPLACE ";" " | " ORCA_INFILL_STATE_KINDS "${_kinds}")
    if (ARG_NO_SORT)
        set(ORCA_INFILL_TRAITS "ORCA_INFILL_TRAIT_NO_SORT")
    else ()
        set(ORCA_INFILL_TRAITS "0u")
    endif ()
    if (ORCA_INFILL_PATTERN_LINKAGE STREQUAL "MODULE")
        set(ORCA_INFILL_ENTRY "orca_infill_builtin_entry_v1")
        set(ORCA_INFILL_ENTRY_EXPORT "ORCA_INFILL_EXPORT")
    else ()
        set(ORCA_INFILL_ENTRY "orca_infill_builtin_entry_v1_${_id}")
        set(ORCA_INFILL_ENTRY_EXPORT "")
    endif ()
    set(_entry "${CMAKE_CURRENT_BINARY_DIR}/${_name}/entry.cpp")
    configure_file(${ORCA_INFILL_RUNTIME_DIR}/entry.cpp.in "${_entry}" @ONLY)

    if (ORCA_INFILL_PATTERN_LINKAGE STREQUAL "MODULE")
        add_library(${_name} MODULE "${_entry}")
        set_target_properties(${_name} PROPERTIES
            PREFIX ""
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/infill_patterns/$<CONFIG>"
            # Multi-configuration generators add the configuration, as for the application's own PDB.
            PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/src"
            INTERPROCEDURAL_OPTIMIZATION OFF
            # A library needs only the system's C and C++ runtimes; an empty rpath entry would search the working
            # directory.
            SKIP_BUILD_RPATH ON
            INSTALL_RPATH ""
            LINK_DEPENDS "${ORCA_INFILL_LINK_DEPENDS}")
        target_compile_definitions(${_name} PRIVATE ORCA_INFILL_PATTERN_MODULE=1)
        target_include_directories(${_name} PRIVATE ${ORCA_INFILL_RUNTIME_DIR})
        target_link_libraries(${_name} PRIVATE ${ARG_OBJECTS} orca_infill_module_rt libslic3r_fillkit libslic3r_compile_settings)
        target_link_options(${_name} PRIVATE ${ORCA_INFILL_LINK_OPTIONS})
        if (MSVC)
            # The map lists the symbols of the library for scripts/check_infill_pattern_libs.py.
            target_link_options(${_name} PRIVATE "/MAP:$<TARGET_FILE_DIR:${_name}>/${_name}.map")
        endif ()
        orca_use_infill_build_id(${_name})
        foreach (_dir IN LISTS ORCA_INFILL_PATTERN_STAGE_DIRS)
            add_custom_command(TARGET ${_name} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "${_dir}"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:${_name}>" "${_dir}"
                VERBATIM)
        endforeach ()
        if (MSVC AND BUILD_TESTS)
            # The audit of the tests reads the symbols of a Windows library from its map.
            add_custom_command(TARGET ${_name} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE_DIR:${_name}>/${_name}.map" "${_test_dir}"
                VERBATIM)
        endif ()
        if (WIN32)
            install(TARGETS ${_name} LIBRARY DESTINATION infill_patterns)
        elseif (NOT APPLE)
            install(TARGETS ${_name} LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/orca-slicer/infill_patterns)
        endif ()
        set(_file "${_name}${CMAKE_SHARED_MODULE_SUFFIX}")
        set_property(GLOBAL APPEND PROPERTY ORCA_INFILL_PATTERN_TARGETS ${_name})
    else ()
        target_sources(orca_infill_patterns PRIVATE "${_entry}")
        set(_file "")
    endif ()

    set_property(GLOBAL APPEND PROPERTY ORCA_INFILL_PATTERN_DEFINES "#define ORCA_INFILL_LIBRARY_${_define} 1\n")
    set_property(GLOBAL APPEND PROPERTY ORCA_INFILL_PATTERN_MANIFEST
                 "    X(${ARG_ROW}, ${ORCA_INFILL_KEY}, \"${_file}\", ${ORCA_INFILL_ENTRY}) \\\n")
    set_property(GLOBAL APPEND PROPERTY ORCA_INFILL_PATTERN_FILES "${_file}")
endfunction()

# Every family, in a fixed order, each adding its rows.
foreach (_family IN ITEMS rectilinear line concentric honeycomb crosshatch planepath tpms adaptive lightning)
    include(${CMAKE_CURRENT_SOURCE_DIR}/${_family}.cmake)
endforeach ()

if (ORCA_INFILL_PATTERN_LINKAGE STREQUAL "MODULE")
    get_property(_targets GLOBAL PROPERTY ORCA_INFILL_PATTERN_TARGETS)
    add_custom_target(orca_infill_patterns)
    if (_targets)
        add_dependencies(orca_infill_patterns ${_targets})
    endif ()
endif ()

# Writes path only when its content changes, so that its includers rebuild only then.
function(orca_infill_write_if_changed path content)
    file(WRITE "${path}.tmp" "${content}")
    configure_file("${path}.tmp" "${path}" COPYONLY)
    file(REMOVE "${path}.tmp")
endfunction()

# orca_infill_libraries.h, which libslic3r_infill_patterns.h includes: the rows a pattern library serves.
get_property(_defines GLOBAL PROPERTY ORCA_INFILL_PATTERN_DEFINES)
string(REPLACE ";" "" _defines "${_defines}")
if (ORCA_INFILL_PATTERN_LINKAGE STREQUAL "STATIC")
    set(_static 1)
else ()
    set(_static 0)
endif ()
orca_infill_write_if_changed("${CMAKE_BINARY_DIR}/src/libslic3r/orca_infill_libraries.h"
"#pragma once
// Generated by src/infill_patterns/CMakeLists.txt: ORCA_INFILL_LIBRARY_<ROW> is defined for every row of the built-in
// pattern table that a pattern library serves.
#define ORCA_INFILL_PATTERN_LINKAGE_STATIC ${_static}
${_defines}")

# orca_infill_manifest.h: X(row, token, file, entry) for every pattern library, with the InfillPattern enumerator of its
# row, its token (nullptr for an internal row), its file name (empty when linked into the application) and its entry.
get_property(_manifest GLOBAL PROPERTY ORCA_INFILL_PATTERN_MANIFEST)
string(REPLACE ";" "" _manifest "${_manifest}")
orca_infill_write_if_changed("${CMAKE_BINARY_DIR}/src/libslic3r/orca_infill_manifest.h"
"#pragma once
// Generated by src/infill_patterns/CMakeLists.txt: the built-in infill pattern libraries of this build.
#define ORCA_INFILL_MANIFEST(X) \\
${_manifest}
")

# The file names of the libraries, one per line, for scripts/check_infill_pattern_libs.py.
function(orca_write_infill_pattern_manifest path)
    get_property(_files GLOBAL PROPERTY ORCA_INFILL_PATTERN_FILES)
    string(REPLACE ";" "\n" _files "${_files}")
    orca_infill_write_if_changed("${path}" "${_files}\n")
endfunction()
orca_write_infill_pattern_manifest("${CMAKE_BINARY_DIR}/infill_patterns/manifest.txt")

# Called on the application's target: builds the libraries first, and copies them next to it again after every link,
# so that a bundle that is rebuilt is filled again.
function(orca_stage_builtin_infill_libraries target)
    if (NOT ORCA_INFILL_PATTERN_LINKAGE STREQUAL "MODULE")
        return()
    endif ()
    add_dependencies(${target} orca_infill_patterns)
    get_property(_targets GLOBAL PROPERTY ORCA_INFILL_PATTERN_TARGETS)
    get_property(_dir GLOBAL PROPERTY ORCA_INFILL_PATTERN_APP_DIR)
    set(_files "")
    foreach (_library IN LISTS _targets)
        list(APPEND _files "$<TARGET_FILE:${_library}>")
    endforeach ()
    if (_files)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${_dir}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_files} "${_dir}"
            COMMENT "Copying the infill pattern libraries next to ${target}"
            VERBATIM)
    endif ()
endfunction()
