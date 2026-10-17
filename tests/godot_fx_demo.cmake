# ctest godot.fx_demo (docs/design/m2a-fx2.md §6, §8): the windowed VFX2 demo check of the Godot project on one fresh
# scratch cache root:
#   <godot> --path bf3-opensource res://scenes/fx_demo.tscn --
#       --fx-check --walkers=0 --no-hud --bf3-cache=<fresh scratch root> --fx-shot-dir=<shots> --quit-after=900
# Rows: E8 (3) drawn instances == FxWorld::particles() every frame, E8 (6) atlas 52 / 52 entries and 0 default-texture
# fallbacks, E8 (4) the oilfire draws 80 +- 1 at 15 s, effect lights <= 64 every frame (under a > 64 stress), the 1p
# muzzle pass, the FX main-thread time (printed), captures and install unchanged. Passes when the run prints
# "FX DEMO: PASS" and exits 0. Captures go to $ENV{OPENBF3_FX2_SHOT_DIR} when set, else <checkout>/b/shots/fx2
# (gitignored /b/; BF3 imagery is never committed). Skips (exit 77) when no Godot console executable is found or the
# windowed row is not opted in (godot_common.cmake), no window / rendering device can be created (a windowed --quit
# probe, killed after 60 s) or no BF3 install is found.
# Script mode: cmake -DPROJECT_DIR=<bf3-opensource> -P godot_fx_demo.cmake (cmake_language(EXIT) needs CMake 3.29).
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
set(shots "$ENV{OPENBF3_FX2_SHOT_DIR}")
if(shots STREQUAL "")
    get_filename_component(shots "${PROJECT_DIR}/../b/shots/fx2" ABSOLUTE)
endif()
string(RANDOM LENGTH 8 ALPHABET 0123456789abcdef tag)
set(root "$ENV{TEMP}/openbf3-godot-fx2-${tag}")
file(REMOVE_RECURSE "${root}")
execute_process(
    COMMAND "${godot}" --path "${PROJECT_DIR}" res://scenes/fx_demo.tscn -- --fx-check --walkers=0 --no-hud "--bf3-cache=${root}"
        "--fx-shot-dir=${shots}" --quit-after=900
    TIMEOUT 1200
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
if(NOT rc STREQUAL "0" OR NOT out MATCHES "FX DEMO: PASS")
    message(FATAL_ERROR "godot.fx_demo failed (exit ${rc}); shots in ${shots}")
endif()
message("godot.fx_demo: PASS; shots in ${shots}")
