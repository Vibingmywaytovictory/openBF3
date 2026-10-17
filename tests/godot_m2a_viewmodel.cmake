# ctest godot.m2a_viewmodel (M2a vm, docs/design/m2a-vm.md): runs the headless viewmodel check of the Godot project
# ("<godot> --headless --path bf3-opensource res://scenes/m2a_viewmodel.tscn -- --vm-check") and passes when it prints
# "M2A VIEWMODEL CHECK: PASS" and exits 0: E19 (FOV, FOV times, recoil, springs, zoom refusal), the E5 viewmodel row
# and E11's viewmodel coverage on the 8 default loadouts, every display frame at 60 and 144 fps. Exits 77 (skipped)
# when OPENBF3_GODOT does not name the Godot console executable or no BF3 install is found.
# Script mode: cmake -DPROJECT_DIR=<bf3-opensource> -P godot_m2a_viewmodel.cmake (cmake_language(EXIT) needs CMake 3.29).
cmake_minimum_required(VERSION 3.29)
set(godot "$ENV{OPENBF3_GODOT}")
if(godot STREQUAL "" OR NOT EXISTS "${godot}")
    message("SKIPPED: OPENBF3_GODOT does not name the Godot console executable")
    cmake_language(EXIT 77)
endif()
if(NOT EXISTS "${PROJECT_DIR}/.godot/extension_list.cfg")
    file(WRITE "${PROJECT_DIR}/.godot/extension_list.cfg" "res://openbf3.gdextension\n")
endif()
execute_process(
    COMMAND "${godot}" --headless --path "${PROJECT_DIR}" res://scenes/m2a_viewmodel.tscn -- --vm-check
    TIMEOUT 400
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
if(NOT rc STREQUAL "0" OR NOT out MATCHES "M2A VIEWMODEL CHECK: PASS")
    message(FATAL_ERROR "godot.m2a_viewmodel failed (exit ${rc})")
endif()
