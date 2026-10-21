cmake_minimum_required(VERSION 3.29)
set(GODOT_WINDOWED TRUE)
include(${CMAKE_CURRENT_LIST_DIR}/godot_common.cmake)
set(shots "$ENV{OPENBF3_FX3_SHOT_DIR}")
if(shots STREQUAL "")
    set(shots "${BUILD_DIR}/shots/godot.fx3_demo")
endif()
set(root "${BUILD_DIR}/scratch/godot.fx3_demo/bf3cache")
file(MAKE_DIRECTORY "${root}")
godot_run(SKIP_WITHOUT_INSTALL TIMEOUT 600
    ARGS --path "${PROJECT_DIR}" res://scenes/fx3_demo.tscn -- --fx3-check --walkers=0 --no-hud "--bf3-cache=${root}"
        "--fx3-shot-dir=${shots}" --quit-after=6000)
godot_require_pass("FX3 DEMO: PASS" "; shots in ${shots}")
message("${ROW}: PASS; shots in ${shots}")
