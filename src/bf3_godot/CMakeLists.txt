# bf3_godot: the GDExtension DLL, the only target that sees Godot (plan §2.2).
# Output: <godot project>/bin/libopenbf3.windows.<template>.x86_64.dll (gitignored).
add_library(bf3_godot SHARED
    src/register_types.cpp
    src/game_session.cpp
    src/game_session.h
    src/dedicated_main_loop.cpp
    src/dedicated_main_loop.h
    src/s0_selftest.cpp
    src/s0_selftest.h
    src/godot_util.cpp
    src/godot_util.h
    src/render_bridge.cpp
    src/render_bridge.h
    src/viewmodel_demo.cpp
    src/viewmodel_demo.h
    src/m9_reload_check.cpp
    src/m9_reload_check.h
)
# Localisation (Translation backed by bf3_core's TextDatabase) and the WeaponDef inspector (M0 exit).
target_sources(bf3_godot PRIVATE
    src/bf3_localisation.cpp
    src/bf3_localisation.h
    src/weapon_inspector.cpp
    src/weapon_inspector.h
    src/m0_inspector_check.cpp
    src/m0_inspector_check.h
)
# Metro collision debug overlay (M0 exit) and its headless check (README "Collision overlay").
target_sources(bf3_godot PRIVATE
    src/collision_overlay.cpp
    src/collision_overlay.h
    src/collision_check.cpp
    src/collision_check.h
)
# The process's derived-data cache under user://bf3cache (meshes of the viewmodel demo; README "Preparing the cache").
target_sources(bf3_godot PRIVATE
    src/godot_cache.cpp
    src/godot_cache.h
)
target_link_libraries(bf3_godot PRIVATE godot-cpp bf3_host bf3_core bf3_sim bf3_net bf3_anim bf3_render)
openbf3_module(bf3_godot)
# No incremental linking: keeps the .ilk out of the project's bin/ and the DLL self-contained.
target_link_options(bf3_godot PRIVATE /INCREMENTAL:NO)

get_target_property(GODOTCPP_SUFFIX godot-cpp GODOTCPP_SUFFIX)
set_target_properties(bf3_godot PROPERTIES
    PREFIX "lib"
    OUTPUT_NAME "openbf3${GODOTCPP_SUFFIX}"
    RUNTIME_OUTPUT_DIRECTORY "$<1:${OPENBF3_GODOT_PROJECT}/bin>"
    LIBRARY_OUTPUT_DIRECTORY "$<1:${OPENBF3_GODOT_PROJECT}/bin>"
    # Import library and PDB stay in the build tree.
    ARCHIVE_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/lib>"
    PDB_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/pdb>"
)

# M1 T6 (godot; docs/design/m1.md §11, §12, §15): the M1 session node Bf3LevelSession (res://scenes/m1_metro.tscn),
# the level upload (C1 worker tasks, C2 main-thread budget), cameras, input mapping, loading screen and the
# --m1-check / --m1-load-check checks. psapi: the game's peak working set (reported by --m1-load-check).
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
    src/m1_check.cpp
    src/m1_check.h
)
target_link_libraries(bf3_godot PRIVATE psapi)

# S3 crowd (docs/design/s3.md §7, §11, §15): the Bf3Crowd node (res://scenes/s3_crowd.tscn) and the skin upload
# backends A (Skeleton3D) and B (RS skeleton).
target_sources(bf3_godot PRIVATE
    src/s3_crowd.cpp
    src/s3_crowd.h
    src/skin_upload.cpp
    src/skin_upload.h
)

# L1 look (README "L1 look"): the visual environment on Godot (sun, sky shader, ambient, fog, exposure, LUTs).
target_sources(bf3_godot PRIVATE
    src/look_bridge.cpp
    src/look_bridge.h
)

# S1 (docs/design/s1.md §12): the navmesh overlay of --bots=N --nav-overlay (Bf3LevelSession).
target_sources(bf3_godot PRIVATE
    src/nav_overlay.cpp
    src/nav_overlay.h
)

# UI ui1 (docs/design/ui-swf-fonts.md §10-§12): Bf3UiAssets (BF3 fonts as FontFiles, SWF atlases as AtlasTextures,
# the HUD placement table) and the ui1 check scene's Bf3UiFontCheck (res://scenes/ui_fonts.tscn), registered in the
# "// UI ui1" block of register_types.cpp.
target_sources(bf3_godot PRIVATE
    src/ui_assets.cpp
    src/ui_assets.h
    src/ui_font_check.cpp
    src/ui_font_check.h
)

