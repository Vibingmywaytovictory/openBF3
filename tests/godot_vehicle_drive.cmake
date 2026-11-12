cmake_minimum_required(VERSION 3.29)
set(GODOT_WINDOWED TRUE)
include(${CMAKE_CURRENT_LIST_DIR}/godot_common.cmake)
set(cache "${BUILD_DIR}/godot-cache/${ROW}")
set(shots "${BUILD_DIR}/shots/${ROW}")
file(MAKE_DIRECTORY "${cache}" "${shots}")
set(netsim "")
if(DEFINED NETSIM)
    set(netsim "--netsim=${NETSIM}")
endif()
godot_run(SKIP_WITHOUT_INSTALL TIMEOUT 170
    ARGS --path "${PROJECT_DIR}" --resolution 1280x720 res://scenes/metro_tdm.tscn -- --level=MP_007 --mode=ConquestSmall0
        --bots=0 --deploy-screen=0 --preround=0 --min-players=1 --vehicle-drive "--vehicle-shots=${shots}" ${netsim}
        "--bf3-cache=${cache}")
godot_require_pass("VEHICLE DRIVE: PASS")
godot_require_pass("\\[level\\] install unchanged: yes")
