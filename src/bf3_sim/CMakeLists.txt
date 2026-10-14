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
