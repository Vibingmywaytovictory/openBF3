# Runs the Godot project headless with user args and checks for a PASS line (ctest godot.*).
#   cmake -DPROJECT_DIR=<bf3-opensource> -DUSER_ARGS=<;-list> -DPASS_LINE=<text> [-DTIMEOUT=<s>] -P godot_check.cmake
# The Godot console executable comes from the environment variable OPENBF3_GODOT; without it (or when
# it does not exist) the test exits 77 (skipped). On a project that was never imported, the one-line
# .godot/extension_list.cfg is written first (README "Run": avoids the crash of a first --import).
# Needs CMake >= 3.29 (cmake_language(EXIT)).

if(NOT DEFINED ENV{OPENBF3_GODOT} OR "$ENV{OPENBF3_GODOT}" STREQUAL "")
    message("OPENBF3_GODOT is not set: skipped")
    cmake_language(EXIT 77)
endif()
set(godot "$ENV{OPENBF3_GODOT}")
if(NOT EXISTS "${godot}")
    message("OPENBF3_GODOT=${godot} does not exist: skipped")
    cmake_language(EXIT 77)
endif()
if(NOT DEFINED TIMEOUT)
    set(TIMEOUT 240)
endif()
if(NOT EXISTS "${PROJECT_DIR}/.godot/extension_list.cfg")
    file(WRITE "${PROJECT_DIR}/.godot/extension_list.cfg" "res://openbf3.gdextension\n")
endif()

execute_process(
    COMMAND "${godot}" --headless --path "${PROJECT_DIR}" -- ${USER_ARGS}
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
