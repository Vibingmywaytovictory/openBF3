cmake_minimum_required(VERSION 3.29)
include(${CMAKE_CURRENT_LIST_DIR}/godot_common.cmake)
string(RANDOM LENGTH 8 ALPHABET 0123456789abcdef tag)
set(root "${BUILD_DIR}/scratch/godot-settings-${tag}")
file(REMOVE_RECURSE "${root}")
file(MAKE_DIRECTORY "${root}")
godot_run(SKIP_WITHOUT_INSTALL TIMEOUT 200 REMOVE_ON_SKIP "${root}"
    ARGS --headless ${godot_log_args} --path "${PROJECT_DIR}" res://scenes/settings.tscn -- ${godot_cache_args}
    "--settings-file=${root}/settings.cfg")
file(REMOVE_RECURSE "${root}")
godot_require_pass("SETTINGS CHECK: PASS")
