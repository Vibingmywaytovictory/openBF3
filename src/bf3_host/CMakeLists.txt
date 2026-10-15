# bf3_host: ServerHost (own std::thread, fixed tick) and the loopback client. Calls no Godot API.
add_library(bf3_host STATIC
    src/server_host.cpp
    src/loopback_client.cpp
    src/tick_timer.cpp
    src/tick_timer.h
    include/bf3/host/server_host.h
    include/bf3/host/loopback_client.h
)
target_include_directories(bf3_host PUBLIC include)
target_link_libraries(bf3_host PUBLIC bf3_sim bf3_net bf3_core)
openbf3_module(bf3_host)

# M1 session (docs/design/m1.md §8, §9, §12): level load, the game server, the client session.
target_sources(bf3_host PRIVATE
    include/bf3/host/level_load.h
    include/bf3/host/game_server.h
    include/bf3/host/client_session.h
)

# M1 T4 (session; docs/design/m1.md §7.4-§9, §12): the level load, the game server, the client session, the E2 rest
# judge. psapi for the loader's peak working-set sample (LoadProgress::peak_working_set).
target_sources(bf3_host PRIVATE
    src/level_load.cpp
    src/game_server.cpp
    src/client_session.cpp
    src/rest_check.cpp
    include/bf3/host/rest_check.h
)
target_link_libraries(bf3_host PRIVATE psapi)
