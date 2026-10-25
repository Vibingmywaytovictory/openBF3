add_library(bf3_core STATIC
    src/bundle_manifest.cpp
    src/db_object.cpp
    src/db_record.h
    src/delta_bundle.cpp
    src/ebx.cpp
    src/ebx_db.cpp
    src/format_errors.h
    src/ids.cpp
    src/install.cpp
    src/install_reader.cpp
    src/install_snapshot.cpp
    src/path_guard.cpp
    src/payload.cpp
    src/read_only_file.cpp
    src/res_types.cpp
    src/sb_formats.cpp
    src/sha1_digest.cpp
    src/superbundle_mount.cpp
    src/thread_pool.cpp
    src/win32_text.cpp
    src/win32_text.h
    include/bf3/core/bundle_manifest.h
    include/bf3/core/db_object.h
    include/bf3/core/delta_bundle.h
    include/bf3/core/ebx.h
    include/bf3/core/ebx_db.h
    include/bf3/core/ids.h
    include/bf3/core/install.h
    include/bf3/core/install_file.h
    include/bf3/core/install_reader.h
    include/bf3/core/install_snapshot.h
    include/bf3/core/path_guard.h
    include/bf3/core/payload.h
    include/bf3/core/read_only_file.h
    include/bf3/core/res_types.h
    include/bf3/core/result.h
    include/bf3/core/sb_formats.h
    include/bf3/core/sha1_digest.h
    include/bf3/core/superbundle_mount.h
    include/bf3/core/thread_pool.h
)
target_sources(bf3_core PRIVATE
    src/math.cpp
    src/util/elapsed_win32.cpp
    src/util/format.cpp
    src/util/key_values.cpp
    src/util/text.cpp
    include/bf3/core/math.h
    include/bf3/core/util/elapsed.h
    include/bf3/core/util/finite.h
    include/bf3/core/util/format.h
    include/bf3/core/util/key_values.h
    include/bf3/core/util/percentile.h
    include/bf3/core/util/splitmix64.h
    include/bf3/core/util/text.h
)
target_include_directories(bf3_core PUBLIC include)
target_link_libraries(bf3_core PRIVATE libdeflate::libdeflate_static)
openbf3_module(bf3_core)

target_sources(bf3_core PRIVATE
    src/cache/cache_fs.h
    src/cache/cache_fs_win32.cpp
    src/cache/cache_journal.h
    src/cache/cache_journal.cpp
    src/cache/cache_key.cpp
    src/cache/digest_win32.cpp
    src/cache/disk_cache.cpp
    include/bf3/core/cache/cache_key.h
    include/bf3/core/cache/digest.h
    include/bf3/core/cache/disk_cache.h
    include/bf3/core/cache/entry_format.h
)
target_link_libraries(bf3_core PRIVATE bcrypt)

target_sources(bf3_core PRIVATE
    src/ant/gd_archive.cpp
    src/ant/ant_package.cpp
    src/ant/ant_clip.cpp
    src/ant/ant_library.cpp
    src/ant/ant_source.cpp
    include/bf3/core/ant/gd_archive.h
    include/bf3/core/ant/ant_package.h
    include/bf3/core/ant/ant_clip.h
    include/bf3/core/ant/ant_library.h
    include/bf3/core/ant/ant_source.h
)

target_sources(bf3_core PRIVATE
    src/ant/ant_install_source.cpp
    include/bf3/core/ant/ant_install_source.h
)

target_sources(bf3_core PRIVATE
    src/res/res_common.cpp
    src/res/res_reader.h
    src/res/mesh_set.cpp
    src/res/dx_texture.cpp
    src/res/havok.cpp
    src/res/havok_geometry.cpp
    include/bf3/core/res/res_types.h
    include/bf3/core/res/res_math.h
    include/bf3/core/res/mesh_set.h
    include/bf3/core/res/dx_texture.h
    include/bf3/core/res/havok.h
)

target_sources(bf3_core PRIVATE
    src/res/res_loader.cpp
    include/bf3/core/res/res_loader.h
)

