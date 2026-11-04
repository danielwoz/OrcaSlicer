get_filename_component(_TEST_NAME ${CMAKE_CURRENT_LIST_DIR} NAME)

# corpus.cpp holds the filler calls the suites of the built-in patterns share.
add_executable(${_TEST_NAME}_tests
    ${_TEST_NAME}_tests.cpp
    corpus.cpp
    corpus.hpp
    test_builtin_library.cpp
    test_builtin_mirror.cpp
    test_golden.cpp
    test_parallel_for.cpp
    )
# The folder the built-in pattern libraries are copied to for the tests (src/infill_patterns/CMakeLists.txt).
get_property(_library_dir GLOBAL PROPERTY ORCA_INFILL_PATTERN_TEST_DIR)
file(GENERATE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/infill_pattern_libraries.h
     CONTENT "#pragma once\n#define ORCA_TEST_INFILL_PATTERN_DIR \"${_library_dir}\"\n#define ORCA_TEST_INFILL_PATTERN_SUFFIX \"${CMAKE_SHARED_MODULE_SUFFIX}\"\n")
target_include_directories(${_TEST_NAME}_tests PRIVATE ${CMAKE_SOURCE_DIR}/sdk/infill/include ${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>)
orca_use_infill_build_id(${_TEST_NAME}_tests)
if (ORCA_INFILL_PATTERN_LINKAGE STREQUAL "STATIC")
    target_link_libraries(${_TEST_NAME}_tests orca_infill_patterns)
else ()
    target_link_libraries(${_TEST_NAME}_tests ${CMAKE_DL_LIBS})
endif ()
target_link_libraries(${_TEST_NAME}_tests test_common libslic3r Catch2::Catch2WithMain)
add_dependencies(${_TEST_NAME}_tests orca_infill_patterns)
set_property(TARGET ${_TEST_NAME}_tests PROPERTY FOLDER "tests")
orcaslicer_copy_test_dlls()
orcaslicer_discover_tests(${_TEST_NAME}_tests)

# The libraries copied for the tests hold exactly the files of the manifest, export only their entry, import only the
# C and C++ runtimes and carry none of the code a library must not contain. The manifest is written into the tests
# folder, the only part of the build CI hands to its test jobs. Those run without the developer environment of Visual
# Studio, so dumpbin is looked for when configuring; the audit is skipped when a tool it needs is missing.
if (ORCA_INFILL_PATTERN_LINKAGE STREQUAL "MODULE")
    find_package(Python3 COMPONENTS Interpreter)
    if (Python3_Interpreter_FOUND)
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
