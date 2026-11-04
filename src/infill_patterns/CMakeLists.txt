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
if (_flags MATCHES "-fsanitize")
    set_property(GLOBAL PROPERTY ORCA_INFILL_SANITIZED ON)
endif ()
set_property(GLOBAL PROPERTY ORCA_INFILL_RUNTIME_DIR "${ORCA_INFILL_RUNTIME_DIR}")
# A library carries the debug information of all the shared sources it links, also for the code --gc-sections drops;
# compressed, it takes about a fifth of the space.
if (NOT MSVC AND NOT APPLE AND CMAKE_VERSION VERSION_GREATER_EQUAL 3.18)
    include(CheckLinkerFlag)
    check_linker_flag(CXX "LINKER:--compress-debug-sections=zlib" ORCA_INFILL_COMPRESS_DEBUG_SECTIONS)
endif ()

# orca_add_infill_pattern_module(<target> SOURCES <source>... LIBRARIES <library>... EXPORTS_MAP <map> EXPORTS_LIST <list>)
# Adds a module linked as a pattern library is: with the module seams (runtime/ModuleSeams.cpp) and libslic3r_fillkit,
# exporting only the symbols of <map> (GNU ld and lld) or <list> (ld64); on Windows only what is __declspec(dllexport).
function(orca_add_infill_pattern_module target)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "EXPORTS_MAP;EXPORTS_LIST" "SOURCES;LIBRARIES")
    get_property(_runtime_dir GLOBAL PROPERTY ORCA_INFILL_RUNTIME_DIR)
    get_property(_sanitized GLOBAL PROPERTY ORCA_INFILL_SANITIZED)
    add_library(${target} MODULE ${ARG_SOURCES})
    set_target_properties(${target} PROPERTIES
        PREFIX ""
        INTERPROCEDURAL_OPTIMIZATION OFF
        # A library needs only the system's C and C++ runtimes; an empty rpath entry would search the working directory.
        SKIP_BUILD_RPATH ON
        INSTALL_RPATH "")
    if (APPLE)
        target_link_options(${target} PRIVATE "LINKER:-exported_symbols_list,${ARG_EXPORTS_LIST}" "LINKER:-dead_strip"
                                              "LINKER:-dead_strip_dylibs")
        set_target_properties(${target} PROPERTIES LINK_DEPENDS "${ARG_EXPORTS_LIST}")
    elseif (MSVC)
        # A PDB in every configuration, as for the application; /DEBUG would turn REF and ICF off unless they are named.
        # The map lists the symbols of the library for scripts/check_infill_pattern_libs.py.
        target_link_options(${target} PRIVATE "$<$<CONFIG:Release>:/DEBUG>" "/OPT:REF" "/OPT:ICF"
                                              "/MAP:$<TARGET_FILE_DIR:${target}>/${target}.map")
    else ()
        target_link_options(${target} PRIVATE "LINKER:--version-script=${ARG_EXPORTS_MAP}" "LINKER:--exclude-libs,ALL"
                                              "LINKER:--as-needed" "LINKER:--gc-sections")
        if (NOT _sanitized)
            target_link_options(${target} PRIVATE "LINKER:--no-undefined")
        endif ()
        if (ORCA_INFILL_COMPRESS_DEBUG_SECTIONS)
            target_link_options(${target} PRIVATE "LINKER:--compress-debug-sections=zlib")
        endif ()
        set_target_properties(${target} PROPERTIES LINK_DEPENDS "${ARG_EXPORTS_MAP}")
    endif ()
    target_compile_definitions(${target} PRIVATE ORCA_INFILL_PATTERN_MODULE=1)
    target_include_directories(${target} PRIVATE ${_runtime_dir})
    target_link_libraries(${target} PRIVATE ${ARG_LIBRARIES} orca_infill_module_rt libslic3r_fillkit libslic3r_compile_settings)
    orca_use_infill_build_id(${target})
endfunction()

# Definitions a pattern library has in place of libslic3r's (runtime/ModuleSeams.cpp). Its objects are linked as
# objects, never as archive members, so that they come before any library that defines the same symbols.
if (ORCA_INFILL_PATTERN_LINKAGE STREQUAL "MODULE")
    add_library(orca_infill_module_rt OBJECT runtime/ModuleSeams.cpp runtime/PatternEntry.hpp)
    target_link_libraries(orca_infill_module_rt PRIVATE libslic3r_compile_settings)
    target_compile_definitions(orca_infill_module_rt PRIVATE ORCA_INFILL_PATTERN_MODULE=1)
    encoding_check(orca_infill_module_rt)
endif ()

