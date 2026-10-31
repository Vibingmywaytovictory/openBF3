cmake_minimum_required(VERSION 3.29)
include(${CMAKE_CURRENT_LIST_DIR}/godot_common.cmake)
godot_run(SKIP_WITHOUT_INSTALL TIMEOUT 120
    ARGS --headless ${godot_log_args} --path "${PROJECT_DIR}" res://scenes/ui_open_census.tscn -- ${godot_cache_args})
godot_require_pass("UI OPEN CENSUS: PASS")
