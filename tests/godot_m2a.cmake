# M2a gx ctest rows godot.m2a, godot.m2a.kits and godot.m2a.play (docs/design/m2a.md §1 E1, E3, E5, E10, E11, E19;
# docs/design/m2a-gx.md §4, §5): the windowed in-game check of the playable scene,
#   <godot> --path bf3-opensource res://scenes/m2a_metro.tscn -- --m2a-check=<CASES> --m2a-shots=<dir>
#           --bf3-cache=<BUILD_DIR>/godot-cache/godot.m2a [ARGS...]
# -DCASES   the cases (m2a_check.h; default "range,reload,hud,ads,audio,fx"); godot.m2a.kits passes "kits",
#           godot.m2a.play "play" (the scripted 5-minute run, -DARGS=--respawn=3)
# -DNO_ERRORS=1   also fail on any "ERROR" line in the output (the play gate: 0 errors in the log)
# Rows: "PASS|FAIL <case>.<row> <value> <rule>" and "SKIP <case> <reason>" (a case whose track has not landed:
# audio waits for au3, fx for fx3); passes when the run prints "M2A CHECK: PASS" and exits 0.
# Screenshots: $ENV{OPENBF3_GX_SHOTS}, default <build dir>/shots/gx (gitignored,
# outside the worktree: the shots show BF3's art). The cache root is shared by the godot.m2a* rows (warm after the
# first run; none of them measures a load: E16 is godot.m2a_load's).
# Skips (exit 77) like godot.m2a_hud: no Godot console executable or no window opt-in (godot_common.cmake), no BF3
# install, or a probe run without a window / rendering device.
# Script mode: cmake -DPROJECT_DIR=<bf3-opensource> [-DBUILD_DIR=<b/dev>] [-DCASES=...] -P godot_m2a.cmake
cmake_minimum_required(VERSION 3.29)
set(GODOT_WINDOWED TRUE)
include(${CMAKE_CURRENT_LIST_DIR}/godot_common.cmake)
if(NOT DEFINED CASES OR CASES STREQUAL "")
    set(CASES "range,reload,hud,ads,audio,fx")
endif()
execute_process(
    COMMAND "${godot}" --path "${PROJECT_DIR}" --quit
    TIMEOUT 60
    RESULT_VARIABLE probe_rc
    OUTPUT_VARIABLE probe_out
    ERROR_VARIABLE probe_err)
if(NOT probe_rc STREQUAL "0")
    message("${probe_out}")
    message("${probe_err}")
    message("SKIPPED: no window or rendering device (probe exit '${probe_rc}')")
    cmake_language(EXIT 77)
endif()
set(shots "$ENV{OPENBF3_GX_SHOTS}")
if(shots STREQUAL "")
    set(shots "<build dir>/shots/gx")
endif()
set(cache_args "")
if(DEFINED BUILD_DIR AND NOT BUILD_DIR STREQUAL "")
    file(MAKE_DIRECTORY "${BUILD_DIR}/godot-cache/godot.m2a")
    set(cache_args "--bf3-cache=${BUILD_DIR}/godot-cache/godot.m2a")
endif()
set(timeout 900)
if(CASES MATCHES "play")
    set(timeout 1500)
endif()
separate_arguments(extra NATIVE_COMMAND "${ARGS}")
execute_process(
    COMMAND "${godot}" --path "${PROJECT_DIR}" res://scenes/m2a_metro.tscn -- "--m2a-check=${CASES}" "--m2a-shots=${shots}" ${cache_args} ${extra}
    TIMEOUT ${timeout}
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err)
message("${out}")
if(NOT err STREQUAL "")
    message("${err}")
endif()
if(out MATCHES "no usable install" OR err MATCHES "no usable install")
    message("SKIPPED: no BF3 install found")
    cmake_language(EXIT 77)
endif()
string(REGEX MATCHALL "[^\n]*ERROR[^\n]*" error_lines "${out}\n${err}")
list(LENGTH error_lines error_count)
message("ERROR lines in the log: ${error_count}")
if(NO_ERRORS AND error_count GREATER 0)
    message(FATAL_ERROR "${ROW}: ${error_count} ERROR line(s) in the log")
endif()
if(NOT rc STREQUAL "0" OR NOT out MATCHES "M2A CHECK: PASS")
    message(FATAL_ERROR "${ROW} failed (exit ${rc})")
endif()
if(NOT out MATCHES "\nPASS ")
    # Every case skipped (the tracks it waits for have not landed): nothing was proven.
    message("SKIPPED: every case skipped")
    cmake_language(EXIT 77)
endif()
