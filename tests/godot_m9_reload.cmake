# ctest godot.m9_reload: runs the headless M9 reload check of the Godot project
# ("<godot> --headless --path bf3-opensource res://scenes/m9_reload.tscn -- --m9-reload-check") and passes when it
# prints "M9 RELOAD CHECK: PASS" and exits 0. Exits 77 (skipped) when no Godot console executable is found
# (godot_common.cmake) or no BF3 install is found. Needs the extension built and the project imported once (README "Run").
# Script mode: cmake -DPROJECT_DIR=<bf3-opensource> -P godot_m9_reload.cmake (cmake_language(EXIT) needs CMake 3.29).
cmake_minimum_required(VERSION 3.29)
include(${CMAKE_CURRENT_LIST_DIR}/godot_common.cmake)
# The per-build cache root of godot_common.cmake (warm across runs); a direct cmake -P run uses user://bf3cache.
set(cache_args "")
if(NOT godot_cache_root STREQUAL "")
    set(cache_args "--bf3-cache=${godot_cache_root}")
endif()
execute_process(
    COMMAND "${godot}" --headless ${godot_log_args} --path "${PROJECT_DIR}" res://scenes/m9_reload.tscn -- --m9-reload-check ${cache_args}
    TIMEOUT 240
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
if(NOT rc STREQUAL "0" OR NOT out MATCHES "M9 RELOAD CHECK: PASS")
    message(FATAL_ERROR "godot.m9_reload failed (exit ${rc})")
endif()
