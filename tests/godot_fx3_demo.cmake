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
set(shots "$ENV{OPENBF3_FX3_SHOT_DIR}")
if(shots STREQUAL "")
    set(shots "${BUILD_DIR}/shots/godot.fx3_demo")
endif()
set(root "${BUILD_DIR}/scratch/godot.fx3_demo/bf3cache")
file(MAKE_DIRECTORY "${root}")
execute_process(
    COMMAND "${godot}" --path "${PROJECT_DIR}" res://scenes/fx3_demo.tscn -- --fx3-check --walkers=0 --no-hud "--bf3-cache=${root}"
        "--fx3-shot-dir=${shots}" --quit-after=6000
    TIMEOUT 600
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
if(NOT rc STREQUAL "0" OR NOT out MATCHES "FX3 DEMO: PASS")
    message(FATAL_ERROR "godot.fx3_demo failed (exit ${rc}); shots in ${shots}")
endif()
message("godot.fx3_demo: PASS; shots in ${shots}")
