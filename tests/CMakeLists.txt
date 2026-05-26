add_executable(test_placeholder test_placeholder.cpp)
target_link_libraries(test_placeholder PRIVATE doctest::doctest)
add_test(NAME placeholder COMMAND test_placeholder)

add_executable(test_forces test_forces.cpp)
target_link_libraries(test_forces PRIVATE zg_physics doctest::doctest)
add_test(NAME forces COMMAND test_forces)
