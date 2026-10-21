add_library(bf3_sim_harness STATIC
    include/bf3/sim_harness/logic_harness.h
    include/bf3/sim_harness/nav_input_check.h
    include/bf3/sim_harness/shooting_range.h
    include/bf3/sim_harness/vehicle_course.h
    include/bf3/sim_harness/vehicle_replay.h
    include/bf3/sim_harness/weapon_checks.h
    include/bf3/sim_harness/weapon_install_checks.h
    src/logic_harness.cpp
    src/nav_input_check.cpp
    src/shooting_range.cpp
    src/vehicle_course.cpp
    src/vehicle_replay.cpp
    src/weapon_checks.cpp
    src/weapon_install_checks.cpp
)
target_include_directories(bf3_sim_harness PUBLIC include)
target_include_directories(bf3_sim_harness PRIVATE ${CMAKE_SOURCE_DIR}/src/bf3_sim/src)
target_link_libraries(bf3_sim_harness PUBLIC bf3_sim)
target_link_libraries(bf3_sim_harness PRIVATE Jolt Recast Detour)
openbf3_module(bf3_sim_harness)
