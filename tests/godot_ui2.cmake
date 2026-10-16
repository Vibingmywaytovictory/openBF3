# UI ui2 ctest godot.ui2 (docs/design/ui-minimap-icons.md §1 G9-G11, G13, G14, §10.4): the windowed in-game minimap
# check of the M1 game:
#   <godot> --path bf3-opensource res://scenes/m1_metro.tscn -- --minimap-check --minimap-mode=ConquestSmall0
#           [--minimap-shots=<OPENBF3_UI2_SHOTS>]
# Rows (minimap_check.h): minimap.loaded (G14), readback.calibration, marker.spawn, map.uv.north_up, map.uv.rotating,
# marker.pixels (G9), icons.minimap, icons.world (G10), shots (G11, only with OPENBF3_UI2_SHOTS), install.unchanged
# (G13). Passes when the run prints "UI2 MINIMAP CHECK: PASS" and exits 0. The session uses its default cache root
# (user://bf3cache), so a first run converts Metro cold (about 15 s, godot.m1_load E4).
# OPENBF3_UI2_SHOTS: a directory for the six PNGs (the acceptance run uses
# C:\Users\olku\BF3 reimplementation\openBF3\b\shots\ui2\, gitignored; never a path in the repo).
# Skips (exit 77) like godot.m1_load: OPENBF3_GODOT unset, no BF3 install, or a probe run without a window / rendering
# device.
# Script mode: cmake -DPROJECT_DIR=<bf3-opensource> -P godot_ui2.cmake (cmake_language(EXIT) needs CMake 3.29).
cmake_minimum_required(VERSION 3.29)
set(godot "$ENV{OPENBF3_GODOT}")
if(godot STREQUAL "" OR NOT EXISTS "${godot}")
    message("SKIPPED: OPENBF3_GODOT does not name the Godot console executable")
    cmake_language(EXIT 77)
endif()
if(NOT EXISTS "${PROJECT_DIR}/.godot/extension_list.cfg")
    file(WRITE "${PROJECT_DIR}/.godot/extension_list.cfg" "res://openbf3.gdextension\n")
endif()
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
set(args --minimap-check --minimap-mode=ConquestSmall0)
if(NOT "$ENV{OPENBF3_UI2_SHOTS}" STREQUAL "")
    list(APPEND args "--minimap-shots=$ENV{OPENBF3_UI2_SHOTS}")
endif()
execute_process(
    COMMAND "${godot}" --path "${PROJECT_DIR}" res://scenes/m1_metro.tscn -- ${args}
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
if(NOT rc STREQUAL "0" OR NOT out MATCHES "UI2 MINIMAP CHECK: PASS")
    message(FATAL_ERROR "godot.ui2 failed (exit ${rc})")
endif()
