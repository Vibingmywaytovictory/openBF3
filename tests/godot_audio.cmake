# ctest godot.audio (gate G8, docs/design/m2a-audio.md §1, §10.3, §10.4): the windowed M2a audio check of the Godot
# project:
#   <godot> --path bf3-opensource res://scenes/audio_m16.tscn -- --audio-check --audio-record=<SHOTS>/godot_m16.wav
#       --quit-after=90
# The M16A3 check script plays through Bf3AudioPlayback on Godot's audio thread (the real driver path: windowed, not
# the headless Dummy driver); the demo then checks rows (a)-(d) on its recording, the recording length, the mixer
# counters, the server mix rate and an in-process offline replay equal to the recording sample for sample, prints
# the table and "AUDIO CHECK: PASS|FAIL", and exits 0/1. This script additionally needs the three WAVs
# (godot_m16.wav, godot_m16_core.wav, godot_m16_noise.wav) in SHOTS (b/shots/audio, gitignored).
# Skips (exit 77) when no Godot console executable is found or the windowed row is not opted in (godot_common.cmake), no BF3 install is found, or no window / rendering device can be
# created: a probe run ("--path <project> --quit", windowed, killed after 60 s) decides first, as in
# godot_m1_load.cmake.
# Script mode: cmake -DPROJECT_DIR=<bf3-opensource> -DSHOTS=<dir> -P godot_audio.cmake (cmake_language(EXIT) needs 3.29).
cmake_minimum_required(VERSION 3.29)
set(GODOT_WINDOWED TRUE)
include(${CMAKE_CURRENT_LIST_DIR}/godot_common.cmake)
if(NOT SHOTS)
    message(FATAL_ERROR "godot_audio: -DSHOTS=<dir> is required")
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
file(MAKE_DIRECTORY "${SHOTS}")
set(wav "${SHOTS}/godot_m16.wav")
file(REMOVE "${wav}" "${SHOTS}/godot_m16_core.wav" "${SHOTS}/godot_m16_noise.wav")
execute_process(
    COMMAND "${godot}" --path "${PROJECT_DIR}" res://scenes/audio_m16.tscn -- --audio-check "--audio-record=${wav}" --quit-after=90
    TIMEOUT 180
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
set(failed FALSE)
if(NOT rc STREQUAL "0" OR NOT out MATCHES "AUDIO CHECK: PASS")
    message("godot.audio: the check failed (exit ${rc})")
    set(failed TRUE)
endif()
if(NOT out MATCHES "install.unchanged yes")
    message("godot.audio: install.unchanged yes missing")
    set(failed TRUE)
endif()
foreach(f godot_m16.wav godot_m16_core.wav godot_m16_noise.wav)
    if(NOT EXISTS "${SHOTS}/${f}")
        message("godot.audio: ${SHOTS}/${f} missing")
        set(failed TRUE)
    endif()
endforeach()
if(failed)
    message(FATAL_ERROR "godot.audio failed")
endif()
message("godot.audio: PASS (WAVs in ${SHOTS})")
