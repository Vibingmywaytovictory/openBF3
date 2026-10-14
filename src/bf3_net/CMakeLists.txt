# bf3_net: thin wrapper over upstream ENet 1.3.18. Godot-free. S0: hosts and a loopback ping.
add_library(bf3_net STATIC
    src/enet_host.cpp
    src/loopback_ping.cpp
    include/bf3/net/enet_host.h
    include/bf3/net/loopback_ping.h
)
target_include_directories(bf3_net PUBLIC include)
target_link_libraries(bf3_net PRIVATE enet)
openbf3_module(bf3_net)
