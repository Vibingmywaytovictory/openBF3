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
