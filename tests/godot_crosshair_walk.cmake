cmake_minimum_required(VERSION 3.29)
set(GODOT_WINDOWED TRUE)
include(${CMAKE_CURRENT_LIST_DIR}/godot_common.cmake)
set(cache_args "")
if(DEFINED BUILD_DIR AND NOT BUILD_DIR STREQUAL "")
    file(MAKE_DIRECTORY "${BUILD_DIR}/godot-cache/godot.crosshair_walk")
    set(cache_args "--bf3-cache=${BUILD_DIR}/godot-cache/godot.crosshair_walk")
endif()
godot_run(SKIP_WITHOUT_INSTALL TIMEOUT 115
    ARGS --path "${PROJECT_DIR}" --resolution 1280x720 res://scenes/metro_tdm.tscn -- --bots=0 --deploy-screen=0 --preround=0 --min-players=1
        --crosshair-walk ${cache_args})
godot_require_pass("CROSSHAIR WALK: PASS")
godot_require_pass("\\[level\\] install unchanged: yes")
