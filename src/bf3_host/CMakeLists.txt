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

# S1 bots (spike S1; docs/design/s1.md §6, §8, §11): walker bots on the GameServer and the scale measurement.
target_sources(bf3_host PRIVATE
    include/bf3/host/bot_fill.h
    include/bf3/host/bot_fill_stats.h
    include/bf3/host/scale_bench.h
    src/bot_director.h
    src/bot_fill.cpp
    src/scale_bench.cpp
)

# M2a w2 (W2.0 contract, docs/design/m2a.md §4, §6.6, §8, §10.1, §11.4, §15): server gunplay (sv), lag compensation
# (lc), the client's events, presented weapon and cosmetic projectiles (cl) and the HUD feed (hud).
target_sources(bf3_host PRIVATE
    include/bf3/host/gunplay.h
    include/bf3/host/kits.h
    include/bf3/host/target_bots.h
    include/bf3/host/server_view.h
    include/bf3/host/lag_comp.h
    include/bf3/host/presentation_events.h
    include/bf3/host/presentation_weapon.h
    include/bf3/host/client_projectiles.h
    include/bf3/host/hud_feed.h
)

# M2a lc (docs/design/m2a.md §6.3, §6.4, §6.6; docs/design/m2a-lc.md): the lag history and the rewind provider.
target_sources(bf3_host PRIVATE
    src/lag_comp.cpp
)

# M2a cl (docs/design/m2a.md §8; docs/design/m2a-cl.md): the events ring, the presented weapon and the client's
# cosmetic projectiles.
target_sources(bf3_host PRIVATE
    src/presentation_events.cpp
    src/presentation_weapon.cpp
    src/client_projectiles.cpp
)

# M2a sv (docs/design/m2a.md §4, §5.6, §10.1; docs/design/m2a-sv.md): server gunplay, kits, target bots. bf3_anim for
# the capsule pose table built from the retail clips (lc's builder).
target_sources(bf3_host PRIVATE
    src/kits.cpp
    src/target_bots.cpp
    src/gunplay.cpp
)
target_link_libraries(bf3_host PUBLIC bf3_anim)
