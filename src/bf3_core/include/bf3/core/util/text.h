#pragma once

#include <string>
#include <string_view>

namespace bf3::core {

constexpr char to_lower_ascii(char p_c) {
	return (p_c >= 'A' && p_c <= 'Z') ? static_cast<char>(p_c + ('a' - 'A')) : p_c;
}

std::string to_lower_ascii(std::string_view p_text);
bool iequals_ascii(std::string_view p_a, std::string_view p_b);
std::string_view trim(std::string_view p_text);

}
