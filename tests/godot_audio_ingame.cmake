cmake_minimum_required(VERSION 3.29)
set(GODOT_WINDOWED TRUE)
include(${CMAKE_CURRENT_LIST_DIR}/godot_common.cmake)
set(out "${BUILD_DIR}/shots/audio_ingame")
file(REMOVE_RECURSE "${out}")
file(MAKE_DIRECTORY "${out}" "${BUILD_DIR}/godot-cache/godot.audio_ingame")
godot_run(SKIP_WITHOUT_INSTALL TIMEOUT 420
    ARGS --path "${PROJECT_DIR}" res://scenes/metro_tdm.tscn -- --bots=31 --preround=0 "--audio-ingame-check=${out}"
        "--bf3-cache=${BUILD_DIR}/godot-cache/godot.audio_ingame")
godot_require_pass("AUDIO INGAME: PASS")
godot_require_pass("\\[level\\] install unchanged: yes")
