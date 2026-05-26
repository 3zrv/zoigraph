add_executable(test_placeholder test_placeholder.cpp)
target_link_libraries(test_placeholder PRIVATE doctest::doctest)
add_test(NAME placeholder COMMAND test_placeholder)

add_executable(test_forces test_forces.cpp)
target_link_libraries(test_forces PRIVATE zg_physics doctest::doctest)
add_test(NAME forces COMMAND test_forces)

add_executable(test_db test_db.cpp)
target_link_libraries(test_db PRIVATE zg_persistence doctest::doctest)
add_test(NAME db COMMAND test_db)

add_executable(test_graph_buffer test_graph_buffer.cpp)
target_link_libraries(test_graph_buffer PRIVATE zg_graph doctest::doctest)
if(NOT WIN32)
    target_link_libraries(test_graph_buffer PRIVATE pthread)
endif()
add_test(NAME graph_buffer COMMAND test_graph_buffer)

add_executable(test_phantom test_phantom.cpp)
target_link_libraries(test_phantom PRIVATE zg_telemetry doctest::doctest)
if(NOT WIN32)
    target_link_libraries(test_phantom PRIVATE pthread)
endif()
add_test(NAME phantom COMMAND test_phantom)

add_executable(test_integrator test_integrator.cpp)
target_link_libraries(test_integrator PRIVATE zg_physics_thread doctest::doctest)
add_test(NAME integrator COMMAND test_integrator)

add_executable(test_barnes_hut test_barnes_hut.cpp)
target_link_libraries(test_barnes_hut PRIVATE zg_physics doctest::doctest)
add_test(NAME barnes_hut COMMAND test_barnes_hut)

add_executable(test_picks test_picks.cpp)
target_link_libraries(test_picks PRIVATE zg_graph doctest::doctest)
add_test(NAME picks COMMAND test_picks)
