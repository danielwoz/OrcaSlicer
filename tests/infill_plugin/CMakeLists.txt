get_filename_component(_TEST_NAME ${CMAKE_CURRENT_LIST_DIR} NAME)
include(${CMAKE_SOURCE_DIR}/sdk/infill/OrcaInfillPlugin.cmake)

# The sample plugin, alone in its directory so that a directory scan finds only it.
orca_add_infill_plugin(sample_infill_lines ${CMAKE_SOURCE_DIR}/sdk/infill/samples/lines/lines.cpp)
set_target_properties(sample_infill_lines PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/plugins)

# Broken, misbehaving and service-using plugins, built from one C++ source, and one plugin written in C.
set(_fixture_defines "")
set(_fixture_targets "")
foreach (_fixture WRONG_ABI NO_ENTRY BAD_KEY DUPLICATE_KEY CONNECT_WITHOUT_CLIP FAILS_ODD_LAYERS THROWS TOP_ONLY INSET BAD_PARAM
                  DECLINES)
    string(TOLOWER ${_fixture} _name)
    if (_fixture STREQUAL "DECLINES")
        # Written in C, so the ABI header is compiled as C.
        orca_add_infill_plugin(infill_fixture_${_name} declines.c)
        set_target_properties(infill_fixture_${_name} PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    else ()
        orca_add_infill_plugin(infill_fixture_${_name} fixtures.cpp)
        target_compile_definitions(infill_fixture_${_name} PRIVATE FIXTURE_${_fixture})
    endif ()
    set_target_properties(infill_fixture_${_name} PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/fixtures)
    list(APPEND _fixture_targets infill_fixture_${_name})
    string(APPEND _fixture_defines "#define INFILL_FIXTURE_${_fixture} \"$<TARGET_FILE:infill_fixture_${_name}>\"\n")
endforeach ()
file(GENERATE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/infill_plugin_paths.h
     CONTENT "#pragma once\n#define SAMPLE_INFILL_PLUGIN \"$<TARGET_FILE:sample_infill_lines>\"\n${_fixture_defines}")

add_executable(${_TEST_NAME}_tests ${_TEST_NAME}_tests.cpp test_infill_plugins.cpp ${CMAKE_SOURCE_DIR}/tests/fff_print/test_helpers.cpp)
target_include_directories(${_TEST_NAME}_tests PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG> ${CMAKE_SOURCE_DIR}/tests/fff_print
                                                       ${CMAKE_SOURCE_DIR}/sdk/infill/include)
target_link_libraries(${_TEST_NAME}_tests test_common libslic3r Catch2::Catch2WithMain)
add_dependencies(${_TEST_NAME}_tests sample_infill_lines ${_fixture_targets})
set_property(TARGET ${_TEST_NAME}_tests PROPERTY FOLDER "tests")
orcaslicer_copy_test_dlls()
orcaslicer_discover_tests(${_TEST_NAME}_tests)
