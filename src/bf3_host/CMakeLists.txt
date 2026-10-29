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
target_link_libraries(bf3_host PUBLIC bf3_present bf3_sim bf3_net bf3_core bf3_ai)
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
    src/server_soldiers.h
    src/server_snapshots.cpp
    src/server_snapshots.h
    src/server_spawner.cpp
    src/server_spawner.h
    src/client_session.cpp
    src/client_loop.cpp
    src/client_loop.h
    src/client_ticks.h
    src/client_transport.h
    src/client_transport.cpp
    src/client_prediction.cpp
    src/client_prediction.h
    src/client_input.cpp
    src/client_input.h
    src/client_gunplay.cpp
    src/client_gunplay.h
    src/client_match.cpp
    src/client_match.h
    src/client_events.cpp
    src/client_events.h
    src/client_tracks.cpp
    src/client_tracks.h
    src/client_clock.cpp
    src/client_clock.h
    src/client_link.cpp
    src/client_link.h
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
    include/bf3/host/loadout.h
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
    src/loadout.cpp
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
    src/tdm_bot_awareness.cpp
    src/tdm_bot_awareness.h
    src/tdm_bot_common.h
    src/tdm_bot_movement.cpp
    src/tdm_bot_movement.h
    src/tdm_bot_objectives.cpp
    src/tdm_bot_objectives.h
    src/tdm_bot_shared.cpp
    src/tdm_bot_shared.h
    src/tdm_bot_targeting.cpp
    src/tdm_bot_targeting.h
    src/tdm_bot_weapons.cpp
    src/tdm_bot_weapons.h
)

target_sources(bf3_host PRIVATE
    src/tdm_rules.cpp
)

target_sources(bf3_host PRIVATE
    src/hud_feed_tdm.cpp
    src/hud_feed_tdm_parts.h
    src/hud_feed_scoreboard.cpp
    src/hud_feed_kill_card.cpp
    src/hud_feed_spawn.cpp
    src/hud_feed_round_end.cpp
    src/hud_feed_loadout.cpp
    src/hud_feed_menu.cpp
    include/bf3/host/round_stats.h
    src/round_stats.cpp
)

target_sources(bf3_host PRIVATE
    include/bf3/host/ui_flow_session.h
    src/ui_flow_session.cpp
    include/bf3/host/logic_screen_effects.h
    src/logic_screen_effects.cpp
)

target_sources(bf3_host PRIVATE
    include/bf3/host/server_level.h
    src/server_level.cpp
    include/bf3/host/map_list.h
    src/map_list.cpp
    include/bf3/host/server_session.h
    src/server_session.cpp
    src/server_net_thread.cpp
    src/server_net_thread.h
)

target_sources(bf3_host PRIVATE
    src/server_handshake.cpp
    src/server_handshake.h
)

target_sources(bf3_host PRIVATE
    src/client_handshake.cpp
    src/client_handshake.h
)
