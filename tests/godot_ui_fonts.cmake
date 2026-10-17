# ctest godot.ui_fonts (docs/design/ui-swf-fonts.md §1 G4, §11.1): the headless ui1 check of the Godot project
# ("<godot> --headless --path bf3-opensource res://scenes/ui_fonts.tscn -- --ui-check"): BF3's fonts as FontFiles
# (set_data, glyphs, contours, metrics, kerning, MÉTRO), the font mapping, the iconsingame atlas, the HUD table and
# install unchanged. Passes when it prints "UI1 CHECK: PASS" and exits 0. Exits 77 (skipped) when no Godot console
# executable is found (godot_common.cmake) or no BF3 install is found. Needs the extension built.
# Script mode: cmake -DPROJECT_DIR=<bf3-opensource> -P godot_ui_fonts.cmake (cmake_language(EXIT) needs CMake 3.29).
cmake_minimum_required(VERSION 3.29)
include(${CMAKE_CURRENT_LIST_DIR}/godot_common.cmake)
# The per-build cache root of godot_common.cmake (warm across runs); a direct cmake -P run uses user://bf3cache.
set(cache_args "")
if(NOT godot_cache_root STREQUAL "")
    set(cache_args "--bf3-cache=${godot_cache_root}")
endif()
execute_process(
    COMMAND "${godot}" --headless ${godot_log_args} --path "${PROJECT_DIR}" res://scenes/ui_fonts.tscn -- --ui-check ${cache_args}
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
if(NOT rc STREQUAL "0" OR NOT out MATCHES "UI1 CHECK: PASS")
    message(FATAL_ERROR "godot.ui_fonts failed (exit ${rc})")
endif()
