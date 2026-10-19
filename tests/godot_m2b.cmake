# M2b gx ctest row godot.m2b.play (docs/design/m2b.md §1 E9; docs/design/m2b-gx.md §4, §5): the windowed in-game check
# of the M2b scene, a scripted Metro TDM round against the TDM bots,
#   <godot> --path bf3-opensource res://scenes/m2b_metro.tscn -- --m2b-check=play --m2b-shots=<dir>
#           --bf3-cache=<BUILD_DIR>/godot-cache/godot.m2b [ARGS...]
# -DARGS   more user args (the row passes "--kill-limit=15 --preround=0", so the round ends in about a minute)
# Rows: "PASS|FAIL|SKIP play.<row> <value> <rule>[ (<skip reason>)]"; passes when the run prints "M2B CHECK: PASS",
# exits 0 and prints no ERROR line. A row whose track has not landed is skipped "(waiting for ui)" / "(waiting for
# cam)"; with OPENBF3_TIMING_REQUIRE_RUN=1 (the merge pass) such a skip fails the row, since every track is merged by
# then. Screenshots: $ENV{OPENBF3_GX_SHOTS}, default C:/Users/olku/BF3 reimplementation/openBF3/b/shots/gx (gitignored,
# outside the worktree: the shots show BF3's art). The cache root is the row's own and warm after the first run (E9
# does not measure a load: E12 is godot.m2b_load's).
# Skips (exit 77) like godot.m2a: no Godot console executable or no window opt-in (godot_common.cmake), no BF3 install,
# or a probe run without a window / rendering device.
# Script mode: cmake -DPROJECT_DIR=<bf3-opensource> [-DBUILD_DIR=<b/dev>] [-DARGS=...] -P godot_m2b.cmake
cmake_minimum_required(VERSION 3.29)
set(GODOT_WINDOWED TRUE)
include(${CMAKE_CURRENT_LIST_DIR}/godot_common.cmake)
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
    set(shots "C:/Users/olku/BF3 reimplementation/openBF3/b/shots/gx")
endif()
set(cache_args "")
if(DEFINED BUILD_DIR AND NOT BUILD_DIR STREQUAL "")
    file(MAKE_DIRECTORY "${BUILD_DIR}/godot-cache/godot.m2b")
    set(cache_args "--bf3-cache=${BUILD_DIR}/godot-cache/godot.m2b")
endif()
separate_arguments(extra NATIVE_COMMAND "${ARGS}")
execute_process(
    COMMAND "${godot}" --path "${PROJECT_DIR}" res://scenes/m2b_metro.tscn -- "--m2b-check=play" "--m2b-shots=${shots}" ${cache_args} ${extra}
    TIMEOUT 540
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
if(error_count GREATER 0)
    message(FATAL_ERROR "${ROW}: ${error_count} ERROR line(s) in the log")
endif()
if(NOT rc STREQUAL "0" OR NOT out MATCHES "M2B CHECK: PASS")
    message(FATAL_ERROR "${ROW} failed (exit ${rc})")
endif()
string(REGEX MATCHALL "SKIP [^\n]*waiting for [^\n]*" waiting "${out}")
list(LENGTH waiting waiting_count)
if(waiting_count GREATER 0)
    if("$ENV{OPENBF3_TIMING_REQUIRE_RUN}" STREQUAL "1")
        message(FATAL_ERROR "${ROW}: ${waiting_count} row(s) still waiting for a track in the merge pass")
    endif()
    message("${ROW}: ${waiting_count} row(s) waiting for a track (ui / cam not merged yet)")
endif()
