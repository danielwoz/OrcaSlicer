get_filename_component(_TEST_NAME ${CMAKE_CURRENT_LIST_DIR} NAME)

# corpus.cpp holds the filler calls the suites of the built-in patterns share.
add_executable(${_TEST_NAME}_tests
    ${_TEST_NAME}_tests.cpp
    corpus.cpp
    corpus.hpp
    test_builtin_mirror.cpp
    test_golden.cpp
    test_parallel_for.cpp
    )
target_include_directories(${_TEST_NAME}_tests PRIVATE ${CMAKE_SOURCE_DIR}/sdk/infill/include)
target_link_libraries(${_TEST_NAME}_tests test_common libslic3r Catch2::Catch2WithMain)
set_property(TARGET ${_TEST_NAME}_tests PROPERTY FOLDER "tests")
orcaslicer_copy_test_dlls()
orcaslicer_discover_tests(${_TEST_NAME}_tests)