target_sources(bf3_core PRIVATE
    src/gameplay/ebx_fields.h
    src/gameplay/ebx_fields.cpp
    src/gameplay/weapon_readers.h
    src/gameplay/weapon_def.cpp
    src/gameplay/weapon_firing.cpp
    src/gameplay/weapon_projectile.cpp
    src/gameplay/weapon_sway.cpp
    src/gameplay/weapon_resolver.cpp
    src/gameplay/material_grid.cpp
    include/bf3/core/gameplay/weapon_def.h
    include/bf3/core/gameplay/weapon_resolver.h
    include/bf3/core/gameplay/material_grid.h
)

target_sources(bf3_core PRIVATE
    src/ui/text_db.cpp
    src/ui/text_census.cpp
    include/bf3/core/ui/text_db.h
    include/bf3/core/ui/text_census.h
)

target_sources(bf3_core PRIVATE
    src/gameplay/weapon_format.cpp
    src/gameplay/weapon_catalog.cpp
    include/bf3/core/gameplay/weapon_format.h
    include/bf3/core/gameplay/weapon_catalog.h
)

target_sources(bf3_core PRIVATE
    src/level/level_transform.cpp
    src/level/budget.cpp
    src/level/connections.cpp
    src/level/level.cpp
    src/level/world.cpp
    src/level/census.cpp
    include/bf3/core/level/level_transform.h
    include/bf3/core/level/budget.h
    include/bf3/core/level/connections.h
    include/bf3/core/level/level.h
    include/bf3/core/level/world.h
    include/bf3/core/level/census.h
)

target_sources(bf3_core PRIVATE
    src/gameplay/gameplay_read.h
    src/gameplay/logic_world.cpp
    src/gameplay/soldier_def.cpp
    src/gameplay/kit_def.cpp
    src/gameplay/mode_def.cpp
    src/gameplay/vehicle_def.cpp
    include/bf3/core/gameplay/logic_world.h
    include/bf3/core/gameplay/soldier_def.h
    include/bf3/core/gameplay/kit_def.h
    include/bf3/core/gameplay/mode_def.h
    include/bf3/core/gameplay/vehicle_def.h
    include/bf3/core/gameplay/work_budget.h
)

target_sources(bf3_core PRIVATE
    src/level/collision_material.cpp
    src/level/collision_soup.cpp
    include/bf3/core/level/collision_soup.h
    src/level/soup_terrain.cpp
    src/level/soup_terrain_internal.h
    src/res/terrain.cpp
    include/bf3/core/res/terrain.h
)

get_filename_component(OPENBF3_ENGINE_ID "${OPENBF3_ENGINE_API}" NAME)
target_sources(bf3_core PRIVATE
    src/cache/blob_io.cpp
    src/cache/cached_convert.cpp
    src/gameplay/weapon_blob.cpp
    include/bf3/core/cache/blob_io.h
    include/bf3/core/cache/cached_convert.h
    include/bf3/core/gameplay/weapon_blob.h
)
set_source_files_properties(src/cache/cached_convert.cpp PROPERTIES COMPILE_DEFINITIONS "OPENBF3_ENGINE_ID=\"${OPENBF3_ENGINE_ID}\"")

target_sources(bf3_core PRIVATE
    include/bf3/core/level/spawn_points.h
    include/bf3/core/level/source_digest.h
)

target_sources(bf3_core PRIVATE
    src/level/source_digest.cpp
)

target_sources(bf3_core PRIVATE
    src/level/spawn_points.cpp
)

target_sources(bf3_core PRIVATE
    src/gameplay/logic_types.cpp
    src/gameplay/logic_graph.cpp
    include/bf3/core/gameplay/logic_types.h
    include/bf3/core/gameplay/logic_graph.h
)

target_sources(bf3_core PRIVATE
    include/bf3/core/worker_group.h
    src/worker_group.cpp
)

target_sources(bf3_core PRIVATE
    include/bf3/core/level/mode_areas.h
    src/level/mode_areas.cpp
)

target_sources(bf3_core PRIVATE
    include/bf3/core/gameplay/vehicle_drive.h
    include/bf3/core/level/vehicle_spawns.h
    src/gameplay/vehicle_drive.cpp
    src/level/vehicle_spawns.cpp
)