# UI ui2 (docs/design/ui-minimap-icons.md §10, §12): the in-game minimap node (Bf3MinimapHud, Bf3MinimapControl,
# Bf3WorldIcons, MinimapCheck), registered in register_types.cpp under "// UI ui2".
target_sources(bf3_godot PRIVATE
    src/minimap_hud.cpp
    src/minimap_hud.h
    src/minimap_control.cpp
    src/minimap_control.h
    src/world_icons.cpp
    src/world_icons.h
    src/minimap_check.cpp
    src/minimap_check.h
)

# M2a vm (docs/design/m2a-vm.md; m2a.md §11.1, §11.2): Bf3Viewmodel (the 1p viewmodel and ADS camera), the vm demo /
# check scene Bf3M2aViewmodelDemo (res://scenes/m2a_viewmodel.tscn) and its scripted weapon timeline, registered in
# register_types.cpp under "// M2a vm".
target_sources(bf3_godot PRIVATE
    src/viewmodel.cpp
    src/viewmodel.h
    src/vm_script.cpp
    src/vm_script.h
    src/m2a_viewmodel_demo.cpp
    src/m2a_viewmodel_demo.h
    src/m2a_viewmodel_check.cpp
)

# M2a fx2 (docs/design/m2a-fx2.md): VFX2's renderer node Bf3FxRenderer, the demo Bf3FxDemo (res://scenes/fx_demo.tscn,
# --fx-check) and the GPU reference bench Bf3FxRef (res://scenes/fx_ref.tscn, --fx-ref-check).
target_sources(bf3_godot PRIVATE
    src/fx_renderer.cpp
    src/fx_renderer.h
    src/fx_demo.cpp
    src/fx_demo.h
    src/fx_ref.cpp
    src/fx_ref.h
)
target_link_libraries(bf3_godot PRIVATE bf3_fx)

# M2a hud (docs/design/m2a-hud.md): the minimal HUD (Bf3Hud and its widgets) and its check (Bf3HudCheck,
# res://scenes/m2a_hud.tscn), registered by the "// M2a hud" block of register_types.cpp.
target_sources(bf3_godot PRIVATE
    src/hud_controls.cpp
    src/hud_controls.h
    src/hud_check.cpp
    src/hud_check.h
)

# M2a audio (docs/design/m2a-audio.md §6.5, §7): Bf3AudioStream / Bf3AudioPlayback over bf3_audio's mixer core
# (registered by the A1 wave) and the audio demo node of res://scenes/audio_m16.tscn (A2 wave).
target_sources(bf3_godot PRIVATE
    src/audio_stream.cpp
    src/audio_stream.h
    src/audio_demo.cpp
    src/audio_demo.h
)
target_link_libraries(bf3_godot PRIVATE bf3_audio)

# M2a w3 (W3.0 contract, docs/design/m2a-w3.md): the wave-3 game parts' headers (Bf3GameAudio au3, Bf3GameFx fx3,
# Bf3GameCrowd tp, M2aCheck gx), the level session's wave-3 hooks (G7, gx), the 1p muzzle (G6, fx3) and a TU that
# compiles the headers together. The owners add their .cpp files in their own blocks.
target_sources(bf3_godot PRIVATE
    src/m2a_game.h
    src/game_audio.h
    src/game_fx.h
    src/game_crowd.h
    src/m2a_check.h
    src/level_session_m2a.h
    src/level_session_m2a.cpp
    src/viewmodel_muzzle.h
    src/viewmodel_muzzle.cpp
    src/m2a_w3_headers.cpp
)

# M2a tp (docs/design/m2a-tp.md): Bf3GameCrowd's body (game_crowd.cpp) and the crowd's kit item lists
# (game_crowd_items.h, Godot-free); Bf3TpCrowdCheck, the crowd's check node of res://scenes/m2a_tp_crowd.tscn.
target_sources(bf3_godot PRIVATE
    src/game_crowd.cpp
    src/game_crowd_items.h
    src/tp_crowd_check.cpp
    src/tp_crowd_check.h
)

# M2a au3 (docs/design/m2a-au3.md): Bf3GameAudio, the game's mixer, stream and player over bf3_audio's router.
target_sources(bf3_godot PRIVATE
    src/game_audio.cpp
)

