# ctest godot.m2a_viewmodel (M2a vm, docs/design/m2a-vm.md): runs the headless viewmodel check of the Godot project
# ("<godot> --headless --path bf3-opensource res://scenes/m2a_viewmodel.tscn -- --vm-check") and passes when it prints
# "M2A VIEWMODEL CHECK: PASS" and exits 0: E19 (FOV, FOV times, recoil, springs, zoom refusal), the E5 viewmodel row
# and E11's viewmodel coverage on the 8 default loadouts, every display frame at 60 and 144 fps. Exits 77 (skipped)
# when no Godot console executable is found (godot_common.cmake) or no BF3 install is found. A ctest run passes the
# per-build cache root of godot_common.cmake (warm across runs); a direct cmake -P run uses user://bf3cache.
# Script mode: cmake -DPROJECT_DIR=<bf3-opensource> -P godot_m2a_viewmodel.cmake (cmake_language(EXIT) needs CMake 3.29).
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
