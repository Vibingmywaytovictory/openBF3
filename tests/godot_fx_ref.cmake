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
