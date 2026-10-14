# ctest godot.m9_reload: runs the headless M9 reload check of the Godot project
# ("<godot> --headless --path bf3-opensource res://scenes/m9_reload.tscn -- --m9-reload-check") and passes when it
# prints "M9 RELOAD CHECK: PASS" and exits 0. Exits 77 (skipped) when OPENBF3_GODOT does not name the Godot console
# executable or no BF3 install is found. Needs the extension built and the project imported once (README "Run").
# Script mode: cmake -DPROJECT_DIR=<bf3-opensource> -P godot_m9_reload.cmake (cmake_language(EXIT) needs CMake 3.29).
cmake_minimum_required(VERSION 3.29)
set(godot "$ENV{OPENBF3_GODOT}")
if(godot STREQUAL "" OR NOT EXISTS "${godot}")
    message("SKIPPED: OPENBF3_GODOT does not name the Godot console executable")
    cmake_language(EXIT 77)
endif()
execute_process(
    COMMAND "${godot}" --headless --path "${PROJECT_DIR}" res://scenes/m9_reload.tscn -- --m9-reload-check
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
