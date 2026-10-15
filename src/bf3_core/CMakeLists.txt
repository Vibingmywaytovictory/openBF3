# bf3_core: install mount and readers. Godot-free.
# S0: InstallReader, PathGuard, install snapshot.
# M0.1: containers (layout/toc/sb/cas.cat/bundles/delta/chunks) and EBX, ported from IW4L.
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
target_include_directories(bf3_core PUBLIC include)
target_link_libraries(bf3_core PRIVATE libdeflate::libdeflate_static)
openbf3_module(bf3_core)

# Derived-data cache (plan §2.1/§2.4, gap load §6.4): digest-keyed entries under an absolute root.
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

# Ant animation banks (EA GenericData + clip codecs), clean-room from research/probes/ant-animation-probe.md
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

# Ant banks read from the install (res by name, chunk by GUID) with the research export's selection rules.
target_sources(bf3_core PRIVATE
    src/ant/ant_install_source.cpp
    include/bf3/core/ant/ant_install_source.h
)

# Resource decoders (MeshSet, DxTexture, Havok physics) over caller-supplied bytes; see
# include/bf3/core/res/res_types.h. Port of IW4L resource_bf3.
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

# Resource loading from a LevelMount (res payload + resMeta + chunks -> decoded MeshSet / DxTexture /
# Havok); see include/bf3/core/res/res_loader.h. The API the Godot bridge calls.
target_sources(bf3_core PRIVATE
    src/res/res_loader.cpp
    include/bf3/core/res/res_loader.h
)

# Gameplay table readers: WeaponDef resolver (variants, modifiers, gun sway, projectiles) and the
# level material grid / soldier hit capsules (plan §3.2, topics/01). Reads through EbxResolver.
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

# UI readers (plan §2.2, §3.8): the UITextDatabase string table and the EBX SID census, clean-room from
# research/topics/gap-r1-ui-hud-minimap-localisation.md §4.
target_sources(bf3_core PRIVATE
    src/ui/text_db.cpp
    src/ui/text_census.cpp
    include/bf3/core/ui/text_db.h
    include/bf3/core/ui/text_census.h
)

# The WeaponDef text block and the weapon sources/catalog shared by bf3dump and the in-game inspector
# (plan §6 M0 exit: the inspector prints the resolved M16A3/M16A4-burst WeaponDef and its localised name).
target_sources(bf3_core PRIVATE
    src/gameplay/weapon_format.cpp
    src/gameplay/weapon_catalog.cpp
    include/bf3/core/gameplay/weapon_format.h
    include/bf3/core/gameplay/weapon_catalog.h
)

# Level traversal (plan §2.2): one level x mode's bundles, the placed objects in Frostbite space, the
# mesh census and the collision placements. Port of IW4L level_bf3; see include/bf3/core/level/.
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

# Gameplay tables (plan §3.1, §3.4, §3.7): soldier, kits, mode constants resolved through the level's
# interfaces (logic_world, ported from IW4L level_bf3), minimal vehicle defs.
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

# Collision soup (M0 exit "Metro collision drawn as a debug overlay"): the level's Havok collision as
# world-space triangles with material, surface class and body kind, in 64 m tiles; see
# include/bf3/core/level/collision_soup.h; the terrain heightfield (res/terrain.h). Parts ported from IW4L
# level_bf3 collision/ and resource_bf3 terrain.rs.
target_sources(bf3_core PRIVATE
    src/level/collision_material.cpp
    src/level/collision_soup.cpp
    include/bf3/core/level/collision_soup.h
    src/res/terrain.cpp
    include/bf3/core/res/terrain.h
)

# Converters through the derived-data cache (plan §2.4 step 3, M0 "per-item streaming converters"): the blob
# reader/writer, cached_convert, and the WeaponDef blob with its dependency-list keys. Keys carry the pinned
# engine API (the engine-api/<version>.<hash> folder name), so the game and bf3dump agree on them.
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

# M1 level inputs (docs/design/m1.md §10, §12): the mode's spawn points and the level source digest.
target_sources(bf3_core PRIVATE
    include/bf3/core/level/spawn_points.h
    include/bf3/core/level/source_digest.h
)

# M1 T1 (world; docs/design/m1.md §5.5, §15): the level source digest (the "jolt-world" cache key). The
# collision_soup.{h,cpp} edit (kCollisionSoupVersion, CollisionSoup::sources) is in place in the soup block.
target_sources(bf3_core PRIVATE
    src/level/source_digest.cpp
)

# M1 T2 (motor; docs/design/m1.md §10.1): the AlternateSpawn points and CharacterSpawnReference spawners of a level.
target_sources(bf3_core PRIVATE
    src/level/spawn_points.cpp
)

# S1 nav (spike S1; docs/design/s1.md §4, §11): the combat areas and objectives of a level x mode.
target_sources(bf3_core PRIVATE
    include/bf3/core/level/mode_areas.h
    src/level/mode_areas.cpp
)
