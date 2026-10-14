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
