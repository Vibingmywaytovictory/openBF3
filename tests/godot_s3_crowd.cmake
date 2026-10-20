# ctest godot.s3_crowd (docs/design/s3.md §1, §12): the windowed S3 crowd check of the Godot project on one fresh
# scratch cache root:
#   <godot> --path bf3-opensource res://scenes/s3_crowd.tscn --
#       --s3-check --walkers=0 --no-hud --bf3-cache=<fresh scratch root> --s3-shot-dir=<shots> --quit-after=600
# G0 validity rows (stage, view, config, blend), G1a / G1b (main-thread animation cost), G2 (frame time, vsync off and
# on), G3 (16 shoulder shots + overview with pinch and hand-IK metrics) and install unchanged. Passes when the run
# prints "S3 CHECK: PASS" and exits 0.
# Noise policy (s3.md §1, fixed in advance): when only timing gates fail (the check prints "S3 TIMING: FAIL"), the whole
# check runs once more and decides; both runs are printed and the repeat is named. A G0 failure is not repeated.
# Shots go to $ENV{OPENBF3_S3_SHOT_DIR} when set, else <checkout>/b/shots/s3 (gitignored /b/; BF3 imagery is never
# committed). Skips (exit 77) when no Godot console executable is found or the windowed row is not opted in (godot_common.cmake), no window / rendering device can be created (the windowed
# --quit probe of godot_m1_load.cmake, killed after 60 s), no BF3 install is found ("no usable install"), or the check
# exits 77 because the primary screen is not 2560x1440.
# Script mode: cmake -DPROJECT_DIR=<bf3-opensource> -P godot_s3_crowd.cmake (cmake_language(EXIT) needs CMake 3.29).
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
set(shots "$ENV{OPENBF3_S3_SHOT_DIR}")
if(shots STREQUAL "")
    get_filename_component(shots "${PROJECT_DIR}/../b/shots/s3" ABSOLUTE)
endif()
set(attempt 0)
set(result "")
foreach(run first repeat)
    string(RANDOM LENGTH 8 ALPHABET 0123456789abcdef tag)
    set(root "${BUILD_DIR}/scratch/godot-s3-${tag}")
    file(MAKE_DIRECTORY "${BUILD_DIR}/scratch")
    file(REMOVE_RECURSE "${root}")
    execute_process(
        COMMAND "${godot}" --path "${PROJECT_DIR}" res://scenes/s3_crowd.tscn -- --s3-check --walkers=0 --no-hud "--bf3-cache=${root}"
            "--s3-shot-dir=${shots}" --quit-after=600
        TIMEOUT 900
        RESULT_VARIABLE rc
        OUTPUT_VARIABLE out
        ERROR_VARIABLE err)
    file(REMOVE_RECURSE "${root}")
    message("---- ${run} run ----")
    message("${out}")
    if(NOT err STREQUAL "")
        message("${err}")
    endif()
    if(out MATCHES "no usable install" OR err MATCHES "no usable install")
        message("SKIPPED: no BF3 install found")
        cmake_language(EXIT 77)
    endif()
    if(rc STREQUAL "77")
        message("SKIPPED: the primary screen is not 2560x1440 (the gate is defined at 1440p only)")
        cmake_language(EXIT 77)
    endif()
    if(rc STREQUAL "0" AND out MATCHES "S3 CHECK: PASS")
        if(run STREQUAL "repeat")
            set(result "PASS on the repeat (the first run failed timing gates only)")
        else()
            set(result "PASS")
        endif()
        break()
    endif()
    if(NOT out MATCHES "S3 TIMING: FAIL")
        set(result "FAIL (exit ${rc}; not a timing-only failure, so no repeat)")
        break()
    endif()
    set(result "FAIL (timing gates failed on both runs)")
endforeach()
message("godot.s3_crowd: ${result}; shots in ${shots}")
if(NOT result MATCHES "^PASS")
    message(FATAL_ERROR "godot.s3_crowd failed")
endif()
