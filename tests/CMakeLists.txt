# Placeholder test binary. Real unit tests land with the physics module (task #2).
add_executable(test_placeholder test_placeholder.cpp)
target_link_libraries(test_placeholder PRIVATE doctest::doctest)
add_test(NAME placeholder COMMAND test_placeholder)
