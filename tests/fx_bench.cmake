# tools.fx_bench (M2a VFX1 gate G9, docs/design/m2a-vfx1.md §9): runs `bf3dump fx bench --seed 1` (the oilfire /
# 1p muzzle / impact mix on one lane, 5 runs) and passes when the bench exits 0: the median ns per admitted particle
# per sub-step is at most 50 and the run is valid (mean live in [12,000, 20,000], spawns_dropped 0, refused_starts 0).
# The figure is wall clock on a shared machine: noisy. Exits 77 (skipped) when bf3dump finds no install.
# REPORT_ONLY (the asan preset) prints the rows without failing.
# Usage: cmake -DBF3DUMP=<bf3dump.exe> [-DREPORT_ONLY=ON] -P fx_bench.cmake
cmake_minimum_required(VERSION 3.29)

if(NOT BF3DUMP OR NOT EXISTS "${BF3DUMP}")
    message(FATAL_ERROR "fx_bench: BF3DUMP=<bf3dump.exe> is required")
endif()

execute_process(COMMAND "${BF3DUMP}" fx bench --seed 1 RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err TIMEOUT 900)
if(_rc EQUAL 77 OR _err MATCHES "install not usable")
    message(STATUS "fx_bench: no install: ${_err}")
    cmake_language(EXIT 77)
endif()
message(STATUS "${_out}")
string(REGEX MATCH "median_ns\t([0-9.]+)" _median "${_out}")
message(STATUS "tools.fx_bench: exit ${_rc}, median ${CMAKE_MATCH_1} ns per admitted particle per sub-step (gate 50, reference 24.7)")
if(NOT _rc EQUAL 0)
    if(REPORT_ONLY)
        message(STATUS "tools.fx_bench: over the gate or invalid, reported only (REPORT_ONLY)\n${_err}")
        return()
    endif()
    message(STATUS "tools.fx_bench: FAIL\n${_err}")
    cmake_language(EXIT 1)
endif()
message(STATUS "tools.fx_bench: PASS")
