#pragma once
// Small helpers shared by the Godot-side classes.

#include <godot_cpp/variant/string.hpp>

#include <string>

namespace bf3::godot_side {

godot::String to_godot(const std::string &p_utf8);
std::string to_std(const godot::String &p_string);

// Value of "--<name>=<value>" among the user args (after "--"); empty if absent.
std::string user_arg_value(const char *p_name);
// True if "--<name>" (with or without a value) is among the user args.
bool has_user_arg(const char *p_name);

// One line describing the engine: "4.8-dev7 (official) c971f93e7 single".
std::string engine_summary();

} // namespace bf3::godot_side
