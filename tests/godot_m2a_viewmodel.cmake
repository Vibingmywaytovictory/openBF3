cmake_minimum_required(VERSION 3.29)
include(${CMAKE_CURRENT_LIST_DIR}/godot_common.cmake)
godot_run(SKIP_WITHOUT_INSTALL TIMEOUT 400
    ARGS --headless ${godot_log_args} --path "${PROJECT_DIR}" res://scenes/m2a_viewmodel.tscn -- --vm-check ${godot_cache_args})
godot_require_pass("M2A VIEWMODEL CHECK: PASS")
