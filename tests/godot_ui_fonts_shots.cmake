# ctest godot.ui_fonts_shots (docs/design/ui-swf-fonts.md §1 G5, §11.2): the ui1 check windowed with screenshots
# ("<godot> --path bf3-opensource res://scenes/ui_fonts.tscn -- --ui-check --shot-dir=<dir>"): passes when the check
# prints "UI1 CHECK: PASS", exits 0 and <dir>/ui1_fonts.png, ui1_coverage.png and ui1_icons.png exist. <dir> is
# $ENV{OPENBF3_UI1_SHOTS}, default <build dir>\shots\ui1 (absolute, gitignored,
# outside any worktree; the shots show BF3 data and are never committed). The implementer inspects them (README).
# Skips (exit 77) when no Godot console executable is found or the windowed row is not opted in (godot_common.cmake), no BF3 install is found, or no window / rendering device can be created:
# a probe run ("--path <project> --quit", windowed, killed after 60 s) decides first, as godot_m1_load.cmake does,
# because Godot's failure to create a rendering device on Windows raises a blocking OS alert.
# Script mode: cmake -DPROJECT_DIR=<bf3-opensource> -P godot_ui_fonts_shots.cmake (cmake_language(EXIT) needs CMake 3.29).
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
set(shots "$ENV{OPENBF3_UI1_SHOTS}")
if(shots STREQUAL "")
    set(shots "${BUILD_DIR}/shots/ui1")
endif()
file(TO_CMAKE_PATH "${shots}" shots)
file(MAKE_DIRECTORY "${shots}")
foreach(png ui1_fonts.png ui1_coverage.png ui1_icons.png)
    file(REMOVE "${shots}/${png}")
endforeach()
execute_process(
    COMMAND "${godot}" --path "${PROJECT_DIR}" res://scenes/ui_fonts.tscn -- --ui-check "--shot-dir=${shots}"
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
set(missing "")
foreach(png ui1_fonts.png ui1_coverage.png ui1_icons.png)
    if(NOT EXISTS "${shots}/${png}")
        string(APPEND missing " ${png}")
    endif()
endforeach()
if(NOT rc STREQUAL "0" OR NOT out MATCHES "UI1 CHECK: PASS" OR NOT missing STREQUAL "")
    message(FATAL_ERROR "godot.ui_fonts_shots failed (exit ${rc}; missing:${missing})")
endif()
message("shots: ${shots}")
