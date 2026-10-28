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
target_sources(bf3_render PRIVATE
    src/collision_prep.cpp
    include/bf3/render/collision_prep.h
)

target_sources(bf3_render PRIVATE
    src/mesh_cache.cpp
    src/level_prepare.cpp
    include/bf3/render/mesh_cache.h
    include/bf3/render/level_prepare.h
)

target_sources(bf3_render PRIVATE
    include/bf3/render/level_scene.h
)

target_sources(bf3_render PRIVATE
    src/level_scene.cpp
    src/level_scene_internal.h
    src/scene_lanes.cpp
)

target_sources(bf3_render PRIVATE
    include/bf3/render/soldier_assets.h
    src/soldier_assets.cpp
)

target_sources(bf3_render PRIVATE
    src/shader_database.cpp
    src/look_materials.cpp
    src/look_environment.cpp
    include/bf3/render/shader_database.h
    include/bf3/render/look_materials.h
    include/bf3/render/look_environment.h
)

target_sources(bf3_render PRIVATE
    include/bf3/render/viewmodel_set.h
)

target_sources(bf3_render PRIVATE
    src/viewmodel_set.cpp
)

target_sources(bf3_render PRIVATE
    include/bf3/render/crowd_weapons.h
)

target_sources(bf3_render PRIVATE
    src/crowd_weapons.cpp
)

target_sources(bf3_render PRIVATE
    include/bf3/render/weapon_sockets.h
    src/weapon_sockets.cpp
)
