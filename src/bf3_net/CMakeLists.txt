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

# M1 protocol (docs/design/m1.md §7): bit packing, the messages, the netsim. Messages carry sim::Command /
# sim::SoldierState, so the headers need bf3_sim and bf3_core.
target_link_libraries(bf3_net PUBLIC bf3_sim bf3_core)
target_sources(bf3_net PRIVATE
    include/bf3/net/bit_stream.h
    include/bf3/net/protocol.h
    include/bf3/net/netsim.h
)
