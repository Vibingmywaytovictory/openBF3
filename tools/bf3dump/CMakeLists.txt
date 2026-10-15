# bf3dump: read-only inspection of the BF3 install through bf3_core (no Godot).
add_executable(bf3dump bf3dump.cpp ebx_commands.cpp ebx_commands.h json_number.cpp json_number.h)
target_sources(bf3dump PRIVATE text_commands.cpp text_commands.h) # UITextDatabase commands
# Gameplay table inspectors (soldier, mode, kits, vehicle, scoring).
target_sources(bf3dump PRIVATE gameplay_commands.cpp gameplay_commands.h)
target_link_libraries(bf3dump PRIVATE bf3_core)
openbf3_module(bf3dump)
# Weapon tables (bf3_core gameplay readers).
target_sources(bf3dump PRIVATE weapon_commands.cpp weapon_commands.h)
# Level traversal listing (bf3_core/level), parity with research/probes/tools/level-oracle.
target_sources(bf3dump PRIVATE level_commands.cpp level_commands.h)
# Collision soup listing (bf3_core level/collision_soup.h), parity with level-oracle collision.
target_sources(bf3dump PRIVATE collision_commands.cpp collision_commands.h)
