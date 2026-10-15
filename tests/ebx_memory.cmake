# tools.ebx_memory (H hygiene): the EbxLibrary memory gate. Runs `bf3dump ebxload @all-mp` (50,486 partitions of 33
# superbundles) and `bf3dump ebxload Win32/Levels/MP_Subway/MP_Subway` and reads the `memory ... peak ws` line
# (GetProcessMemoryInfo): pass when every partition parses and the peak working set is at most 760 MiB for @all-mp
# and 145 MiB for Metro (before the shared type tables: 903-908 and 166-168 MiB; after: 678-680 and 127-128).
# Exits 77 (skipped) when bf3dump finds no install. REPORT_ONLY (the asan preset) prints the rows without failing.
# Usage: cmake -DBF3DUMP=<bf3dump.exe> [-DREPORT_ONLY=ON] -P ebx_memory.cmake
cmake_minimum_required(VERSION 3.29)

if(NOT BF3DUMP OR NOT EXISTS "${BF3DUMP}")
    message(FATAL_ERROR "ebx_memory: BF3DUMP=<bf3dump.exe> is required")
endif()

set(_failed 0)
function(check_load _spec _limit_mib)
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
    string(REGEX MATCH "partition_heap\t[^\n]*" _heap "${_out}")
    message(STATUS "${_spec}: exit ${_rc}, ${_parts} partitions, ${_bad} failed, peak working set ${_peak} MiB (limit ${_limit_mib})")
    message(STATUS "  ${_mem}")
    message(STATUS "  ${_heap}")
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

check_load("@all-mp" 760)
check_load("Win32/Levels/MP_Subway/MP_Subway" 145)

if(_failed AND NOT REPORT_ONLY)
    message(STATUS "tools.ebx_memory: FAIL")
    cmake_language(EXIT 1)
endif()
message(STATUS "tools.ebx_memory: PASS")
