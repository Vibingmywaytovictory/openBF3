add_library(bf3_godot SHARED
    src/register_types.cpp
    src/game_session.cpp
    src/game_session.h
    src/dedicated_main_loop.cpp
    src/dedicated_main_loop.h
    src/godot_util.cpp
    src/godot_util.h
    src/render_bridge.cpp
    src/render_bridge.h
)
target_sources(bf3_godot PRIVATE
    src/bf3_localisation.cpp
    src/bf3_localisation.h
    src/weapon_inspector.cpp
    src/weapon_inspector.h
)
target_sources(bf3_godot PRIVATE
    src/collision_overlay.cpp
    src/collision_overlay.h
)
target_sources(bf3_godot PRIVATE
    src/godot_cache.cpp
    src/godot_cache.h
)
target_link_libraries(bf3_godot PRIVATE godot-cpp bf3_host bf3_core bf3_sim bf3_net bf3_anim bf3_render)
openbf3_module(bf3_godot)
target_link_options(bf3_godot PRIVATE /INCREMENTAL:NO)

get_target_property(GODOTCPP_SUFFIX godot-cpp GODOTCPP_SUFFIX)
set_target_properties(bf3_godot PROPERTIES
    PREFIX "lib"
    OUTPUT_NAME "openbf3${GODOTCPP_SUFFIX}"
    RUNTIME_OUTPUT_DIRECTORY "$<1:${OPENBF3_GODOT_PROJECT}/bin>"
    LIBRARY_OUTPUT_DIRECTORY "$<1:${OPENBF3_GODOT_PROJECT}/bin>"
    ARCHIVE_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/lib>"
    PDB_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/pdb>"
)

target_sources(bf3_godot PRIVATE
    src/level_session.cpp
    src/level_session.h
    src/level_upload.cpp
    src/level_upload.h
    src/fp_camera.cpp
    src/fp_camera.h
    src/input_mapper.cpp
    src/input_mapper.h
    src/loading_screen.cpp
    src/loading_screen.h
)
target_link_libraries(bf3_godot PRIVATE psapi)

target_sources(bf3_godot PRIVATE
    src/crowd_scene.cpp
    src/crowd_scene.h
    src/skin_upload.cpp
    src/skin_upload.h
)

target_sources(bf3_godot PRIVATE
    src/look_bridge.cpp
    src/look_bridge.h
)

target_sources(bf3_godot PRIVATE
    src/nav_overlay.cpp
    src/nav_overlay.h
)

target_sources(bf3_godot PRIVATE
    src/ui_assets.cpp
    src/ui_assets.h
    src/swf_view.cpp
    src/swf_view.h
    src/ui_textures.cpp
    src/ui_textures.h
)

target_sources(bf3_godot PRIVATE
    src/minimap_hud.cpp
    src/minimap_hud.h
    src/minimap_control.cpp
    src/minimap_control.h
    src/world_icons.cpp
    src/world_icons.h
)

target_sources(bf3_godot PRIVATE
    src/viewmodel.cpp
    src/viewmodel.h
    src/vm_script.cpp
    src/vm_script.h
)

target_sources(bf3_godot PRIVATE
    src/fx_renderer.cpp
    src/fx_renderer.h
)
target_link_libraries(bf3_godot PRIVATE bf3_fx)

target_sources(bf3_godot PRIVATE
    src/hud_items.cpp
    src/hud_items.h
    src/native_hud.cpp
    src/native_hud.h
    src/native_flow.cpp
    src/native_flow.h
    src/native_screens.cpp
    src/native_screens.h
    src/ui_screen_host.h
    src/ui_screen_records.cpp
    src/ui_screen_records.h
    src/native_tags.cpp
    src/native_tags.h
    src/native_hud_moments.h
)

target_sources(bf3_godot PRIVATE
    src/audio_stream.cpp
    src/audio_stream.h
)
target_link_libraries(bf3_godot PRIVATE bf3_audio bf3_client_audio)

target_sources(bf3_godot PRIVATE
    src/game_frame.h
    src/game_audio.h
    src/game_fx.h
    src/game_crowd.h
    src/m2a_check.h
    src/level_session_game.h
    src/level_session_game.cpp
    src/viewmodel_muzzle.h
    src/viewmodel_muzzle.cpp
)

