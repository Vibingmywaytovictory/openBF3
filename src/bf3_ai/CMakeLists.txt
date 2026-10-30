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
    include/bf3/ai/plan/plan_entries.h
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
    src/plan/plan_context.cpp
    src/plan/plan_entries.cpp
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
    include/bf3/ai/plan/infantry_entries.h
    src/bot/bot_main.cpp
    src/behaviour/behaviours.cpp
    src/plan/infantry_entries.cpp
)