# M2a fx3 (docs/design/m2a-fx3.md): Bf3GameFx (game_fx.h's body: FxWorld + Bf3FxRenderer + VFX3's triggers + the decal
# pool), the decal pool FxDecalPool and the fx3 demo / capture scene Bf3Fx3Demo (res://scenes/fx3_demo.tscn,
# --fx3-check), registered by the "// M2a fx3" block of register_types.cpp.
target_sources(bf3_godot PRIVATE
    src/game_fx.cpp
    src/fx_decals.cpp
    src/fx_decals.h
    src/fx3_demo.cpp
    src/fx3_demo.h
)

# M2a gx (docs/design/m2a-gx.md): the in-game check (M2aCheck) and the playable scene's wiring
# (level_session_m2a.cpp's hook bodies). The wave-3 parts are linked in by their owners' blocks; the hooks build them
# only when their body is in the tree (the file's presence: no edit here when tp, au3, fx3 or bt merge).
target_sources(bf3_godot PRIVATE
    src/m2a_check.cpp
)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/game_fx.cpp")
    target_compile_definitions(bf3_godot PRIVATE OPENBF3_M2A_HAVE_FX=1)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/game_audio.cpp")
    target_compile_definitions(bf3_godot PRIVATE OPENBF3_M2A_HAVE_AUDIO=1)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/game_crowd.cpp")
    target_compile_definitions(bf3_godot PRIVATE OPENBF3_M2A_HAVE_CROWD=1)
endif()
# bt: W3.0's firefight_brain.cpp is a stub that says so; bt's body does not.
set(_m2a_gx_brain "${CMAKE_CURRENT_SOURCE_DIR}/../bf3_host/src/firefight_brain.cpp")
if(EXISTS "${_m2a_gx_brain}")
    file(READ "${_m2a_gx_brain}" _m2a_gx_brain_text)
    string(FIND "${_m2a_gx_brain_text}" "W3.0 stub" _m2a_gx_stub)
    if(_m2a_gx_stub EQUAL -1)
        target_compile_definitions(bf3_godot PRIVATE OPENBF3_M2A_HAVE_BT=1)
    endif()
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_m2a_gx_brain}")

# M2b W2.0 (W2.0 contract, docs/design/m2b.md §2.6 #16-#18, §11 wave 2; docs/design/m2b-w2.md): the wave-2 headers
# (ui's Bf3TdmHud, cam's Bf3DeathCamera, gx's M2b game, session state and check) and a TU that compiles them together.
# The owners add their .cpp files in their own blocks (ui: hud_tdm.cpp and its screens, bf3_host's hud_feed_tdm.cpp;
# cam: death_camera.cpp; gx: level_session_m2b.cpp, m2b_check.cpp). gx builds ui's and cam's parts only when their body
# is in the tree: OPENBF3_M2B_HAVE_UI / OPENBF3_M2B_HAVE_CAM from the files' presence (the M2a gx pattern), so gx's
# branch links before they land and picks them up after the merge without an edit.
target_sources(bf3_godot PRIVATE
    src/hud_tdm.h
    src/death_camera.h
    src/m2b_game.h
    src/m2b_check.h
    src/level_session_m2b.h
    src/m2b_w2_headers.cpp
)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/hud_tdm.cpp")
    target_compile_definitions(bf3_godot PRIVATE OPENBF3_M2B_HAVE_UI=1)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/death_camera.cpp")
    target_compile_definitions(bf3_godot PRIVATE OPENBF3_M2B_HAVE_CAM=1)
endif()

# M2b ui (docs/design/m2b.md §4, §2.6 #16; docs/design/m2b-ui.md): Bf3TdmHud and its six screens (kill feed,
# scoreboard, spawn screen, kill card, pre-round, round end), the strings, fonts, icons and placements they look up
# (hud_tdm_text.h, Godot-free: ui.hud.tdm.install resolves the same lists). hud_tdm.cpp's presence turns on
# OPENBF3_M2B_HAVE_UI (the "M2b W2.0" block above).
target_sources(bf3_godot PRIVATE
    src/hud_tdm.cpp
    src/hud_tdm_screen.h
    src/hud_tdm_text.h
    src/hud_killfeed.h
    src/hud_killfeed.cpp
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
