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
)
target_link_libraries(bf3_godot PRIVATE godot-cpp bf3_host bf3_core bf3_sim bf3_net)
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
