cmake_minimum_required(VERSION 3.29)
set(GODOT_WINDOWED TRUE)
include(${CMAKE_CURRENT_LIST_DIR}/godot_common.cmake)
set(shots "${BUILD_DIR}/shots/${ROW}")
file(MAKE_DIRECTORY "${shots}")
godot_run(SKIP_WITHOUT_INSTALL TIMEOUT 300
    ARGS ${godot_log_args} --path "${PROJECT_DIR}" --resolution 1280x720 res://scenes/metro_tdm.tscn --
        --level=MP_007 --mode=ConquestSmall0 --bots=16 --deploy-screen=0 --preround=0 --min-players=1 --bot-vehicles
        "--bot-vehicles-shots=${shots}" ${godot_cache_args})
godot_require_pass("BOT VEHICLES ROW bots_seated PASS")
godot_require_pass("BOT VEHICLES ROW shot PASS")
godot_require_pass("BOT VEHICLES ROW bot_driving PASS")
godot_require_pass("BOT VEHICLES ROW drive_shot PASS")
godot_require_pass("BOT VEHICLES: PASS")
