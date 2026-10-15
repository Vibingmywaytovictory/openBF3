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
