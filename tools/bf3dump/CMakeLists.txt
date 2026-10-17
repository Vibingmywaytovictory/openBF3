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
# S4 logic (docs/design/s4.md §9.4): `logic instantiate|stats|ports|fields|run|trace|census` (bf3_sim from M1 T1's block).
target_sources(bf3dump PRIVATE logic_commands.cpp logic_commands.h)
# L1 look (README "L1 look"): `looks <LEVEL> <MODE>`, the drawn-material census (bf3_render from the prepare block).
target_sources(bf3dump PRIVATE look_commands.cpp look_commands.h)
# S1 (server scale; docs/design/s1.md §12): `nav bake|stats|components|path|probe` and `bots`, the navmesh, its
# components and the 64-walker scale bench (bf3_host for the bot level and run_scale_bench).
target_sources(bf3dump PRIVATE nav_commands.cpp nav_commands.h)
target_link_libraries(bf3dump PRIVATE bf3_host)
# S2 vehicle (docs/design/s2.md §1, §8): `vehicledrive` and `vehicle-replay` (bf3_sim from M1 T1's block above).
target_sources(bf3dump PRIVATE vehicle_commands.cpp vehicle_commands.h)
# UI ui1 (docs/design/ui-swf-fonts.md §12.4): `swf census | movie | labels | layout | hud | fonts`.
target_sources(bf3dump PRIVATE swf_commands.cpp swf_commands.h)
# UI ui2 (docs/design/ui-minimap-icons.md §15): `minimap`, `hud-icons`, `item-icons`, `hud-sources`.
target_sources(bf3dump PRIVATE ui_commands.cpp ui_commands.h)
# M2a weapons (docs/design/m2a-weapons.md §10): `weapon-sim` and `range`, the weapon simulation of bf3_sim on defs
# resolved from the install (bf3_sim from M1 T1's block above).
target_sources(bf3dump PRIVATE weapon_sim_commands.cpp weapon_sim_commands.h)
# M2a VFX1 (docs/design/m2a-vfx1.md §7.7): `fx census | compile | simulate | check | bench | atlas | impacts`.
target_sources(bf3dump PRIVATE fx_commands.cpp fx_commands.h)
target_link_libraries(bf3dump PRIVATE bf3_fx)
# M2a lc (docs/design/m2a.md §6, E13, E18; docs/design/m2a-lc.md): `lagcomp <LEVEL> <MODE> [--deviation] [--perf]`, the
# capsule pose table from the install, the E18 (a) deviation rows and the E13 rewind rows (bf3_host from the nav block).
target_sources(bf3dump PRIVATE lagcomp_commands.cpp lagcomp_commands.h)
target_link_libraries(bf3dump PRIVATE bf3_anim)
# M2a vm (docs/design/m2a-vm.md): `vm clips`, the 1p viewmodel clip roles, E5 reload rows and E11 coverage (bf3_render
# from the prepare block above).
target_sources(bf3dump PRIVATE vm_commands.cpp vm_commands.h)
