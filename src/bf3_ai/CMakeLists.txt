add_library(bf3_ai STATIC
    include/bf3/ai/knowledge/ai_object.h
    include/bf3/ai/knowledge/handle_pool.h
    include/bf3/ai/knowledge/knowledge_context.h
    include/bf3/ai/knowledge/information.h
    include/bf3/ai/knowledge/plug_ins.h
    include/bf3/ai/knowledge/weapon_info.h
    include/bf3/ai/knowledge/information_factory.h
    include/bf3/ai/knowledge/information_grid.h
    include/bf3/ai/behaviour/curves.h
    include/bf3/ai/behaviour/urgency_merger.h
    include/bf3/ai/main/ai_settings.h
    include/bf3/ai/main/ai_random.h
    include/bf3/ai/main/ai_game.h
    include/bf3/ai/main/bot.h
    include/bf3/ai/main/bot_manager.h
    include/bf3/ai/main/behaviour_manager.h
    include/bf3/ai/main/event_courier.h
    include/bf3/ai/main/player_courier.h
    include/bf3/ai/main/ai_main.h
    src/knowledge/information.cpp
    src/knowledge/physical_plug_in.cpp
    src/knowledge/unit_plug_in.cpp
    src/knowledge/cover_plug_in.cpp
    src/knowledge/mobile_plug_in.cpp
    src/knowledge/armament_plug_in.cpp
    src/knowledge/weapon_info.cpp
    src/knowledge/information_factory.cpp
    src/knowledge/information_grid.cpp
    src/behaviour/curves.cpp
    src/behaviour/urgency_merger.cpp
    src/main/ai_settings.cpp
    src/main/ai_random.cpp
    src/main/bot_manager.cpp
    src/main/behaviour_manager.cpp
    src/main/event_courier.cpp
    src/main/ai_main.cpp
    src/main/bf2_ai.cpp
)
target_include_directories(bf3_ai PUBLIC include)
target_link_libraries(bf3_ai PUBLIC bf3_sim bf3_core)
openbf3_module(bf3_ai)

target_sources(bf3_ai PRIVATE
    include/bf3/ai/squad/squad_world.h
    include/bf3/ai/squad/squad_member.h
    include/bf3/ai/squad/squad_formation.h
    include/bf3/ai/squad/squad_coordinator.h
    include/bf3/ai/squad/squad_manager.h
    src/squad/squad_member.cpp
    src/squad/squad_formation.cpp
    src/squad/squad_coordinator.cpp
    src/squad/squad_manager.cpp
)

target_sources(bf3_ai PRIVATE
    include/bf3/ai/path/material.h
    include/bf3/ai/path/map_properties.h
    include/bf3/ai/path/cluster_graph.h
    src/path/material.cpp
    src/path/map_properties.cpp
    src/path/cluster_graph.cpp
)

target_sources(bf3_ai PRIVATE
    include/bf3/ai/vehicle/vehicle_occupancy.h
    include/bf3/ai/vehicle/aerial_height_map.h
    include/bf3/ai/vehicle/landing_zone_manager.h
    src/vehicle/vehicle_occupancy.cpp
    src/vehicle/aerial_height_map.cpp
    src/vehicle/landing_zone_manager.cpp
)

target_sources(bf3_ai PRIVATE
    include/bf3/ai/strategic/bot_spawner.h
    src/strategic/bot_spawner.cpp
)

target_sources(bf3_ai PRIVATE
    include/bf3/ai/ai_world.h
    include/bf3/ai/nav/nav_clusters.h
    include/bf3/ai/nav/bot_navigation.h
    src/nav/nav_clusters.cpp
    src/nav/bot_navigation.cpp
)

target_sources(bf3_ai PRIVATE
    include/bf3/ai/strategic/strategic_object.h
    include/bf3/ai/strategic/sai.h
    src/strategic/strategic_object.cpp
    src/strategic/strategic_area.cpp
    src/strategic/sai.cpp
)

target_sources(bf3_ai PRIVATE
    include/bf3/ai/bot/infantry.h
    include/bf3/ai/bot/bot_main.h
    include/bf3/ai/behaviour/behaviours.h
    src/bot/bot_main.cpp
    src/behaviour/behaviours.cpp
)

target_sources(bf3_ai PRIVATE
    include/bf3/ai/strategic/strategy.h
    include/bf3/ai/strategic/strategy_model.h
    include/bf3/ai/squad/bot_squad.h
    src/strategic/strategy.cpp
    src/strategic/strategy_model.cpp
    src/squad/orders.cpp
)

target_sources(bf3_ai PRIVATE
    include/bf3/ai/knowledge/fire_object.h
    src/knowledge/fire_object.cpp
)

target_sources(bf3_ai PRIVATE
    src/knowledge/information_known.cpp
    src/knowledge/known_plug_ins.cpp
)

target_sources(bf3_ai PRIVATE
    include/bf3/ai/main/lod_manager.h
    src/main/lod_manager.cpp
)

target_sources(bf3_ai PRIVATE
    include/bf3/ai/debug/draw_primitives.h
    src/debug/draw_primitives.cpp
)

target_sources(bf3_ai PRIVATE
    include/bf3/ai/debug/ai_console.h
    src/debug/ai_console.cpp
)

target_sources(bf3_ai PRIVATE
    include/bf3/ai/actionplan/statement.h
    include/bf3/ai/actionplan/variables.h
    include/bf3/ai/actionplan/conditions.h
    include/bf3/ai/actionplan/flow_control.h
    src/actionplan/conditions.cpp
    src/actionplan/flow_control.cpp
    include/bf3/ai/actionplan/aimer.h
    src/actionplan/aimer.cpp
)

target_sources(bf3_ai PRIVATE
    include/bf3/ai/actionplan/infantry_actions.h
    include/bf3/ai/interpreter/bap_interpreter.h
    include/bf3/ai/interpreter/infantry_entries.h
    include/bf3/ai/interpreter/bot_plan.h
    src/actionplan/infantry_actions.cpp
    src/interpreter/bap_interpreter.cpp
    src/interpreter/infantry_entries.cpp
    src/interpreter/bot_plan.cpp
)

target_sources(bf3_ai PRIVATE
    include/bf3/ai/bot/bot_memory.h
    src/bot/bot_memory.cpp
)

target_sources(bf3_ai PRIVATE
    include/bf3/ai/bot/waypoints.h
    src/bot/waypoints.cpp
)

target_sources(bf3_ai PRIVATE
    include/bf3/ai/bot/communication_agent.h
    src/bot/communication_agent.cpp
)

target_sources(bf3_ai PRIVATE
    include/bf3/ai/commander/commander.h
    src/commander/commander.cpp
    src/strategic/sai_distribution.cpp
)
