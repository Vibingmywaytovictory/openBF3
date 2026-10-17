# tools.ebx_memory (H hygiene): the EbxLibrary memory gate. Runs `bf3dump ebxload @all-mp` (50,486 partitions of 33
# superbundles) and `bf3dump ebxload Win32/Levels/MP_Subway/MP_Subway` and reads the `memory ... peak ws` line
# (GetProcessMemoryInfo): pass when every partition parses and the peak working set is at most 760 MiB for @all-mp
# and 145 MiB for Metro (before the shared type tables: 903-908 and 166-168 MiB; after: 678-680 and 127-128).
# Exits 77 (skipped) when bf3dump finds no install. REPORT_ONLY (the asan preset) prints the rows without failing.
# Also printed (info, never gated; docs/testing.md): the peak commit (PeakPagefileUsage, bf3dump's "peak ... private"),
# which working-set trimming does not lower, and a warning when peak ws / peak commit falls below 0.8 x the serial
# ratio (the process was trimmed, e.g. by a parallel neighbour; the serial merge-pass run decides).
# Usage: cmake -DBF3DUMP=<bf3dump.exe> [-DREPORT_ONLY=ON] -P ebx_memory.cmake
cmake_minimum_required(VERSION 3.29)

if(NOT BF3DUMP OR NOT EXISTS "${BF3DUMP}")
    message(FATAL_ERROR "ebx_memory: BF3DUMP=<bf3dump.exe> is required")
endif()

set(_failed 0)
function(check_load _spec _limit_mib _serial_ratio_milli)
    execute_process(COMMAND "${BF3DUMP}" ebxload "${_spec}" RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err TIMEOUT 300)
    if(_rc EQUAL 77 OR _err MATCHES "install not usable")
        message(STATUS "ebx_memory: no install: ${_err}")
        cmake_language(EXIT 77)
    endif()
    string(REGEX MATCH "partitions\t([0-9]+)\tparsed\t([0-9]+)\tfailed\t([0-9]+)" _counts "${_out}")
    set(_parts "${CMAKE_MATCH_1}")
    set(_bad "${CMAKE_MATCH_3}")
    string(REGEX MATCH "memory\t[^\n]*peak ws ([0-9.]+) private ([0-9.]+)" _mem "${_out}")
    set(_peak "${CMAKE_MATCH_1}")
    set(_commit "${CMAKE_MATCH_2}")
    string(REGEX MATCH "partition_heap\t[^\n]*" _heap "${_out}")
    message(STATUS "${_spec}: exit ${_rc}, ${_parts} partitions, ${_bad} failed, peak working set ${_peak} MiB (limit ${_limit_mib})")
    message(STATUS "  ${_mem}")
    message(STATUS "  ${_heap}")
    string(REGEX MATCH "^[0-9]+" _ws_int "${_peak}")
    string(REGEX MATCH "^[0-9]+" _commit_int "${_commit}")
    if(NOT _ws_int STREQUAL "" AND _commit_int GREATER 0)
        math(EXPR _ratio "${_ws_int} * 1000 / ${_commit_int}")
        math(EXPR _floor "${_serial_ratio_milli} * 8 / 10")
        message(STATUS "  info  peak commit (PeakPagefileUsage) ${_commit} MiB; peak ws / commit ${_ratio} / 1000 (serial ${_serial_ratio_milli})")
        if(_ratio LESS _floor)
            message(STATUS "  WARN  peak ws / commit ${_ratio} / 1000 is below 0.8 x serial: the working set was trimmed")
        endif()
    endif()
    set(_ok TRUE)
    if(NOT _rc EQUAL 0 OR NOT _bad STREQUAL "0" OR _peak STREQUAL "")
        set(_ok FALSE)
    elseif(_peak GREATER ${_limit_mib})
        set(_ok FALSE)
    endif()
    if(_ok)
        message(STATUS "  ok    ${_spec}")
    else()
        message(STATUS "  FAIL  ${_spec}\n${_err}")
        set(_failed 1 PARENT_SCOPE)
    endif()
endfunction()

# The third value: peak ws / peak commit x 1000 measured alone (2026-10-17: 678.1 / 699.5, 128.1 / 127.5 MiB).
check_load("@all-mp" 760 969)
check_load("Win32/Levels/MP_Subway/MP_Subway" 145 1004)

if(_failed AND NOT REPORT_ONLY)
    message(STATUS "tools.ebx_memory: FAIL")
    cmake_language(EXIT 1)
endif()
message(STATUS "tools.ebx_memory: PASS")
