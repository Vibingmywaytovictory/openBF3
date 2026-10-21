cmake_minimum_required(VERSION 3.29)

include(${CMAKE_CURRENT_LIST_DIR}/godot_common.cmake)
if(NOT DEFINED TIMEOUT)
    set(TIMEOUT 240)
endif()

godot_run(TIMEOUT ${TIMEOUT} ARGS --headless ${godot_log_args} --path "${PROJECT_DIR}" -- ${USER_ARGS})
godot_require_pass("${PASS_LINE}")
