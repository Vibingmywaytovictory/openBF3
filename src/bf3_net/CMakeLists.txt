add_library(bf3_net STATIC
    src/enet_host.cpp
    src/loopback_ping.cpp
    include/bf3/net/enet_host.h
    include/bf3/net/loopback_ping.h
)
target_include_directories(bf3_net PUBLIC include)
target_link_libraries(bf3_net PRIVATE enet)
openbf3_module(bf3_net)

target_link_libraries(bf3_net PUBLIC bf3_sim bf3_core)
target_sources(bf3_net PRIVATE
    include/bf3/net/bit_stream.h
    include/bf3/net/protocol.h
    include/bf3/net/netsim.h
)

target_sources(bf3_net PRIVATE
    src/bit_archive.h
    src/bit_stream.cpp
    src/protocol.cpp
    src/netsim.cpp
)

target_sources(bf3_net PRIVATE
    include/bf3/net/weapons_wire.h
    include/bf3/net/other_sample.h
)

target_sources(bf3_net PRIVATE
    src/weapons_wire.cpp
    src/weapons_wire_layout.h
)

target_sources(bf3_net PRIVATE
    src/other_sample.cpp
)

find_package(Git QUIET)
set(OPENBF3_BUILD_ID_DIR "${CMAKE_CURRENT_BINARY_DIR}/build_id")
add_custom_target(bf3_build_id
    COMMAND "${CMAKE_COMMAND}" "-DGIT=${GIT_EXECUTABLE}" "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
        "-DCOMPILER=${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}" "-DOUT=${OPENBF3_BUILD_ID_DIR}/build_id.h"
        -P "${PROJECT_SOURCE_DIR}/cmake/build_id.cmake"
    BYPRODUCTS "${OPENBF3_BUILD_ID_DIR}/build_id.h"
    VERBATIM)
add_dependencies(bf3_net bf3_build_id)
target_include_directories(bf3_net PRIVATE "${OPENBF3_BUILD_ID_DIR}")
target_sources(bf3_net PRIVATE src/build_id.cpp)

target_sources(bf3_net PRIVATE
    src/ghost_delta.h
    src/ghost_delta.cpp
)
