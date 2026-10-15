# ctest godot.l1_look (README "L1 look"): the frame-time A/B of the L1 look against the M1 look, and the before /
# after screenshots. Runs the windowed M1 scene fullscreen (2560x1440 on the reference monitor) with vsync off,
# alternating --look=m1 and --look=l1 for ROUNDS rounds (default 3), each run taking the same four free-camera
# views (street at TDM spawn 13, station at TDM spawn 22, park, overview) after a 3 s settle, timing 300 frames at
# each view (frame interval and the viewport's GPU render time) before its screenshot:
#   <godot> --fullscreen --disable-vsync --path bf3-opensource res://scenes/m1_metro.tscn -- --look=<m1|l1>
#       --shot-settle=3 --shot-measure=300 --shots=<dir>/<look>_<view>.png@x,y,z,yaw,pitch;... --quit-after=1
# Gate: the median over the rounds of L1's mean frame interval <= 1.10 x M1's. The GPU times are reported. Timing
# is noisy (other processes, the display compositor): the median of alternating rounds is what is compared.
# Screenshots go to $OPENBF3_LOOK_SHOTS when set (e.g. openBF3\b\shots\l1, gitignored; never commit them), else to
# the system temp directory, removed at the end. Skips (exit 77) like godot.m1_load: OPENBF3_GODOT unset, no BF3
# install, or no window / rendering device (probe run).
# Script mode: cmake -DPROJECT_DIR=<bf3-opensource> [-DROUNDS=n] -P godot_l1_look.cmake (CMake 3.29).
cmake_minimum_required(VERSION 3.29)
set(godot "$ENV{OPENBF3_GODOT}")
if(godot STREQUAL "" OR NOT EXISTS "${godot}")
    message("SKIPPED: OPENBF3_GODOT does not name the Godot console executable")
    cmake_language(EXIT 77)
endif()
if(NOT DEFINED ROUNDS)
    set(ROUNDS 3)
endif()
if(NOT EXISTS "${PROJECT_DIR}/.godot/extension_list.cfg")
    file(WRITE "${PROJECT_DIR}/.godot/extension_list.cfg" "res://openbf3.gdextension\n")
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
string(RANDOM LENGTH 8 ALPHABET 0123456789abcdef tag)
set(root "$ENV{TEMP}/openbf3-godot-l1-look-${tag}")
set(keep_shots FALSE)
if("$ENV{OPENBF3_LOOK_SHOTS}" STREQUAL "")
    set(shots "$ENV{TEMP}/openbf3-godot-l1-look-shots-${tag}")
else()
    file(TO_CMAKE_PATH "$ENV{OPENBF3_LOOK_SHOTS}" shots)
    set(keep_shots TRUE)
endif()
file(MAKE_DIRECTORY "${shots}")

# "<ms>.<frac>" -> integer microseconds.
function(to_us value out)
    string(REGEX MATCH "^([0-9]+)\\.([0-9]+)$" ok "${value}")
    if(NOT ok)
        set(${out} -1 PARENT_SCOPE)
        return()
    endif()
    set(frac "${CMAKE_MATCH_2}000")
    string(SUBSTRING "${frac}" 0 3 frac)
    string(REGEX REPLACE "^0+([0-9])" "\\1" frac "${frac}")
    math(EXPR us "${CMAKE_MATCH_1} * 1000 + ${frac}")
    set(${out} ${us} PARENT_SCOPE)
endfunction()

set(failed FALSE)
set(m1_us "")
set(l1_us "")
set(m1_gpu "")
set(l1_gpu "")
foreach(round RANGE 1 ${ROUNDS})
    foreach(look m1 l1)
        set(views "${shots}/${look}_street.png@-196.14,61.47,-568.25,-136.6,0;${shots}/${look}_station.png@-167.76,66.2,-475.19,-150.8,0;${shots}/${look}_park.png@-40,110,-230,30,-20;${shots}/${look}_overview.png@-150,260,-800,0,-40")
        execute_process(
            COMMAND "${godot}" --fullscreen --disable-vsync --path "${PROJECT_DIR}" res://scenes/m1_metro.tscn -- "--look=${look}" "--bf3-cache=${root}"
                --no-hud --shot-settle=3 --shot-measure=300 "--shots=${views}" --quit-after=1
            TIMEOUT 240
            RESULT_VARIABLE rc
            OUTPUT_VARIABLE out
            ERROR_VARIABLE err)
        if(out MATCHES "no usable install" OR err MATCHES "no usable install")
            file(REMOVE_RECURSE "${root}")
            message("SKIPPED: no BF3 install found")
            cmake_language(EXIT 77)
        endif()
        string(REGEX MATCHALL "\\[look\\] frame time[^\n]*" lines "${out}")
        foreach(line IN LISTS lines)
            message("round ${round} ${look}: ${line}")
        endforeach()
        string(REGEX MATCH "frame time mean: ([0-9]+\\.[0-9]+) ms per frame \\(interval\\), GPU ([0-9]+\\.[0-9]+) ms" mean "${out}")
        if(NOT rc STREQUAL "0" OR NOT mean)
            message("${out}")
            message("${err}")
            message("godot.l1_look: round ${round} ${look} failed (exit ${rc}) or printed no frame time")
            set(failed TRUE)
            continue()
        endif()
        to_us("${CMAKE_MATCH_1}" us)
        to_us("${CMAKE_MATCH_2}" gpu)
        list(APPEND ${look}_us ${us})
        list(APPEND ${look}_gpu ${gpu})
        string(REGEX MATCHALL "\\[m1\\] shot look L1:[^\n]*" envs "${out}")
        if(round EQUAL 1)
            foreach(line IN LISTS envs)
                message("  ${line}")
            endforeach()
        endif()
    endforeach()
endforeach()
file(REMOVE_RECURSE "${root}")
if(NOT keep_shots)
    file(REMOVE_RECURSE "${shots}")
else()
    message("shots (last round): ${shots}")
endif()
if(failed)
    message(FATAL_ERROR "godot.l1_look: a run failed")
endif()

function(median values out)
    list(SORT values COMPARE NATURAL)
    list(LENGTH values n)
    math(EXPR mid "${n} / 2")
    list(GET values ${mid} m)
    set(${out} ${m} PARENT_SCOPE)
endfunction()
median("${m1_us}" m1)
median("${l1_us}" l1)
median("${m1_gpu}" m1g)
median("${l1_gpu}" l1g)
math(EXPR limit "${m1} * 110 / 100")
math(EXPR ratio "${l1} * 1000 / ${m1}")
math(EXPR gratio "${l1g} * 1000 / ${m1g}")
message("frame interval per round (us): M1 ${m1_us}; L1 ${l1_us}")
message("GPU render time per round (us): M1 ${m1_gpu}; L1 ${l1_gpu}")
message("median frame interval: M1 ${m1} us, L1 ${l1} us (L1 / M1 = ${ratio} per mille; limit ${limit} us = 1.10 x M1)")
message("median GPU render time: M1 ${m1g} us, L1 ${l1g} us (L1 / M1 = ${gratio} per mille; reported, not gated)")
if(l1 GREATER limit)
    message(FATAL_ERROR "godot.l1_look: FAIL (L1 frame interval more than 10 % over M1)")
endif()
message("godot.l1_look: PASS")
