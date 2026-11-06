# Runs the real orca-slicer binary, so it needs the built app and resources/, not just build/tests.
# The CI unit-test job only receives build/tests, so the test carries the RequiresApp label that
# scripts/run_unit_tests.sh excludes. Run it with `ctest -C Release -L RequiresApp`. It also exits 77
# (skipped) when the binary is missing.

find_program(ORCA_CLI_TEST_PYTHON NAMES python3)
if (NOT ORCA_CLI_TEST_PYTHON)
    message(STATUS "python3 not found, not registering the CLI tests")
    return()
endif ()

add_test(NAME cli_strict_mode
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_cli_strict.sh $<TARGET_FILE:OrcaSlicer> ${ORCA_CLI_TEST_PYTHON})
add_test(NAME cli_empty_project_config
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_cli_empty_project_config.sh $<TARGET_FILE:OrcaSlicer> ${ORCA_CLI_TEST_PYTHON} ${CMAKE_SOURCE_DIR})
set_tests_properties(cli_empty_project_config PROPERTIES
    LABELS "CLI;RequiresApp"
    SKIP_RETURN_CODE 77
    TIMEOUT 300)

set_tests_properties(cli_strict_mode PROPERTIES
    LABELS "CLI;RequiresApp"
    SKIP_RETURN_CODE 77
    TIMEOUT 900)

add_test(NAME cli_project_missing_keys
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_cli_project_missing_keys.sh $<TARGET_FILE:OrcaSlicer> ${ORCA_CLI_TEST_PYTHON}
        ${CMAKE_SOURCE_DIR}/resources/profiles/BBL)
set_tests_properties(cli_project_missing_keys PROPERTIES
    LABELS "CLI;RequiresApp"
    SKIP_RETURN_CODE 77
    TIMEOUT 900)

add_test(NAME cli_arrange_rotations
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_cli_arrange_rotations.sh $<TARGET_FILE:OrcaSlicer> ${ORCA_CLI_TEST_PYTHON}
        ${CMAKE_SOURCE_DIR}/resources/profiles/BBL)
set_tests_properties(cli_arrange_rotations PROPERTIES
    LABELS "CLI;RequiresApp"
    SKIP_RETURN_CODE 77
    TIMEOUT 900)

add_test(NAME cli_no_suitable_objects
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_cli_no_suitable_objects.sh $<TARGET_FILE:OrcaSlicer> ${ORCA_CLI_TEST_PYTHON}
        ${CMAKE_SOURCE_DIR}/resources/profiles/BBL)
set_tests_properties(cli_no_suitable_objects PROPERTIES
    LABELS "CLI;RequiresApp"
    SKIP_RETURN_CODE 77
    TIMEOUT 900)

add_test(NAME cli_filament_colour_count
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_cli_filament_colour_count.sh $<TARGET_FILE:OrcaSlicer> ${ORCA_CLI_TEST_PYTHON}
        ${CMAKE_SOURCE_DIR}/resources/profiles/BBL)
set_tests_properties(cli_filament_colour_count PROPERTIES
    LABELS "CLI;RequiresApp"
    SKIP_RETURN_CODE 77
    TIMEOUT 300)

add_test(NAME cli_malformed_input
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_cli_malformed_input.sh $<TARGET_FILE:OrcaSlicer> ${ORCA_CLI_TEST_PYTHON})
set_tests_properties(cli_malformed_input PROPERTIES
    LABELS "CLI;RequiresApp"
    SKIP_RETURN_CODE 77
    TIMEOUT 900)

# Infill patterns left out of the build (ORCA_INFILL_<NAME>=OFF, see src/libslic3r/CMakeLists.txt) slice
# as their fallbacks. Skipped when the build includes every pattern.
add_test(NAME cli_infill_unavailable
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_cli_infill_unavailable.sh $<TARGET_FILE:OrcaSlicer> ${ORCA_CLI_TEST_PYTHON}
        "${ORCA_INFILL_LEFT_OUT}")
set_tests_properties(cli_infill_unavailable PROPERTIES
    LABELS "CLI;RequiresApp"
    SKIP_RETURN_CODE 77
    TIMEOUT 900)

# The built-in infill pattern libraries next to the binary, and a copy of the application without one of them. The copy
# and the file names are those of a Linux build tree.
if (ORCA_INFILL_PATTERN_LINKAGE STREQUAL "MODULE" AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_test(NAME cli_infill_pattern_libraries
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_cli_infill_pattern_libraries.sh $<TARGET_FILE:OrcaSlicer>)
    set_tests_properties(cli_infill_pattern_libraries PROPERTIES
        LABELS "CLI;RequiresApp"
        SKIP_RETURN_CODE 77
        TIMEOUT 900)
endif ()

# Infill pattern plugins: the sample plugin of tests/infill_plugin, loaded from the environment or a CLI flag.
if (TARGET sample_infill_lines)
    add_test(NAME cli_infill_plugin
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test_cli_infill_plugin.sh $<TARGET_FILE:OrcaSlicer> ${ORCA_CLI_TEST_PYTHON}
            $<TARGET_FILE_DIR:sample_infill_lines>)
    set_tests_properties(cli_infill_plugin PROPERTIES
        LABELS "CLI;RequiresApp"
        SKIP_RETURN_CODE 77
        TIMEOUT 900)
endif ()
