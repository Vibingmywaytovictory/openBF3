# ctest godot.fx_ref (docs/design/m2a-fx2.md §7, §8; E9 item 8): the Godot reference workloads of research gap-r2-vfx
# §6.2 re-created next to VFX2's 16k-particle row, windowed 1920 x 1080, vsync off:
#   <godot> --path bf3-opensource res://scenes/fx_ref.tscn -- --fx-ref-check
# Every row is measured with the S3 G1b method (alternating 5 s on / off windows, cost = p50 GPU on - p50 GPU off).
# Passes when the run prints "FX REF: PASS" (VFX2's cost <= 1.25 x the MultiMesh 16k lit+soft row's) and exits 0. Needs
# no install. Run it under the quiet lock (shared machine: GPU times are noisy). Skips (exit 77) when no Godot console
# executable is found or the windowed row is not opted in (godot_common.cmake), or no window / rendering device can be
# created.
# Script mode: cmake -DPROJECT_DIR=<bf3-opensource> -P godot_fx_ref.cmake (cmake_language(EXIT) needs CMake 3.29).
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
execute_process(
    COMMAND "${godot}" --path "${PROJECT_DIR}" res://scenes/fx_ref.tscn -- --fx-ref-check
    TIMEOUT 900
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err)
message("${out}")
if(NOT err STREQUAL "")
    message("${err}")
endif()
if(NOT rc STREQUAL "0" OR NOT out MATCHES "FX REF: PASS")
    message(FATAL_ERROR "godot.fx_ref failed (exit ${rc})")
endif()
message("godot.fx_ref: PASS")
