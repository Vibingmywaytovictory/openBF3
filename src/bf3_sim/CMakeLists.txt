# bf3_sim: gameplay simulation on our own Jolt world. Godot-free. S0: a fixed-step world.
add_library(bf3_sim STATIC
    src/physics_world.cpp
    src/nav_link.cpp
    include/bf3/sim/physics_world.h
    include/bf3/sim/nav_link.h
)
target_include_directories(bf3_sim PUBLIC include)
target_link_libraries(bf3_sim PRIVATE Jolt Recast Detour)
openbf3_module(bf3_sim)

# M1 interfaces (docs/design/m1.md §4, §5, §6, §10): the static world of a level, the collision queries, the
# soldier motor and spawning. The sim reads bf3_core types (collision soup, cache, SoldierDef) in its headers.
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

# M1 T1 (world; docs/design/m1.md §5): the static world's Jolt shapes and their cache entries (static_world_cache.cpp),
# the per-owner CollisionWorld and its queries, and the process-wide Jolt runtime that PhysicsWorld, the shapes and
# every CollisionWorld share.
target_sources(bf3_sim PRIVATE
    src/jolt_runtime.cpp
    src/jolt_runtime.h
    src/static_world.cpp
    src/static_world_internal.h
    src/static_world_cache.cpp
    src/static_world_terrain.cpp
    src/collision_world.cpp
)

# M1 T2 (motor; docs/design/m1.md §6, §9.2, §10): the soldier motor on Jolt's CharacterVirtual, the state hash, the
# tuning file and the params, spawning and settle, the test-course searches, the prediction ring and sim::catch_up.
target_sources(bf3_sim PRIVATE
    include/bf3/sim/prediction.h
    src/soldier_state.cpp
    src/motor_tuning.cpp
    src/soldier_motor.cpp
    src/spawn.cpp
    src/prediction.cpp
)

# S4 logic (docs/design/s4.md §7-§9): the logic interpreter, its trace and ghost, the native port bindings and stubs,
# the scripted runs and gate judges.
target_sources(bf3_sim PRIVATE
    src/logic/logic_runtime.cpp
    src/logic/logic_ghost.cpp
    src/logic/logic_native.cpp
    src/logic/logic_harness.cpp
    include/bf3/sim/logic/logic_runtime.h
    include/bf3/sim/logic/logic_native.h
    include/bf3/sim/logic/logic_harness.h
)

# S1 nav (spike S1 server scale; docs/design/s1.md §5, §7, §11): the navmesh from the motor's collision set, its
# queries and the walker brain (bots/). Detour's path corridor lives in DetourCrowd.
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

# S2 vehicle (docs/design/s2.md §5-§8, §12): the vehicle model and tuning, the vehicle world (VehicleConstraint on our
# Jolt, save / restore), the quantised ghost, the courses and command logs, the replay harness and its gates.
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

# M2a weapons (docs/design/m2a-weapons.md §5, §13): the weapon state machine with sub-tick shot times, the
# counter-based shot RNG, spread and recoil with the unmeasured mechanics as switches, ballistics against the
# CollisionWorld and zone-capsule hit targets, damage from the level grid, and the headless shooting range.
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
# M2a weapons implementation (docs/design/m2a-weapons.md §13).
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

# M2a w2 (W2.0 contract, docs/design/m2a.md §5.2, §5.5, §6.5, §15): the integration rules (step_weapons_m2a, sv; the
# .cpp is a forwarding stub until sv lands it), the mechanics cfg parser (sv) and the posed target (lc).
target_sources(bf3_sim PRIVATE
    include/bf3/sim/weapons/weapon_rules.h
    src/weapons/weapon_rules.cpp
    include/bf3/sim/weapons/mechanics_tuning.h
    include/bf3/sim/weapons/posed_target.h
)

# M2a lc (docs/design/m2a.md §6.5; docs/design/m2a-lc.md): hit targets posed from the capsule pose table.
target_sources(bf3_sim PRIVATE
    src/weapons/posed_target.cpp
)

# M2a sv (docs/design/m2a.md §5.5; docs/design/m2a-sv.md): the weapon_mechanics.cfg parser.
target_sources(bf3_sim PRIVATE
    src/weapons/mechanics_tuning.cpp
)

# M2b W1.0 (docs/design/m2b.md §2.6, §11; docs/design/m2b-w1.md): the wave-1 bf3_sim interface headers. ru adds
# round_natives.cpp and the scored spawn choice, bb perception.cpp and aim_model.cpp, each in a block of its own.
target_sources(bf3_sim PRIVATE
    include/bf3/sim/logic/round_natives.h
    include/bf3/sim/bots/perception.h
    include/bf3/sim/bots/aim_model.h
)

# M2b bb (docs/design/m2b.md §5.1, §5.3; docs/design/m2b-bb.md): the bot perception and aim model bodies.
target_sources(bf3_sim PRIVATE
    src/bots/perception.cpp
    src/bots/aim_model.cpp
)