target_sources(bf3_core PRIVATE
    include/bf3/core/ui/swf_movie.h
    include/bf3/core/ui/swf_library.h
    include/bf3/core/ui/swf_census.h
    include/bf3/core/ui/swf_atlas.h
    include/bf3/core/ui/swf_font.h
    include/bf3/core/ui/font_ttf.h
    include/bf3/core/ui/ui_fonts.h
    include/bf3/core/ui/swf_layout.h
    include/bf3/core/ui/swf_draw.h
    include/bf3/core/ui/swf_geometry.h
    include/bf3/core/ui/swf_image.h
    include/bf3/core/ui/swf_scene.h
    include/bf3/core/ui/swf_text.h
)
target_sources(bf3_core PRIVATE
    src/ui/swf_bits.h
    src/ui/swf_font_detail.h
    src/ui/swf_movie.cpp
    src/ui/swf_shape_detail.h
    src/ui/swf_shape.cpp
    src/ui/swf_draw.cpp
    src/ui/swf_geometry.cpp
    src/ui/swf_image.cpp
    src/ui/swf_actions.cpp
    src/ui/swf_display.cpp
    src/ui/swf_scene.cpp
    src/ui/swf_text.cpp
    src/ui/swf_library.cpp
    src/ui/swf_font.cpp
    src/ui/font_ttf_write.cpp
    src/ui/font_ttf_read.cpp
    src/ui/swf_census.cpp
    src/ui/swf_atlas.cpp
    src/ui/ui_fonts.cpp
    src/ui/swf_layout.cpp
)
target_sources(bf3_core PRIVATE
    include/bf3/core/ui/avm1.h
    src/ui/avm1.cpp
    src/ui/avm1_movie.cpp
    src/ui/avm1_format.cpp
)

target_sources(bf3_core PRIVATE
    include/bf3/core/ui/minimap.h
    include/bf3/core/ui/hud_icons.h
    include/bf3/core/ui/hud_sources.h
    include/bf3/core/ui/hud_model.h
    src/ui/ui_ebx.h
    src/ui/minimap.cpp
    src/ui/minimap_images.cpp
    src/ui/hud_icons.cpp
    src/ui/item_icons.cpp
    src/ui/hud_sources.cpp
    src/ui/hud_model.cpp
    include/bf3/core/ui/as2_clip.h
    include/bf3/core/ui/as2_clip_loader.h
    include/bf3/core/ui/hud_screen.h
    include/bf3/core/ui/hud_widgets.h
    include/bf3/core/ui/hud_messages.h
    src/ui/as2_clip.cpp
    src/ui/as2_clip_loader.cpp
    src/ui/hud_screen.cpp
    src/ui/hud_widgets.cpp
    src/ui/hud_messages.cpp
    include/bf3/core/ui/hud_widget_registry.h
    src/ui/hud_widget_registry.cpp
    src/ui/widgets/hud_widget_support.h
    src/ui/widgets/image_manager.h
)
openbf3_registry(bf3_core src/ui/widgets bf3/core/ui/hud_widget_registry.h HudWidgetRegistry bf3::core::ui register_hud_widgets_ register_hud_widget_files)

target_sources(bf3_core PRIVATE
    include/bf3/core/gameplay/grid_pairs.h
    src/gameplay/grid_pairs.cpp
)

target_sources(bf3_core PRIVATE
    include/bf3/core/gameplay/capsule_pose_table.h
)

target_sources(bf3_core PRIVATE
    src/gameplay/capsule_pose_table.cpp
)

target_sources(bf3_core PRIVATE
    src/audio/sound_program.cpp
    src/audio/sound_program_blob.cpp
    src/audio/sound_stream.cpp
    src/audio/sound_types.cpp
    src/audio/sound_wave.cpp
    src/audio/wave_bank.cpp
    include/bf3/core/audio/sound_ports.h
    include/bf3/core/audio/sound_program.h
    include/bf3/core/audio/sound_stream.h
    include/bf3/core/audio/sound_types.h
    include/bf3/core/audio/sound_wave.h
    include/bf3/core/audio/wave_bank.h
)

target_sources(bf3_core PRIVATE
    include/bf3/core/audio/hdr_setting.h
    include/bf3/core/audio/sound_scope.h
)

target_sources(bf3_core PRIVATE
    src/audio/hdr_setting.cpp
    src/audio/sound_scope.cpp
)

target_sources(bf3_core PRIVATE
    src/ui/nametags.cpp
    include/bf3/core/ui/nametags.h
    src/gameplay/kit_appearance.cpp
    include/bf3/core/gameplay/kit_appearance.h
)
