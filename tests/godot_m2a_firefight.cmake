# M2a gx ctest godot.m2a_firefight (docs/design/m2a.md §1 E13 / E14, §10.2; docs/design/m2a-gx.md §5): the windowed
# firefight case with bt's bots,
#   <godot> --path bf3-opensource res://scenes/m2a_metro.tscn -- --m2a-firefight --bf3-cache=<BUILD_DIR>/godot-cache/godot.m2a
# A timing row (frame time and the event / effect / voice counts under 64 firing bots): the integrator's, on main
# after the merges (timing rows run on main only). Until bt lands the case prints "SKIP firefight waiting for bt"
# and the row exits 77. Skips (77) also like godot.m2a.
# Script mode: cmake -DPROJECT_DIR=<bf3-opensource> [-DBUILD_DIR=<b/dev>] -P godot_m2a_firefight.cmake
cmake_minimum_required(VERSION 3.29)
set(GODOT_WINDOWED TRUE)
include(${CMAKE_CURRENT_LIST_DIR}/godot_common.cmake)
execute_process(
    COMMAND "${godot}" --path "${PROJECT_DIR}" --quit
    TIMEOUT 60
    RESULT_VARIABLE probe_rc
    OUTPUT_VARIABLE probe_out
    ERROR_VARIABLE probe_err)
if(NOT probe_rc STREQUAL "0")
    message("${probe_out}")
    message("${probe_err}")
    message("SKIPPED: no window or rendering device (probe exit '${probe_rc}')")
    cmake_language(EXIT 77)
endif()
set(cache_args "")
if(DEFINED BUILD_DIR AND NOT BUILD_DIR STREQUAL "")
    file(MAKE_DIRECTORY "${BUILD_DIR}/godot-cache/godot.m2a")
    set(cache_args "--bf3-cache=${BUILD_DIR}/godot-cache/godot.m2a")
endif()
execute_process(
    COMMAND "${godot}" --path "${PROJECT_DIR}" res://scenes/m2a_metro.tscn -- --m2a-firefight ${cache_args}
    TIMEOUT 900
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err)
message("${out}")
if(NOT err STREQUAL "")
    message("${err}")
endif()
if(out MATCHES "no usable install" OR err MATCHES "no usable install")
    message("SKIPPED: no BF3 install found")
    cmake_language(EXIT 77)
endif()
if(out MATCHES "SKIP firefight")
    message("SKIPPED: the firefight case waits for bt")
    cmake_language(EXIT 77)
endif()
if(NOT rc STREQUAL "0" OR NOT out MATCHES "M2A CHECK: PASS")
    message(FATAL_ERROR "godot.m2a_firefight failed (exit ${rc})")
endif()
