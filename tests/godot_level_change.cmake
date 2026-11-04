cmake_minimum_required(VERSION 3.29)
set(GODOT_WINDOWED TRUE)
include(${CMAKE_CURRENT_LIST_DIR}/godot_common.cmake)
string(RANDOM LENGTH 8 ALPHABET 0123456789abcdef tag)
set(scratch "${BUILD_DIR}/scratch/${ROW}-${tag}")
set(cache "${BUILD_DIR}/godot-cache/${ROW}")
file(REMOVE_RECURSE "${scratch}")
file(MAKE_DIRECTORY "${scratch}" "${cache}")
file(WRITE "${scratch}/maplist.cfg" "MP_Subway TeamDeathMatch0 1\nMP_011 TeamDeathMatch0 1\nMP_003 TeamDeathMatch0 1\n")
godot_run(SKIP_WITHOUT_INSTALL TIMEOUT 570 REMOVE_ON_SKIP "${scratch}"
    ARGS --path "${PROJECT_DIR}" --resolution 1280x720 res://scenes/metro_tdm.tscn -- "--maplist=${scratch}/maplist.cfg"
        --bots=15 --kill-limit=10 --preround=0 --match-levels=3 --quit-after=2 "--bf3-cache=${cache}")
file(REMOVE_RECURSE "${scratch}")
set(failures "")
foreach(want
        "[match] listen server rotates MP_Subway TeamDeathMatch0 x1, MP_011 TeamDeathMatch0 x1, MP_003 TeamDeathMatch0 x1"
        "[match] level change 1: MP_011 TeamDeathMatch0 round 1,"
        "[match] level change 2: MP_003 TeamDeathMatch0 round 2,"
        "[match] level 1 MP_Subway TeamDeathMatch0 done: "
        "[match] level 2 MP_011 TeamDeathMatch0 done: "
        "[match] level 3 MP_003 TeamDeathMatch0 done: "
        "[level] install unchanged: yes")
    string(FIND "${out}" "${want}" at)
    if(at EQUAL -1)
        list(APPEND failures "no line '${want}'")
    endif()
endforeach()
string(REGEX MATCHALL "\\[match\\] level [0-9] [^\n]* done: [^\n]*" done_lines "${out}")
foreach(line IN LISTS done_lines)
    message("godot.level_change: ${line}")
    if(NOT line MATCHES ", orphan nodes 0,")
        list(APPEND failures "orphan nodes left: '${line}'")
    endif()
endforeach()
if(out MATCHES " ended: ")
    list(APPEND failures "the client ended or was kicked")
endif()
if(NOT rc STREQUAL "0" OR failures)
    list(JOIN failures "; " failure_text)
    message(FATAL_ERROR "${ROW} failed (exit ${rc}): ${failure_text}")
endif()
