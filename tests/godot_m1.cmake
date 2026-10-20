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
