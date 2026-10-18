# ctest godot.m2a_tp_crowd (M2a tp, docs/design/m2a-tp.md): runs tp's headless crowd check of the Godot project
# ("<godot> --headless --path bf3-opensource res://scenes/m2a_tp_crowd.tscn -- --tp-check --walkers=0 --no-hud") and
# passes when it prints "M2A TP CROWD CHECK: PASS" and exits 0: Bf3GameCrowd loaded as the level session loads it,
# built, and stepped through the scripted presented soldiers (drawn soldiers, weapon instances, muzzles and their
# effect frame, the death hide, the respawn, the weapon rebase, no frozen frame). Functional only (the load time is
# printed, never gated). Exits 77 (skipped) when no Godot console executable or no BF3 install is found. A ctest run
# passes the per-build cache root of godot_common.cmake; a direct cmake -P run uses user://bf3cache.
# Script mode: cmake -DPROJECT_DIR=<bf3-opensource> -P godot_m2a_tp_crowd.cmake (cmake_language(EXIT) needs CMake 3.29).
cmake_minimum_required(VERSION 3.29)
include(${CMAKE_CURRENT_LIST_DIR}/godot_common.cmake)
set(cache_args "")
if(NOT godot_cache_root STREQUAL "")
    set(cache_args "--bf3-cache=${godot_cache_root}")
endif()
execute_process(
    COMMAND "${godot}" --headless ${godot_log_args} --path "${PROJECT_DIR}" res://scenes/m2a_tp_crowd.tscn -- --tp-check --walkers=0 --no-hud ${cache_args}
    TIMEOUT 400
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
if(NOT rc STREQUAL "0" OR NOT out MATCHES "M2A TP CROWD CHECK: PASS")
    message(FATAL_ERROR "godot.m2a_tp_crowd failed (exit ${rc})")
endif()
