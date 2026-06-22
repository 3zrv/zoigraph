add_executable(test_placeholder test_placeholder.cpp)
target_link_libraries(test_placeholder PRIVATE doctest::doctest)
add_test(NAME placeholder COMMAND test_placeholder)

add_executable(test_forces test_forces.cpp)
target_link_libraries(test_forces PRIVATE zg_physics doctest::doctest)
add_test(NAME forces COMMAND test_forces)

add_executable(test_db test_db.cpp)
target_link_libraries(test_db PRIVATE zg_persistence doctest::doctest)
add_test(NAME db COMMAND test_db)

add_executable(test_project_store test_project_store.cpp)
target_link_libraries(test_project_store PRIVATE zg_persistence doctest::doctest)
add_test(NAME project_store COMMAND test_project_store)

add_executable(test_settings test_settings.cpp)
target_link_libraries(test_settings PRIVATE zg_app doctest::doctest)
add_test(NAME settings COMMAND test_settings)

add_executable(test_session test_session.cpp)
target_link_libraries(test_session PRIVATE zg_app doctest::doctest)
add_test(NAME session COMMAND test_session)

add_executable(test_paths test_paths.cpp)
target_link_libraries(test_paths PRIVATE zg_app doctest::doctest)
add_test(NAME paths COMMAND test_paths)

add_executable(test_cli test_cli.cpp)
target_link_libraries(test_cli PRIVATE zg_ui doctest::doctest)
add_test(NAME cli COMMAND test_cli)

add_executable(test_secure_wipe test_secure_wipe.cpp)
target_link_libraries(test_secure_wipe PRIVATE zg_persistence doctest::doctest)
add_test(NAME secure_wipe COMMAND test_secure_wipe)

add_executable(test_seed test_seed.cpp)
target_link_libraries(test_seed PRIVATE zg_persistence doctest::doctest)
add_test(NAME seed COMMAND test_seed)

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

add_executable(test_query_protocol test_query_protocol.cpp)
target_link_libraries(test_query_protocol PRIVATE zg_telemetry doctest::doctest)
if(NOT WIN32)
    target_link_libraries(test_query_protocol PRIVATE pthread)
endif()
add_test(NAME query_protocol COMMAND test_query_protocol)

add_executable(test_integrator test_integrator.cpp)
target_link_libraries(test_integrator PRIVATE zg_physics_thread doctest::doctest)
add_test(NAME integrator COMMAND test_integrator)

add_executable(test_barnes_hut test_barnes_hut.cpp)
target_link_libraries(test_barnes_hut PRIVATE zg_physics doctest::doctest)
add_test(NAME barnes_hut COMMAND test_barnes_hut)

add_executable(test_picks test_picks.cpp)
target_link_libraries(test_picks PRIVATE zg_graph doctest::doctest)
add_test(NAME picks COMMAND test_picks)

add_executable(test_rabbit test_rabbit.cpp)
target_link_libraries(test_rabbit PRIVATE zg_macros doctest::doctest)
add_test(NAME rabbit COMMAND test_rabbit)

add_executable(test_wikilinks test_wikilinks.cpp)
target_link_libraries(test_wikilinks PRIVATE zg_graph doctest::doctest)
add_test(NAME wikilinks COMMAND test_wikilinks)

add_executable(test_cluster test_cluster.cpp)
target_link_libraries(test_cluster PRIVATE zg_graph doctest::doctest)
add_test(NAME cluster COMMAND test_cluster)

add_executable(test_ppr test_ppr.cpp)
target_link_libraries(test_ppr PRIVATE zg_graph doctest::doctest)
add_test(NAME ppr COMMAND test_ppr)

add_executable(test_timeline test_timeline.cpp)
target_link_libraries(test_timeline PRIVATE zg_graph doctest::doctest)
add_test(NAME timeline COMMAND test_timeline)

add_executable(test_escape_wipe test_escape_wipe.cpp)
target_link_libraries(test_escape_wipe PRIVATE zg_input doctest::doctest)
add_test(NAME escape_wipe COMMAND test_escape_wipe)

add_executable(test_ask test_ask.cpp)
target_link_libraries(test_ask PRIVATE zg_app doctest::doctest)
add_test(NAME ask COMMAND test_ask)

add_executable(test_promote test_promote.cpp)
target_link_libraries(test_promote PRIVATE zg_app doctest::doctest)
add_test(NAME promote COMMAND test_promote)

add_executable(test_query_responder test_query_responder.cpp)
target_link_libraries(test_query_responder PRIVATE zg_app doctest::doctest)
add_test(NAME query_responder COMMAND test_query_responder)

add_executable(test_query_token test_query_token.cpp)
target_link_libraries(test_query_token PRIVATE zg_app doctest::doctest)
add_test(NAME query_token COMMAND test_query_token)

add_executable(test_phantom_lifecycle test_phantom_lifecycle.cpp)
target_link_libraries(test_phantom_lifecycle PRIVATE zg_app doctest::doctest)
add_test(NAME phantom_lifecycle COMMAND test_phantom_lifecycle)

add_executable(test_labels test_labels.cpp)
target_link_libraries(test_labels PRIVATE zg_render doctest::doctest)
add_test(NAME labels COMMAND test_labels)
