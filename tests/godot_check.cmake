# Runs the Godot project headless with user args and checks for a PASS line (ctest godot.*).
#   cmake -DPROJECT_DIR=<bf3-opensource> -DUSER_ARGS=<;-list> -DPASS_LINE=<text> [-DTIMEOUT=<s>] -P godot_check.cmake
# The Godot console executable comes from godot_common.cmake (OPENBF3_GODOT, else the configure-time
# OPENBF3_GODOT_EXE); without one the test exits 77 (skipped). The godot.project_setup fixture has written the one-line
# .godot/extension_list.cfg (README "Run": avoids the crash of a first --import).
# Needs CMake >= 3.29 (cmake_language(EXIT)).
cmake_minimum_required(VERSION 3.29)

include(${CMAKE_CURRENT_LIST_DIR}/godot_common.cmake)
if(NOT DEFINED TIMEOUT)
    set(TIMEOUT 240)
endif()

execute_process(
    COMMAND "${godot}" --headless ${godot_log_args} --path "${PROJECT_DIR}" -- ${USER_ARGS}
    RESULT_VARIABLE code
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err
    TIMEOUT ${TIMEOUT})
message("${out}")
if(NOT "${err}" STREQUAL "")
    message("${err}")
endif()
string(FIND "${out}" "${PASS_LINE}" at)
if(NOT code EQUAL 0 OR at EQUAL -1)
    message("FAILED: exit '${code}', '${PASS_LINE}' ${at}")
    cmake_language(EXIT 1)
endif()
