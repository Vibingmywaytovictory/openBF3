#pragma once

#include <godot_cpp/variant/string.hpp>

#include <string>

namespace bf3::godot_side {

godot::String to_godot(const std::string &p_utf8);
std::string to_std(const godot::String &p_string);

std::string user_arg_value(const char *p_name);

bool has_user_arg(const char *p_name);

inline constexpr bool kBuildChecks = OPENBF3_BUILD_CHECKS != 0;

bool has_check_arg(const char *p_name);

std::string engine_summary();

}