# The entry of every library once more, under a name of its own and linked into an executable, with the table the
# loader binds them from (runtime/LinkedLibraries.cpp). With ORCA_INFILL_PATTERN_LINKAGE=STATIC libslic3r links it in
# place of the files; otherwise orca_link_builtin_infill_patterns() links it into the tools that ship as a single file.
add_library(orca_infill_patterns_linked OBJECT runtime/LinkedLibraries.cpp runtime/PatternEntry.hpp)
target_link_libraries(orca_infill_patterns_linked PRIVATE libslic3r_compile_settings)
target_compile_definitions(orca_infill_patterns_linked PRIVATE ORCA_INFILL_PATTERN_MODULE=0)
target_include_directories(orca_infill_patterns_linked PRIVATE ${ORCA_INFILL_RUNTIME_DIR})
orca_use_infill_build_id(orca_infill_patterns_linked)
encoding_check(orca_infill_patterns_linked)

# The rows of the built-in pattern table (Fill/InfillPatternRegistry.cpp) in InfillPattern order, each named by its
# token in upper case without punctuation. orca_infill_libraries.h defines ORCA_INFILL_LIBRARY_<ROW> for every one.
set(ORCA_INFILL_ROWS MONOTONIC MONOTONICLINE RECTILINEAR ALIGNEDRECTILINEAR ZIGZAG CROSSZAG LOCKEDZAG LINE GRID TRIANGLES
                     TRIHEXAGON CUBIC ADAPTIVECUBIC QUARTERCUBIC SUPPORTCUBIC LIGHTNING HONEYCOMB 3DHONEYCOMB LATERALHONEYCOMB
                     LATERALLATTICE CROSSHATCH TPMSD TPMSFK GYROID CONCENTRIC SPIRALINSET HILBERTCURVE ARCHIMEDEANCHORDS
                     OCTAGRAMSPIRAL SUPPORTBASE CONCENTRICINTERNAL)

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
    if (NOT _define IN_LIST ORCA_INFILL_ROWS)
        message(FATAL_ERROR "orca_add_builtin_infill_library(${token}): ${_define} is not in ORCA_INFILL_ROWS")
    endif ()
    get_property(_library_rows GLOBAL PROPERTY ORCA_INFILL_PATTERN_LIBRARY_ROWS)
    if (_define IN_LIST _library_rows)
        message(FATAL_ERROR "orca_add_builtin_infill_library(${token}): ${_define} has a library already")
    endif ()

    # The entry linked into executables, under a name of its own.
    set(_linked_entry "orca_infill_builtin_entry_v1_${_id}")
    set(ORCA_INFILL_ENTRY "${_linked_entry}")
    set(ORCA_INFILL_ENTRY_EXPORT "")
    set(_entry "${CMAKE_CURRENT_BINARY_DIR}/${_name}/entry_linked.cpp")
    configure_file(${ORCA_INFILL_RUNTIME_DIR}/entry.cpp.in "${_entry}" @ONLY)
    target_sources(orca_infill_patterns_linked PRIVATE "${_entry}")

    if (ORCA_INFILL_PATTERN_LINKAGE STREQUAL "MODULE")
        set(ORCA_INFILL_ENTRY "orca_infill_builtin_entry_v1")
        set(ORCA_INFILL_ENTRY_EXPORT "ORCA_INFILL_EXPORT")
        set(_entry "${CMAKE_CURRENT_BINARY_DIR}/${_name}/entry.cpp")
        configure_file(${ORCA_INFILL_RUNTIME_DIR}/entry.cpp.in "${_entry}" @ONLY)
        orca_add_infill_pattern_module(${_name} SOURCES "${_entry}" LIBRARIES ${ARG_OBJECTS}
                                       EXPORTS_MAP ${ORCA_INFILL_RUNTIME_DIR}/exports.map EXPORTS_LIST ${ORCA_INFILL_RUNTIME_DIR}/exports.txt)
        set_target_properties(${_name} PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/infill_patterns/$<CONFIG>"
            # Multi-configuration generators add the configuration, as for the application's own PDB.
            PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/src")
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
        set(_file "")
    endif ()

    set_property(GLOBAL APPEND PROPERTY ORCA_INFILL_PATTERN_LIBRARY_ROWS ${_define})
    set_property(GLOBAL PROPERTY ORCA_INFILL_PATTERN_MANIFEST_${_define}
                 "    X(${ARG_ROW}, ${ORCA_INFILL_KEY}, \"${_name}\", \"${_file}\", ${_linked_entry}) \\\n")
    set_property(GLOBAL PROPERTY ORCA_INFILL_PATTERN_FILE_${_define} "${_file}")
endfunction()

# Every family, in a fixed order, each adding its rows.
foreach (_family IN ITEMS rectilinear line concentric honeycomb crosshatch planepath tpms adaptive lightning)
    include(${CMAKE_CURRENT_SOURCE_DIR}/${_family}.cmake)
endforeach ()

# The manifest lines and file names of the libraries in InfillPattern order, whatever the order of the families.
get_property(_library_rows GLOBAL PROPERTY ORCA_INFILL_PATTERN_LIBRARY_ROWS)
set(_manifest "")
set(_files "")
foreach (_row IN LISTS ORCA_INFILL_ROWS)
    if (_row IN_LIST _library_rows)
        get_property(_line GLOBAL PROPERTY ORCA_INFILL_PATTERN_MANIFEST_${_row})
        get_property(_file GLOBAL PROPERTY ORCA_INFILL_PATTERN_FILE_${_row})
        string(APPEND _manifest "${_line}")
        if (_file)
            list(APPEND _files "${_file}")
        endif ()
    endif ()
endforeach ()
set_property(GLOBAL PROPERTY ORCA_INFILL_PATTERN_FILES "${_files}")

# Builds the pattern libraries; with ORCA_INFILL_PATTERN_LINKAGE=STATIC there are none, and libslic3r links the entries.
get_property(_targets GLOBAL PROPERTY ORCA_INFILL_PATTERN_TARGETS)
add_custom_target(orca_infill_patterns)
if (_targets)
    add_dependencies(orca_infill_patterns ${_targets})
endif ()
if (ORCA_INFILL_PATTERN_LINKAGE STREQUAL "STATIC")
    target_link_libraries(libslic3r PRIVATE orca_infill_patterns_linked)
endif ()

# Links the entries of the pattern libraries into target, an executable that ships without the library files and binds
# them with bind_builtin_infill_patterns(linked_builtin_infill_patterns()) (Fill/InfillPatternLibraries.hpp).
# The objects are added as sources, so that the target may use either signature of target_link_libraries().
function(orca_link_builtin_infill_patterns target)
    if (ORCA_INFILL_PATTERN_LINKAGE STREQUAL "MODULE")
        target_sources(${target} PRIVATE $<TARGET_OBJECTS:orca_infill_patterns_linked>)
    endif ()
endfunction()

# Writes path only when its content changes, so that its includers rebuild only then.
function(orca_infill_write_if_changed path content)
    file(WRITE "${path}.tmp" "${content}")
    configure_file("${path}.tmp" "${path}" COPYONLY)
    file(REMOVE "${path}.tmp")
endfunction()

# orca_infill_libraries.h, which libslic3r_infill_patterns.h includes: ORCA_INFILL_LIBRARY_<ROW> is 1 for the rows a
# pattern library serves and 0 for the others.
set(_defines "")
foreach (_row IN LISTS ORCA_INFILL_ROWS)
    if (_row IN_LIST _library_rows)
        string(APPEND _defines "#define ORCA_INFILL_LIBRARY_${_row} 1\n")
    else ()
        string(APPEND _defines "#define ORCA_INFILL_LIBRARY_${_row} 0\n")
    endif ()
endforeach ()
if (ORCA_INFILL_PATTERN_LINKAGE STREQUAL "STATIC")
    set(_static 1)
else ()
    set(_static 0)
endif ()
orca_infill_write_if_changed("${CMAKE_BINARY_DIR}/src/libslic3r/orca_infill_libraries.h"
"#pragma once
// Generated by src/infill_patterns/CMakeLists.txt: ORCA_INFILL_LIBRARY_<ROW> is 1 for every row of the built-in
// pattern table that a pattern library serves, and 0 for the others.
#define ORCA_INFILL_PATTERN_LINKAGE_STATIC ${_static}
${_defines}")

# orca_infill_manifest.h: X(row, token, name, file, linked) for every pattern library, with the InfillPattern enumerator
# of its row, its token (nullptr for an internal row), its name, its file name (empty when libslic3r links the entries)
# and the entry of its copy in orca_infill_patterns_linked, in InfillPattern order.
orca_infill_write_if_changed("${CMAKE_BINARY_DIR}/src/libslic3r/orca_infill_manifest.h"
"#pragma once
// Generated by src/infill_patterns/CMakeLists.txt: the built-in infill pattern libraries of this build.
#define ORCA_INFILL_MANIFEST(X) \\
${_manifest}
")

# The file names of the libraries, one per line, for scripts/check_infill_pattern_libs.py: the packaging of the
# application checks its folder of libraries against this one (src/dev-utils/platform/unix/build_linux_image.sh.in).
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
