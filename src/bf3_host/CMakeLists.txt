add_library(bf3_host STATIC
    src/server_host.cpp
    src/loopback_client.cpp
    src/tick_timer.cpp
    src/tick_timer.h
    src/steady_tick_clock.cpp
    include/bf3/host/tick_clock.h
    include/bf3/host/server_host.h
    include/bf3/host/loopback_client.h
)
target_include_directories(bf3_host PUBLIC include)
target_link_libraries(bf3_host PUBLIC bf3_present bf3_sim bf3_net bf3_core)
openbf3_module(bf3_host)

target_sources(bf3_host PRIVATE
    include/bf3/host/level_load.h
    include/bf3/host/game_server.h
    include/bf3/host/client_session.h
)

target_sources(bf3_host PRIVATE
    src/level_load.cpp
    src/game_server.cpp
    src/server_combat.cpp
    src/server_combat.h
    src/server_combat_log.cpp
    src/server_combat_log.h
    src/server_commands.cpp
    src/server_commands.h
    src/server_dev.cpp
    src/server_dev.h
    src/server_match.cpp
    src/server_match.h
    src/server_outbox.h
    src/server_slot_view.cpp
    src/server_slot_view.h
    src/server_slots.h
    src/server_snapshots.cpp
    src/server_snapshots.h
    src/server_spawner.cpp
    src/server_spawner.h
    src/client_session.cpp
    src/rest_check.cpp
    include/bf3/host/rest_check.h
)
target_link_libraries(bf3_host PRIVATE psapi)

target_sources(bf3_host PRIVATE
    include/bf3/host/bot_fill.h
    include/bf3/host/bot_fill_stats.h
    include/bf3/host/scale_bench.h
    src/bot_director.h
    src/bot_fill.cpp
    src/scale_bench.cpp
)

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

target_sources(bf3_host PRIVATE
    src/lag_comp.cpp
)

target_sources(bf3_host PRIVATE
    src/presentation_weapon.cpp
    src/client_projectiles.cpp
)

target_sources(bf3_host PRIVATE
    src/kits.cpp
    src/target_bots.cpp
    src/gunplay.cpp
)
target_link_libraries(bf3_host PUBLIC bf3_anim)

target_sources(bf3_host PRIVATE
    src/hud_feed.cpp
)

target_sources(bf3_host PRIVATE
    include/bf3/host/firefight_brain.h
    src/firefight_brain.cpp
    include/bf3/host/client_wiring.h
    src/client_wiring.cpp
)

target_sources(bf3_host PRIVATE
    include/bf3/host/firefight_run.h
    src/firefight_run.cpp
)

target_sources(bf3_host PRIVATE
    include/bf3/host/tdm_rules.h
    include/bf3/host/tdm_brain.h
    src/tdm_brain.cpp
    src/tdm_bot_common.h
    src/tdm_bot_shared.cpp
    src/tdm_bot_shared.h
)

target_sources(bf3_host PRIVATE
    src/tdm_rules.cpp
)

target_sources(bf3_host PRIVATE
    src/hud_feed_tdm.cpp
)
