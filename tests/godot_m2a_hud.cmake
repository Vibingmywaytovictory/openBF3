# M2a hud ctest godot.m2a_hud (docs/design/m2a.md §1 E10; docs/design/m2a-hud.md §5): the windowed minimal-HUD check:
#   <godot> --path bf3-opensource res://scenes/m2a_hud.tscn -- --hud-check --hud-shots=<OPENBF3_HUD_SHOTS>
# Rows (hud_check.h): load.hud, load.fonts, load.hit_art, load.static, then per resolution (1280x720, 1920x1080,
# 2560x1440) e10.<size>.frames / .ammo / .crosshair / .hit / .name / .icon / .health / .pixels / .shots, and
# install.unchanged. Passes when the run prints "M2A HUD CHECK: PASS" and exits 0.
# OPENBF3_HUD_SHOTS: the directory for the PNGs (default C:/Users/olku/BF3 reimplementation/openBF3/b/shots/hud,
# gitignored; never a path in the repo: the shots show BF3's fonts and icons).
# Skips (exit 77) like godot.ui2: OPENBF3_GODOT unset, no BF3 install, or a probe run without a window / rendering
# device.
# Script mode: cmake -DPROJECT_DIR=<bf3-opensource> -P godot_m2a_hud.cmake (cmake_language(EXIT) needs CMake 3.29).
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
set(shots "$ENV{OPENBF3_HUD_SHOTS}")
if(shots STREQUAL "")
    set(shots "C:/Users/olku/BF3 reimplementation/openBF3/b/shots/hud")
endif()
execute_process(
    COMMAND "${godot}" --path "${PROJECT_DIR}" res://scenes/m2a_hud.tscn -- --hud-check "--hud-shots=${shots}"
    TIMEOUT 480
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
if(NOT rc STREQUAL "0" OR NOT out MATCHES "M2A HUD CHECK: PASS")
    message(FATAL_ERROR "godot.m2a_hud failed (exit ${rc})")
endif()
