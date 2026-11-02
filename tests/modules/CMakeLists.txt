get_filename_component(_TEST_NAME ${CMAKE_CURRENT_LIST_DIR} NAME)
include(${CMAKE_SOURCE_DIR}/sdk/modules/OrcaModule.cmake)

# The sample modules, alone in their directory so that a directory scan finds only them.
orca_add_module(sample_lines_module ${CMAKE_SOURCE_DIR}/sdk/infill/samples/lines/lines.cpp)
target_compile_definitions(sample_lines_module PRIVATE ORCA_SAMPLE_LINES_MODULE)
orca_add_module(infill_walls ${CMAKE_SOURCE_DIR}/sdk/modules/samples/infill_walls/infill_walls.cpp)
orca_add_module(change_at_z ${CMAKE_SOURCE_DIR}/sdk/modules/samples/change_at_z/change_at_z.cpp)
set(_sample_targets sample_lines_module infill_walls change_at_z)
set_target_properties(${_sample_targets} PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/modules)

# Broken, unusual and test-only modules, built from one C++ source, and one module written in C.
set(_fixture_defines "")
set(_fixture_targets "")
foreach (_fixture OPTIONS WRONG_MAJOR BAD_KEY DUPLICATE_KEY DUPLICATE_OPTION RESERVED_SCOPE NO_STEPS BAD_CONDITION
                  FOREIGN_CONDITION PARAM_COLLIDES FILL_FAILS FILL_KINDS GCODE_THROWS FUTURE_MINOR MINIMAL)
    string(TOLOWER ${_fixture} _name)
    if (_fixture STREQUAL "WRONG_MAJOR")
        # Written in C, so the module ABI header is compiled as C.
        orca_add_module(module_fixture_${_name} module_fixture_${_name}.c)
        set_target_properties(module_fixture_${_name} PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    else ()
        orca_add_module(module_fixture_${_name} module_fixtures.cpp)
        target_compile_definitions(module_fixture_${_name} PRIVATE FIXTURE_${_fixture})
    endif ()
    set_target_properties(module_fixture_${_name} PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/fixtures)
    list(APPEND _fixture_targets module_fixture_${_name})
    string(APPEND _fixture_defines "#define FIXTURE_${_fixture} \"$<TARGET_FILE:module_fixture_${_name}>\"\n")
endforeach ()
file(GENERATE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/module_paths.h
     CONTENT "#pragma once
#define SAMPLE_LINES_MODULE \"$<TARGET_FILE:sample_lines_module>\"
#define SAMPLE_INFILL_WALLS \"$<TARGET_FILE:infill_walls>\"
#define SAMPLE_CHANGE_AT_Z \"$<TARGET_FILE:change_at_z>\"
${_fixture_defines}")

add_executable(${_TEST_NAME}_tests ${_TEST_NAME}_tests.cpp test_modules.cpp ${CMAKE_SOURCE_DIR}/tests/fff_print/test_helpers.cpp)
target_include_directories(${_TEST_NAME}_tests PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG> ${CMAKE_SOURCE_DIR}/tests/fff_print
                                                       ${CMAKE_SOURCE_DIR}/sdk/modules/include ${CMAKE_SOURCE_DIR}/sdk/infill/include)
target_link_libraries(${_TEST_NAME}_tests test_common libslic3r Catch2::Catch2WithMain)
add_dependencies(${_TEST_NAME}_tests ${_sample_targets} ${_fixture_targets})
set_property(TARGET ${_TEST_NAME}_tests PROPERTY FOLDER "tests")
orcaslicer_copy_test_dlls()
orcaslicer_discover_tests(${_TEST_NAME}_tests)
