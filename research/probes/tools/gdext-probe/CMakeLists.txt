# gdext-probe: measures the Godot 4.8 APIs the BF3 reimplementation depends on.
# Build (from a VS x64 developer shell, see README.md):
#   cmake -S <this dir> -B <scratch build dir> -G Ninja -DCMAKE_BUILD_TYPE=Release
#         -DGODOT_CPP_DIR=<godot-cpp checkout>
#         -DGODOTCPP_CUSTOM_API_FILE=<extension_api.json dumped by the exact Godot build>
#         -DGODOTCPP_GDEXTENSION_DIR=<dir holding gdextension_interface.json dumped by the same build>
#         -DPROBE_OUT_DIR=<godot project copy>/bin
#   cmake --build <scratch build dir>
cmake_minimum_required(VERSION 3.20)
project(gdext_probe LANGUAGES CXX)

set(GODOT_CPP_DIR "" CACHE PATH "Path to a godot-cpp checkout (pinned commit, see README.md)")
set(PROBE_OUT_DIR "${CMAKE_BINARY_DIR}/out" CACHE PATH "Where the probe DLL is written (the test project's bin/)")
if(NOT GODOT_CPP_DIR)
    message(FATAL_ERROR "Set -DGODOT_CPP_DIR=<godot-cpp checkout>")
endif()

# godot-cpp reads GODOTCPP_CUSTOM_API_FILE / GODOTCPP_GDEXTENSION_DIR / GODOTCPP_TARGET from the cache.
add_subdirectory("${GODOT_CPP_DIR}" "${CMAKE_BINARY_DIR}/godot-cpp")
# godot-cpp's windows.cmake points its PDB at ${CMAKE_SOURCE_DIR}/bin; keep the source tree clean.
set_target_properties(godot-cpp PROPERTIES PDB_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/pdb>")

add_library(gdext_probe SHARED
    src/register_types.cpp
    src/probe_node.cpp
    src/probe_node.h
)
target_link_libraries(gdext_probe PRIVATE godot-cpp)

get_target_property(GODOTCPP_SUFFIX godot-cpp GODOTCPP_SUFFIX)
set_target_properties(gdext_probe PROPERTIES
    CXX_STANDARD 17
    CXX_EXTENSIONS OFF
    LIBRARY_OUTPUT_DIRECTORY "$<1:${PROBE_OUT_DIR}>"
    RUNTIME_OUTPUT_DIRECTORY "$<1:${PROBE_OUT_DIR}>"
    PDB_OUTPUT_DIRECTORY     "$<1:${PROBE_OUT_DIR}>"
    PREFIX "lib"
    OUTPUT_NAME "gdext_probe${GODOTCPP_SUFFIX}"
)
