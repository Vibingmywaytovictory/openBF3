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
