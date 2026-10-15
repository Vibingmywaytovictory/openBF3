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
