# bf3_render: Godot-free preparation for the Godot render bridge (plan §2.2 bf3_godot "RS bridge, material
# families, viewmodel"; §2.5): MeshSet -> ArrayMesh-layout arrays, DxTexture -> upload-ready mip chains, MeshAsset
# materials through the MeshVariationDatabase, and the 1P viewmodel asset set. Runs on loader threads; the
# Godot side (src/bf3_godot) only creates resources from its output.
add_library(bf3_render STATIC
    src/mesh_prep.cpp
    src/texture_prep.cpp
    src/material_lookup.cpp
    src/viewmodel_assets.cpp
    include/bf3/render/mesh_prep.h
    include/bf3/render/texture_prep.h
    include/bf3/render/material_lookup.h
    include/bf3/render/viewmodel_assets.h
)
target_include_directories(bf3_render PUBLIC include)
target_link_libraries(bf3_render PUBLIC bf3_core bf3_anim)
openbf3_module(bf3_render)
# Collision soup -> per-(tile, layer) vertex arrays for the Godot collision overlay (README "Collision overlay").
target_sources(bf3_render PRIVATE
    src/collision_prep.cpp
    include/bf3/render/collision_prep.h
)

# Prepared meshes through the derived-data cache and `bf3dump prepare` (plan §2.4 step 3, §3.12).
target_sources(bf3_render PRIVATE
    src/mesh_cache.cpp
    src/level_prepare.cpp
    include/bf3/render/mesh_cache.h
    include/bf3/render/level_prepare.h
)

# M1 level scene (docs/design/m1.md §11): the plan of RS meshes, materials, textures and instances.
target_sources(bf3_render PRIVATE
    include/bf3/render/level_scene.h
)

# M1 T5: the level scene plan, its items and the scene lanes (docs/design/m1.md §11.2-§11.4, §12.2).
target_sources(bf3_render PRIVATE
    src/level_scene.cpp
    src/level_scene_internal.h
    src/scene_lanes.cpp
)

# S3 crowd (docs/design/s3.md §4, §6, §15): the 3p US / RU soldier asset set and the shoulder pinch metric.
target_sources(bf3_render PRIVATE
    include/bf3/render/soldier_assets.h
    src/soldier_assets.cpp
)

# L1 look (README "L1 look"): the level shader database and the material resolution through it.
target_sources(bf3_render PRIVATE
    src/shader_database.cpp
    src/look_materials.cpp
    src/look_environment.cpp
    include/bf3/render/shader_database.h
    include/bf3/render/look_materials.h
    include/bf3/render/look_environment.h
)

# M2a w2 (W2.0 contract, docs/design/m2a.md §11.1, §15): a kit's 1p viewmodels (vm).
target_sources(bf3_render PRIVATE
    include/bf3/render/viewmodel_set.h
)
