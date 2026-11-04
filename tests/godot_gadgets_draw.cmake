cmake_minimum_required(VERSION 3.29)
set(GODOT_WINDOWED TRUE)
include(${CMAKE_CURRENT_LIST_DIR}/godot_common.cmake)
set(cache_args "")
if(DEFINED BUILD_DIR AND NOT BUILD_DIR STREQUAL "")
    file(MAKE_DIRECTORY "${BUILD_DIR}/godot-cache/godot.gadgets_draw")
    set(cache_args "--bf3-cache=${BUILD_DIR}/godot-cache/godot.gadgets_draw")
endif()
godot_run(SKIP_WITHOUT_INSTALL TIMEOUT 110
    ARGS --path "${PROJECT_DIR}" --resolution 1280x720 res://scenes/metro_tdm.tscn -- --bots=0 --deploy-screen=1 --preround=0 --min-players=1
        --gadgets-draw ${cache_args})
godot_require_pass("GADGETS DRAW: PASS")
godot_require_pass("\\[level\\] install unchanged: yes")
