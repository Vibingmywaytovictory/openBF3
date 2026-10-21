cmake_minimum_required(VERSION 3.29)
include(${CMAKE_CURRENT_LIST_DIR}/godot_common.cmake)
godot_run(SKIP_WITHOUT_INSTALL TIMEOUT 400
    ARGS --headless ${godot_log_args} --path "${PROJECT_DIR}" res://scenes/m2a_tp_crowd.tscn -- --tp-check --walkers=0 --no-hud ${godot_cache_args})
godot_require_pass("M2A TP CROWD CHECK: PASS")