target_sources(bf3_godot PRIVATE
    src/game_crowd.cpp
    src/game_crowd_items.h
)

target_sources(bf3_godot PRIVATE
    src/game_audio.cpp
)

target_sources(bf3_godot PRIVATE
    src/game_fx.cpp
    src/fx_decals.cpp
    src/fx_decals.h
)

target_sources(bf3_godot PRIVATE
    src/hud_tdm.h
    src/death_camera.h
    src/tdm_flow.h
    src/m2b_check.h
    src/level_session_tdm.h
)

target_sources(bf3_godot PRIVATE
    src/death_camera.cpp
)

target_sources(bf3_godot PRIVATE
    src/hud_tdm.cpp
    src/tdm_hud_records.cpp
    src/hud_tdm_screen.h
    src/hud_tdm_text.h
    src/hud_scoreboard.h
    src/hud_scoreboard.cpp
    src/spawn_screen.h
    src/spawn_screen.cpp
    src/kill_card.h
    src/kill_card.cpp
    src/pre_round.h
    src/pre_round.cpp
    src/round_end.h
    src/round_end.cpp
)

target_sources(bf3_godot PRIVATE
    src/level_session_tdm.cpp
    src/tdm_flow.cpp
)

target_sources(bf3_godot PRIVATE
    src/iff_frame.h
    src/iff_frame.cpp
)

target_sources(bf3_godot PRIVATE
    src/ui_post_process.cpp
    src/ui_post_process.h
    src/ui_cursor.cpp
    src/ui_cursor.h
    src/overview_map.cpp
    src/overview_map.h
)

if(OPENBF3_BUILD_CHECKS)
    target_sources(bf3_godot PRIVATE
        src/audio_demo.cpp
        src/audio_demo.h
        src/collision_check.cpp
        src/collision_check.h
        src/crosshair_walk_check.cpp
        src/fx3_demo.cpp
        src/fx3_demo.h
        src/fx_demo.cpp
        src/fx_demo.h
        src/fx_ref.cpp
        src/fx_ref.h
        src/hud_check.cpp
        src/hud_check.h
        src/hud_tdm_demo.cpp
        src/hud_tdm_demo.h
        src/m0_inspector_check.cpp
        src/m0_inspector_check.h
        src/m1_check.cpp
        src/m1_check.h
        src/m2a_check.cpp
        src/m2a_viewmodel_check.cpp
        src/m2a_viewmodel_demo.cpp
        src/m2a_viewmodel_demo.h
        src/m2b_check.cpp
        src/m2b_check_rows.cpp
        src/m2b_check_rows.h
        src/m9_reload_check.cpp
        src/m9_reload_check.h
        src/minimap_check.cpp
        src/minimap_check.h
        src/native_hud_moments.cpp
        src/native_screens_check.cpp
        src/native_screens_check.h
        src/s0_selftest.cpp
        src/s0_selftest.h
        src/swf_view_check.cpp
        src/swf_view_check.h
        src/tp_crowd_check.cpp
        src/tp_crowd_check.h
        src/ui_font_check.cpp
        src/ui_font_check.h
        src/viewmodel_demo.cpp
        src/viewmodel_demo.h
    )
    target_sources(bf3_godot PRIVATE
        src/ui_effects_check.cpp
        src/ui_effects_check.h
    )
    target_link_libraries(bf3_godot PRIVATE bf3_sim_harness)
    target_include_directories(bf3_godot PRIVATE ${PROJECT_SOURCE_DIR}/tests)
    target_include_directories(bf3_godot PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_sources(bf3_godot PRIVATE src/moments/in_round_moments.h)
    openbf3_registry(bf3_godot src/moments native_hud_moments.h NativeHudMomentGroups bf3::godot_side register_native_hud_moments_ register_native_hud_moment_files)
endif()
target_compile_definitions(bf3_godot PRIVATE OPENBF3_BUILD_CHECKS=$<BOOL:${OPENBF3_BUILD_CHECKS}>)
