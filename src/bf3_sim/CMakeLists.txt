add_library(bf3_sim STATIC
    src/physics_world.cpp
    src/nav_link.cpp
    include/bf3/sim/physics_world.h
    include/bf3/sim/nav_link.h
)
target_include_directories(bf3_sim PUBLIC include)
target_link_libraries(bf3_sim PRIVATE Jolt Recast Detour)
openbf3_module(bf3_sim)

target_link_libraries(bf3_sim PUBLIC bf3_core)
target_sources(bf3_sim PRIVATE
    include/bf3/sim/sim_types.h
    include/bf3/sim/command.h
    include/bf3/sim/soldier_state.h
    include/bf3/sim/static_world.h
    include/bf3/sim/collision_world.h
    include/bf3/sim/soldier_motor.h
    include/bf3/sim/spawn.h
    src/collision_world_internal.h
)

target_sources(bf3_sim PRIVATE
    src/jolt_runtime.cpp
    src/jolt_runtime.h
    src/static_world.cpp
    src/static_world_internal.h
    src/static_world_cache.cpp
    src/static_world_terrain.cpp
    src/collision_world.cpp
)

target_sources(bf3_sim PRIVATE
    include/bf3/sim/prediction.h
    src/soldier_state.cpp
    src/motor_tuning.cpp
    src/soldier_motor.cpp
    src/spawn.cpp
    src/prediction.cpp
)

target_sources(bf3_sim PRIVATE
    src/logic/logic_runtime.cpp
    src/logic/logic_ghost.cpp
    src/logic/logic_native.cpp
    src/logic/logic_harness.cpp
    include/bf3/sim/logic/logic_runtime.h
    include/bf3/sim/logic/logic_native.h
    include/bf3/sim/logic/logic_harness.h
)

target_sources(bf3_sim PRIVATE
    include/bf3/sim/nav.h
    include/bf3/sim/nav_query.h
    include/bf3/sim/bots/bot_stats.h
    include/bf3/sim/bots/walker_brain.h
    src/nav_internal.h
    src/nav.cpp
    src/nav_bake.cpp
    src/nav_cache.cpp
    src/nav_validate.cpp
    src/nav_components.cpp
    src/nav_query.cpp
    src/bots/path_service.cpp
    src/bots/walker_brain.cpp
)
target_link_libraries(bf3_sim PRIVATE DetourCrowd)

target_sources(bf3_sim PRIVATE
    include/bf3/sim/vehicle_types.h
    include/bf3/sim/vehicle_ghost.h
    include/bf3/sim/vehicle_model.h
    include/bf3/sim/vehicle_world.h
    include/bf3/sim/vehicle_course.h
    include/bf3/sim/vehicle_replay.h
    src/vehicle_types.cpp
    src/vehicle_ghost.cpp
    src/vehicle_tuning.cpp
    src/vehicle_model.cpp
    src/vehicle_world_internal.h
    src/vehicle_world.cpp
    src/vehicle_course.cpp
    src/vehicle_replay.cpp
)

target_sources(bf3_sim PRIVATE
    include/bf3/sim/weapons/sim_time.h
    include/bf3/sim/weapons/weapon_input.h
    include/bf3/sim/weapons/weapon_rng.h
    include/bf3/sim/weapons/mechanics.h
    include/bf3/sim/weapons/hit_target.h
    include/bf3/sim/weapons/surface_materials.h
    include/bf3/sim/weapons/weapon_sim_def.h
    include/bf3/sim/weapons/spread_recoil.h
    include/bf3/sim/weapons/weapon_sim.h
    include/bf3/sim/weapons/damage.h
    include/bf3/sim/weapons/ballistics.h
    include/bf3/sim/weapons/shooting_range.h
)
target_sources(bf3_sim PRIVATE
    src/weapons/weapons_internal.h
    src/weapons/weapon_sim_def.cpp
    src/weapons/surface_materials.cpp
    src/weapons/spread_recoil.cpp
    src/weapons/weapon_sim.cpp
    src/weapons/hit_target.cpp
    src/weapons/damage.cpp
    src/weapons/ballistics.cpp
    src/weapons/shooting_range.cpp
    include/bf3/sim/weapons/weapon_checks.h
    src/weapons/weapon_checks.cpp
    include/bf3/sim/weapons/weapon_install.h
    src/weapons/weapon_install.cpp
)

target_sources(bf3_sim PRIVATE
    include/bf3/sim/weapons/weapon_rules.h
    src/weapons/weapon_rules.cpp
    include/bf3/sim/weapons/mechanics_tuning.h
    include/bf3/sim/weapons/posed_target.h
)

target_sources(bf3_sim PRIVATE
    src/weapons/posed_target.cpp
)

target_sources(bf3_sim PRIVATE
    src/weapons/mechanics_tuning.cpp
)

target_sources(bf3_sim PRIVATE
    include/bf3/sim/logic/round_natives.h
    include/bf3/sim/bots/perception.h
    include/bf3/sim/bots/aim_model.h
)

target_sources(bf3_sim PRIVATE
    src/logic/round_natives.cpp
)

target_sources(bf3_sim PRIVATE
    src/bots/perception.cpp
    src/bots/aim_model.cpp
)
