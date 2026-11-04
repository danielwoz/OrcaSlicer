get_filename_component(_TEST_NAME ${CMAKE_CURRENT_LIST_DIR} NAME)

# corpus.cpp holds the filler calls the suites of the built-in patterns share; native_fillers.cpp creates the Fill class
# of a row itself, the reference of the pattern libraries.
add_executable(${_TEST_NAME}_tests
    ${_TEST_NAME}_tests.cpp
    corpus.cpp
    corpus.hpp
    native_fillers.cpp
    native_fillers.hpp
    test_builtin_binding.cpp
    test_builtin_equivalence.cpp
    test_builtin_mirror.cpp
    test_golden.cpp
    test_parallel_for.cpp
    )
target_include_directories(${_TEST_NAME}_tests PRIVATE ${CMAKE_SOURCE_DIR}/sdk/infill/include ${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>)
orca_use_infill_build_id(${_TEST_NAME}_tests)
# The entries of the libraries linked into the executable, as ORCA_INFILL_PATTERN_LINKAGE=STATIC links them.
orca_link_builtin_infill_patterns(${_TEST_NAME}_tests)
target_link_libraries(${_TEST_NAME}_tests test_common libslic3r Catch2::Catch2WithMain)

if (ORCA_INFILL_PATTERN_LINKAGE STREQUAL "MODULE")
    # Pattern libraries that each break one rule of the loader (fixtures/fixture_library.cpp), linked as the libraries of
    # src/infill_patterns are; each exports the symbols its rule names.
    set(_fixture_defines "")
    set(_fixture_targets "")
    foreach (_fixture BAD_BUILD_ID WRONG_ROW WRONG_TOKEN CALLS_MISMATCH TRAIT_MISMATCH NO_ENTRY ALSO_PLUGIN_ENTRY THROWS_IN_RUN)
        string(TOLOWER ${_fixture} _name)
        set(_target infill_pattern_fixture_${_name})
        if (_fixture STREQUAL "NO_ENTRY")
            set(_exports orca_infill_fixture_entry)
        elseif (_fixture STREQUAL "ALSO_PLUGIN_ENTRY")
            set(_exports orca_infill_builtin_entry_v1 orca_infill_plugin_entry_v1)
        else ()
            set(_exports orca_infill_builtin_entry_v1)
        endif ()
        set(_dir ${CMAKE_CURRENT_BINARY_DIR}/fixtures/${_name})
        string(REPLACE ";" "; " _map "${_exports}")
        file(WRITE ${_dir}/exports.map "{\n    global: ${_map};\n    local: *;\n};\n")
        list(TRANSFORM _exports PREPEND "_" OUTPUT_VARIABLE _list)
        string(REPLACE ";" "\n" _list "${_list}")
        file(WRITE ${_dir}/exports.txt "${_list}\n")
        orca_add_infill_pattern_module(${_target} SOURCES fixtures/fixture_library.cpp EXPORTS_MAP ${_dir}/exports.map
                                       EXPORTS_LIST ${_dir}/exports.txt)
        target_compile_definitions(${_target} PRIVATE FIXTURE_${_fixture})
        if (NOT APPLE AND NOT MSVC)
            # Only their entry is of interest; without the debug information of libslic3r_fillkit they stay small.
            target_link_options(${_target} PRIVATE "LINKER:--strip-debug")
        endif ()
        set_target_properties(${_target} PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/fixtures/$<CONFIG>
                                                    FOLDER "tests")
        list(APPEND _fixture_targets ${_target})
        string(APPEND _fixture_defines "#define INFILL_PATTERN_FIXTURE_${_fixture} \"$<TARGET_FILE:${_target}>\"\n")
    endforeach ()
    file(GENERATE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/infill_pattern_fixtures.h CONTENT "#pragma once\n${_fixture_defines}")
    add_dependencies(${_TEST_NAME}_tests ${_fixture_targets})
endif ()

set_property(TARGET ${_TEST_NAME}_tests PROPERTY FOLDER "tests")
orcaslicer_copy_test_dlls()
orcaslicer_discover_tests(${_TEST_NAME}_tests)

# Slicing without the libraries: an executable of its own, which loads them from an empty folder before its test.
if (ORCA_INFILL_PATTERN_LINKAGE STREQUAL "MODULE")
    add_executable(${_TEST_NAME}_unbound_tests ${_TEST_NAME}_tests.cpp test_builtin_unbound.cpp)
    target_compile_definitions(${_TEST_NAME}_unbound_tests PRIVATE ORCA_TEST_INFILL_PATTERNS_UNLOADED)
    target_link_libraries(${_TEST_NAME}_unbound_tests test_common libslic3r Catch2::Catch2WithMain)
    set_property(TARGET ${_TEST_NAME}_unbound_tests PROPERTY FOLDER "tests")
    orcaslicer_copy_test_dlls(${_TEST_NAME}_unbound_tests)
    orcaslicer_discover_tests(${_TEST_NAME}_unbound_tests)
endif ()

# The libraries copied for the tests hold exactly the files of the manifest, export only their entry, import only the
# C and C++ runtimes and carry none of the code a library must not contain. The manifest is written into the tests
# folder, the only part of the build CI hands to its test jobs. Those run without the developer environment of Visual
# Studio, so dumpbin is looked for when configuring; the audit is skipped when a tool it needs is missing.
if (ORCA_INFILL_PATTERN_LINKAGE STREQUAL "MODULE")
    find_package(Python3 COMPONENTS Interpreter)
    if (Python3_Interpreter_FOUND)
        get_property(_library_dir GLOBAL PROPERTY ORCA_INFILL_PATTERN_TEST_DIR)
        orca_write_infill_pattern_manifest(${CMAKE_CURRENT_BINARY_DIR}/infill_pattern_manifest.txt)
        set(_audit_options "")
        if (WIN32)
            find_program(ORCA_INFILL_DUMPBIN dumpbin)
            if (ORCA_INFILL_DUMPBIN)
                set(_audit_options --dumpbin "${ORCA_INFILL_DUMPBIN}")
            endif ()
        endif ()
        add_test(NAME infill_pattern_libraries_audit
                 COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/scripts/check_infill_pattern_libs.py ${_audit_options}
                         --manifest ${CMAKE_CURRENT_BINARY_DIR}/infill_pattern_manifest.txt "${_library_dir}")
        set_tests_properties(infill_pattern_libraries_audit PROPERTIES LABELS packaging SKIP_RETURN_CODE 77)
    else ()
        message(WARNING "No Python 3 interpreter: the audit of the infill pattern libraries is not run")
    endif ()
endif ()
