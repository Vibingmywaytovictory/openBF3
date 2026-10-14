# antdump: Godot-free CLI over bf3_core's Ant reader (census, clip JSON dump, oracle comparison, bench).
# antdump_lib is shared with the core.ant tests.
add_library(antdump_lib STATIC antdump_lib.cpp antdump_lib.h mini_json.h)
target_include_directories(antdump_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(antdump_lib PUBLIC bf3_core)
openbf3_module(antdump_lib)

add_executable(antdump main.cpp)
target_link_libraries(antdump PRIVATE antdump_lib)
openbf3_module(antdump)
