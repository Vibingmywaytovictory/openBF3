cmake_minimum_required(VERSION 3.29)
set(GODOT_WINDOWED TRUE)
include(${CMAKE_CURRENT_LIST_DIR}/godot_common.cmake)
set(cache "${BUILD_DIR}/godot-cache/${ROW}")
set(shots "${BUILD_DIR}/shots/${ROW}")
file(MAKE_DIRECTORY "${cache}" "${shots}")
godot_run(SKIP_WITHOUT_INSTALL TIMEOUT 200 HEADER "MP_007 ConquestSmall0"
    ARGS --path "${PROJECT_DIR}" res://scenes/metro_tdm.tscn -- --level=MP_007 --mode=ConquestSmall0
        --conquest-hud "--conquest-hud-shot=${shots}/conquest_hud.png" --bots=31 --preround=0 "--bf3-cache=${cache}")
godot_require_pass("CONQUEST HUD: PASS")
godot_require_pass("\\[level\\] install unchanged: yes")
