# ctest godot.l1_look (README "L1 look"): the frame-time A/B of the L1 look against the M1 look, and the before /
# after screenshots. Runs the windowed M1 scene fullscreen (the monitor's mode; a run whose viewport is not 2560x1440 fails) with vsync off,
# alternating --look=m1 and --look=l1 for ROUNDS rounds (default 5), each run taking the same four free-camera
# views (street at TDM spawn 13, station at TDM spawn 22, park, overview) after a 3 s settle, timing 300 frames at
# each view (frame interval and the viewport's GPU render time) before its screenshot:
#   <godot> --fullscreen --disable-vsync --path bf3-opensource res://scenes/m1_metro.tscn -- --look=<m1|l1>
#       --shot-settle=3 --shot-measure=300 --shots=<dir>/<look>_<view>.png@x,y,z,yaw,pitch;... --quit-after=1
# Gate (integration decision, README "L1 look"): the median over the rounds of L1's frame interval <= 1.25 x M1's,
# where a run's frame interval is the mean over its views of the per-view median frame interval, AND the median over
# the rounds of L1's p99 frame interval (over every timed frame of the run, all views) <= 16.7 ms at 2560x1440 with
# vsync off. (The first gate, 1.10 x M1, penalised drawing glass, fog and the LUT that M1 skipped.) The GPU times
# (same statistic), M1's p99 and the plain means are reported. Timing is noisy (other processes, the display compositor): per-frame medians and the median of
# alternating rounds keep a single spike or a busy round from deciding.
# Screenshots go to $OPENBF3_LOOK_SHOTS when set (e.g. openBF3\b\shots\l1, gitignored; never commit them), else to
# the system temp directory, removed at the end. Skips (exit 77) like godot.m1_load: no Godot executable or no window opt-in (godot_common.cmake), no BF3
# install, or no window / rendering device (probe run).
# Script mode: cmake -DPROJECT_DIR=<bf3-opensource> [-DROUNDS=n] -P godot_l1_look.cmake (CMake 3.29).
cmake_minimum_required(VERSION 3.29)
set(GODOT_WINDOWED TRUE)
include(${CMAKE_CURRENT_LIST_DIR}/godot_common.cmake)
if(NOT DEFINED ROUNDS)
    set(ROUNDS 5)
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
    set(whole "${CMAKE_MATCH_1}") # saved: the REGEX REPLACE below resets CMAKE_MATCH_<n>
    set(frac "${CMAKE_MATCH_2}000")
    string(SUBSTRING "${frac}" 0 3 frac)
    string(REGEX REPLACE "^0+([0-9])" "\\1" frac "${frac}")
    math(EXPR us "${whole} * 1000 + ${frac}")
    set(${out} ${us} PARENT_SCOPE)
endfunction()

set(failed FALSE)
set(m1_us "")
set(l1_us "")
set(m1_gpu "")
set(l1_gpu "")
set(m1_p99 "")
set(l1_p99 "")
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
        string(REGEX MATCH "frame time median: ([0-9]+\\.[0-9]+) ms per frame \\(interval\\), GPU ([0-9]+\\.[0-9]+) ms" mean "${out}")
        if(NOT rc STREQUAL "0" OR NOT mean)
            message("${out}")
            message("${err}")
            message("godot.l1_look: round ${round} ${look} failed (exit ${rc}) or printed no frame time")
            set(failed TRUE)
            continue()
        endif()
        set(mean_ms "${CMAKE_MATCH_1}")
        set(gpu_ms "${CMAKE_MATCH_2}")
        # The gate is defined at 1440p: --fullscreen takes the monitor's mode, so check it rather than assume it.
        string(REGEX MATCH "mean of the per-view medians at [0-9]+ views, viewport ([0-9]+)x([0-9]+)" vp "${out}")
        if(NOT vp OR NOT CMAKE_MATCH_1 STREQUAL "2560" OR NOT CMAKE_MATCH_2 STREQUAL "1440")
            message("godot.l1_look: round ${round} ${look} rendered at '${CMAKE_MATCH_1}x${CMAKE_MATCH_2}', not 2560x1440 (the gate's resolution)")
            set(failed TRUE)
            continue()
        endif()
        to_us("${mean_ms}" us)
        to_us("${gpu_ms}" gpu)
        list(APPEND ${look}_us ${us})
        list(APPEND ${look}_gpu ${gpu})
        # Integration: the run's p99 frame interval over every timed frame (m1_check.cpp "[look] frame time p99").
        string(REGEX MATCH "frame time p99: ([0-9]+\\.[0-9]+) ms per frame" p99_line "${out}")
        if(NOT p99_line)
            message("godot.l1_look: round ${round} ${look} printed no p99 frame time")
            set(failed TRUE)
            continue()
        endif()
        to_us("${CMAKE_MATCH_1}" p99)
        list(APPEND ${look}_p99 ${p99})
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
median("${m1_p99}" m1p)
median("${l1_p99}" l1p)
math(EXPR limit "${m1} * 125 / 100")
set(p99_limit 16700)
math(EXPR ratio "${l1} * 1000 / ${m1}")
math(EXPR gratio "${l1g} * 1000 / ${m1g}")
message("frame interval (mean of per-view medians) per round (us): M1 ${m1_us}; L1 ${l1_us}")
message("GPU render time (mean of per-view medians) per round (us): M1 ${m1_gpu}; L1 ${l1_gpu}")
message("p99 frame interval (all timed frames of a run) per round (us): M1 ${m1_p99}; L1 ${l1_p99}")
message("median frame interval: M1 ${m1} us, L1 ${l1} us (L1 / M1 = ${ratio} per mille; limit ${limit} us = 1.25 x M1)")
message("median p99 frame interval: M1 ${m1p} us (reported), L1 ${l1p} us (limit ${p99_limit} us)")
message("median GPU render time: M1 ${m1g} us, L1 ${l1g} us (L1 / M1 = ${gratio} per mille; reported, not gated)")
set(gate_failed FALSE)
if(l1 GREATER limit)
    message("godot.l1_look: L1 frame interval more than 25 % over M1")
    set(gate_failed TRUE)
endif()
if(l1p GREATER p99_limit)
    message("godot.l1_look: L1 p99 frame interval over 16.7 ms")
    set(gate_failed TRUE)
endif()
if(gate_failed)
    message(FATAL_ERROR "godot.l1_look: FAIL")
endif()
message("godot.l1_look: PASS")
