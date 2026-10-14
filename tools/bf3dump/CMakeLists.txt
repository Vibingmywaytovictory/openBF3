# bf3dump: read-only inspection of the BF3 install through bf3_core (no Godot).
add_executable(bf3dump bf3dump.cpp ebx_commands.cpp ebx_commands.h json_number.cpp json_number.h)
target_link_libraries(bf3dump PRIVATE bf3_core)
openbf3_module(bf3dump)
# Level traversal listing (bf3_core/level), parity with research/probes/tools/level-oracle.
target_sources(bf3dump PRIVATE level_commands.cpp level_commands.h)
