# ctest godot.m1_load (docs/design/m1.md §1 E4/E5, §12, §14): the windowed M1 load check of the Godot project, cold
# then warm on one fresh scratch cache root:
#   <godot> --path bf3-opensource res://scenes/m1_metro.tscn -- --m1-load-check=cold --bf3-cache=<root> --shot-spawns=...
#   ... the same with --m1-load-check=warm
# E4 (cold <= 15 s) and E5 (warm <= 5 s) from process start to the first controllable frame, the per-phase work and
# wall times, the frame time after Done, the game's peak working set (reported), install unchanged, and one
# first-person shot at 8 sampled spawn points (written to the system temp directory, never the repo). Passes when
# both runs print "M1 LOAD CHECK: PASS" and exit 0.
# Skips (exit 77) when OPENBF3_GODOT is unset, no BF3 install is found, or no window / rendering device can be
# created: on Windows, Godot's failure to create a rendering device raises a blocking OS alert, so a probe run
# ("--path <project> --quit", windowed, killed after 60 s) decides first; only a clean probe starts the real runs
# (docs §14: an environment variable such as SESSIONNAME cannot tell a desktop session from a service).
# Acceptance needs this test to report Passed, not Skipped, in a desktop session.
# Script mode: cmake -DPROJECT_DIR=<bf3-opensource> -P godot_m1_load.cmake (cmake_language(EXIT) needs CMake 3.29).
cmake_minimum_required(VERSION 3.29)
set(godot "$ENV{OPENBF3_GODOT}")
if(godot STREQUAL "" OR NOT EXISTS "${godot}")
    message("SKIPPED: OPENBF3_GODOT does not name the Godot console executable")
    cmake_language(EXIT 77)
endif()
if(NOT EXISTS "${PROJECT_DIR}/.godot/extension_list.cfg")
    file(WRITE "${PROJECT_DIR}/.godot/extension_list.cfg" "res://openbf3.gdextension\n")
endif()
# The probe: a windowed start that quits at once.
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
string(RANDOM LENGTH 8 ALPHABET 0123456789abcdef tag)
set(root "$ENV{TEMP}/openbf3-godot-m1-load-${tag}")
set(shots "$ENV{TEMP}/openbf3-godot-m1-shots")
file(REMOVE_RECURSE "${root}")
file(MAKE_DIRECTORY "${shots}")
set(failed FALSE)
foreach(pass cold warm)
    execute_process(
        COMMAND "${godot}" --path "${PROJECT_DIR}" res://scenes/m1_metro.tscn -- "--m1-load-check=${pass}" "--bf3-cache=${root}"
            --shot-spawns=0,13,26,39,52,65,78,91 "--shot-dir=${shots}/${pass}"
        TIMEOUT 140
        RESULT_VARIABLE rc
        OUTPUT_VARIABLE out
        ERROR_VARIABLE err)
    message("---- ${pass} ----")
    message("${out}")
    if(NOT err STREQUAL "")
        message("${err}")
    endif()
    if(out MATCHES "no usable install" OR err MATCHES "no usable install")
        file(REMOVE_RECURSE "${root}")
        message("SKIPPED: no BF3 install found")
        cmake_language(EXIT 77)
    endif()
    if(NOT rc STREQUAL "0" OR NOT out MATCHES "M1 LOAD CHECK: PASS")
        message("godot.m1_load ${pass} run failed (exit ${rc})")
        set(failed TRUE)
    endif()
endforeach()
file(REMOVE_RECURSE "${root}")
message("shots: ${shots}/cold, ${shots}/warm")
if(failed)
    message(FATAL_ERROR "godot.m1_load failed")
endif()
