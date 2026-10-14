# bf3dump: read-only inspection of the BF3 install through bf3_core (no Godot).
add_executable(bf3dump bf3dump.cpp ebx_commands.cpp ebx_commands.h json_number.cpp json_number.h)
target_link_libraries(bf3dump PRIVATE bf3_core)
openbf3_module(bf3dump)
# Weapon tables (bf3_core gameplay readers).
target_sources(bf3dump PRIVATE weapon_commands.cpp weapon_commands.h)
