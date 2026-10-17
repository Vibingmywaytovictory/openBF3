# ctest godot.collision: runs the headless collision overlay check of the Godot project
# ("<godot> --headless --path bf3-opensource res://scenes/metro_collision.tscn -- --collision-check", README
# "Collision overlay") and passes when it prints "COLLISION CHECK: PASS" and exits 0. Exits 77 (skipped) when
# no Godot console executable is found (godot_common.cmake) or no BF3 install is found. Needs the extension built.
# Script mode: cmake -DPROJECT_DIR=<bf3-opensource> -P godot_collision.cmake (cmake_language(EXIT) needs CMake 3.29).
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
