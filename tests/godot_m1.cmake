# ctest godot.m1 (docs/design/m1.md §1 E1-E3, §14): the headless M1 check of the Godot project
# ("<godot> --headless --path bf3-opensource res://scenes/m1_metro.tscn -- --m1-check", README "M1"): E1 through the
# loopback server on per-tick Trace reports, E2 under "150/2" (the gate) and "lat=150-165,loss=2" (reported) judged by
# host::judge_rest, E3 SpawnSweep with 128 slots against in-process settle_spawn rows, install unchanged. Passes when
# it prints "M1 CHECK: PASS" and exits 0. A fresh scratch cache root under the system temp directory (never the repo,
# never user://bf3cache), removed afterwards. Exits 77 (skipped) when no Godot console executable is found
# (godot_common.cmake) or no BF3 install is found. Needs the extension built.
# -DPHASE=e1|e2_gate|e2_stress runs one shard (--m1-phase; the rows godot.m1.<phase>), each on its own cache root.
# Script mode: cmake -DPROJECT_DIR=<bf3-opensource> [-DPHASE=<phase>] -P godot_m1.cmake (cmake_language(EXIT) needs
# CMake 3.29).
cmake_minimum_required(VERSION 3.29)
include(${CMAKE_CURRENT_LIST_DIR}/godot_common.cmake)
string(RANDOM LENGTH 8 ALPHABET 0123456789abcdef tag)
set(root "${BUILD_DIR}/scratch/godot-m1-${tag}")
file(MAKE_DIRECTORY "${BUILD_DIR}/scratch")
set(phase_args "")
if(DEFINED PHASE AND NOT PHASE STREQUAL "")
    set(phase_args "--m1-phase=${PHASE}")
endif()
file(REMOVE_RECURSE "${root}")
execute_process(
    COMMAND "${godot}" --headless ${godot_log_args} --path "${PROJECT_DIR}" res://scenes/m1_metro.tscn -- --m1-check "--bf3-cache=${root}" ${phase_args}
    TIMEOUT 580
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err)
file(REMOVE_RECURSE "${root}")
message("${out}")
if(NOT err STREQUAL "")
    message("${err}")
endif()
if(out MATCHES "no usable install" OR err MATCHES "no usable install")
    message("SKIPPED: no BF3 install found")
    cmake_language(EXIT 77)
endif()
if(NOT rc STREQUAL "0" OR NOT out MATCHES "M1 CHECK: PASS")
    message(FATAL_ERROR "${ROW} failed (exit ${rc})")
endif()
