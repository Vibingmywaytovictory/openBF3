cmake_minimum_required(VERSION 3.29)
include(${CMAKE_CURRENT_LIST_DIR}/godot_common.cmake)
set(cache_args "")
if(NOT godot_cache_root STREQUAL "")
    set(cache_args "--bf3-cache=${godot_cache_root}")
endif()
execute_process(
    COMMAND "${godot}" --headless ${godot_log_args} --path "${PROJECT_DIR}" res://scenes/m2a_viewmodel.tscn -- --vm-check ${cache_args}
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
if(NOT rc STREQUAL "0" OR NOT out MATCHES "M2A VIEWMODEL CHECK: PASS")
    message(FATAL_ERROR "godot.m2a_viewmodel failed (exit ${rc})")
endif()
