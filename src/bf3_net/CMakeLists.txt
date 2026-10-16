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

# M1 T3 net (docs/design/m1.md §7, §15): the bodies of bit_stream.h, protocol.h and netsim.h.
target_sources(bf3_net PRIVATE
    src/bit_stream.cpp
    src/protocol.cpp
    src/netsim.cpp
)

# M2a w2 (W2.0 contract, docs/design/m2a.md §6.1, §7, §15): the own-weapons wire view (nt) and the shared sampler of
# one other soldier (lc).
target_sources(bf3_net PRIVATE
    include/bf3/net/weapons_wire.h
    include/bf3/net/other_sample.h
)

# M2a nt (docs/design/m2a-nt.md): the body of weapons_wire.h.
target_sources(bf3_net PRIVATE
    src/weapons_wire.cpp
)
