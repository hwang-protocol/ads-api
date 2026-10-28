add_executable(common_tests tests/common_tests.cpp)
target_link_libraries(common_tests PRIVATE common)
add_test(NAME common_tests COMMAND common_tests)
