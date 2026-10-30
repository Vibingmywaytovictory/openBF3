set(godot_glue ${PROJECT_SOURCE_DIR}/src/bf3_godot/src)
add_library(bf3_godot_server SHARED
    register_server_types.cpp
    dedicated_main_loop.cpp
    dedicated_main_loop.h
    ${godot_glue}/godot_util.cpp
    ${godot_glue}/godot_util.h
    ${godot_glue}/godot_cache.cpp
    ${godot_glue}/godot_cache.h
    ${godot_glue}/tdm_flow.cpp
    ${godot_glue}/tdm_flow.h
    ${godot_glue}/tdm_args.cpp
)
target_include_directories(bf3_godot_server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${godot_glue})
target_link_libraries(bf3_godot_server PRIVATE godot-cpp bf3_host)
target_compile_definitions(bf3_godot_server PRIVATE OPENBF3_BUILD_CHECKS=0)
openbf3_module(bf3_godot_server)
target_link_options(bf3_godot_server PRIVATE /INCREMENTAL:NO "/MAP:${CMAKE_BINARY_DIR}/pdb/bf3_godot_server.map")

get_target_property(GODOTCPP_SUFFIX godot-cpp GODOTCPP_SUFFIX)
set_target_properties(bf3_godot_server PROPERTIES
    PREFIX "lib"
    OUTPUT_NAME "openbf3_server${GODOTCPP_SUFFIX}"
    RUNTIME_OUTPUT_DIRECTORY "$<1:${OPENBF3_GODOT_SERVER_PROJECT}/bin>"
    LIBRARY_OUTPUT_DIRECTORY "$<1:${OPENBF3_GODOT_SERVER_PROJECT}/bin>"
    ARCHIVE_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/lib>"
    PDB_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/pdb>"
)

include(${PROJECT_SOURCE_DIR}/cmake/server_deps_guard.cmake)
cmake_language(DEFER DIRECTORY ${PROJECT_SOURCE_DIR} CALL openbf3_server_deps_guard dedicated_main_loop.cpp)
