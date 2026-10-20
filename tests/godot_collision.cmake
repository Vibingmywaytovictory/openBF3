cmake_minimum_required(VERSION 3.29)
include(${CMAKE_CURRENT_LIST_DIR}/godot_common.cmake)
execute_process(
    COMMAND "${godot}" --headless ${godot_log_args} --path "${PROJECT_DIR}" res://scenes/metro_collision.tscn -- --collision-check
    TIMEOUT 300
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
if(NOT rc STREQUAL "0" OR NOT out MATCHES "COLLISION CHECK: PASS")
    message(FATAL_ERROR "godot.collision failed (exit ${rc})")
endif()
