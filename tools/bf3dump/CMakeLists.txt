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
# Derived-data cache preparation of one level x mode (bf3_render level_prepare.h).
target_sources(bf3dump PRIVATE prepare_commands.cpp prepare_commands.h)
target_link_libraries(bf3dump PRIVATE bf3_render psapi)
# M1 T1 (world; docs/design/m1.md §15): bf3dump links bf3_sim for the static-world commands (`shapes`, `prepare
# --shapes`, T1 phase 2) and T2's `spawns`.
target_link_libraries(bf3dump PRIVATE bf3_sim)
target_sources(bf3dump PRIVATE shapes_commands.cpp shapes_commands.h) # M1 T1: `shapes`, `prepare --shapes`
# M1 T2 (motor; docs/design/m1.md §10.1): `spawns <LEVEL> <MODE>`, the spawn points and spawners as TSV (bf3_sim from
# T1's block above).
target_sources(bf3dump PRIVATE spawns_commands.cpp spawns_commands.h)
# S4 logic (docs/design/s4.md �9.4): `logic instantiate|stats|ports|fields|run|trace|census` (bf3_sim from M1 T1's block).
target_sources(bf3dump PRIVATE logic_commands.cpp logic_commands.h)
