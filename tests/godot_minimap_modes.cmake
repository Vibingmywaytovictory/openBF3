cmake_minimum_required(VERSION 3.29)
set(GODOT_WINDOWED TRUE)
include(${CMAKE_CURRENT_LIST_DIR}/godot_common.cmake)
set(cache "${BUILD_DIR}/godot-cache/${ROW}")
file(MAKE_DIRECTORY "${cache}")
godot_run(SKIP_WITHOUT_INSTALL TIMEOUT 180 HEADER "${LEVEL} ${MODE}"
    ARGS --path "${PROJECT_DIR}" res://scenes/metro_tdm.tscn -- "--level=${LEVEL}" --mode=TeamDeathMatch0
        "--minimap-mode=${MODE}" --minimap-modes --bots=31 --preround=0 "--bf3-cache=${cache}")
godot_require_pass("MINIMAP MODES ${MODE}: PASS")
godot_require_pass("\\[level\\] install unchanged: yes")
