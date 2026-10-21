cmake_minimum_required(VERSION 3.29)
include(${CMAKE_CURRENT_LIST_DIR}/godot_common.cmake)
godot_run(SKIP_WITHOUT_INSTALL TIMEOUT 300
    ARGS --headless ${godot_log_args} --path "${PROJECT_DIR}" res://scenes/metro_collision.tscn -- --collision-check)
godot_require_pass("COLLISION CHECK: PASS")
