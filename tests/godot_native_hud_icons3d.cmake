cmake_minimum_required(VERSION 3.29)
set(GODOT_WINDOWED TRUE)
include(${CMAKE_CURRENT_LIST_DIR}/godot_common.cmake)
set(cache "${BUILD_DIR}/godot-cache/${ROW}")
file(MAKE_DIRECTORY "${cache}")
godot_run(SKIP_WITHOUT_INSTALL TIMEOUT 110
    ARGS --path "${PROJECT_DIR}" --resolution 1280x720 res://scenes/metro_tdm.tscn -- --bots=0 --deploy-screen=0 --preround=0 --min-players=1
        --icons3d-check "--bf3-cache=${cache}")
godot_require_pass("ICONS3D CHECK: PASS")
godot_require_pass("\\[level\\] install unchanged: yes")
