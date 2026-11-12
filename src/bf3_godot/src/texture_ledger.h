#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace bf3::godot_side {

void ledger_texture(std::string_view p_path, std::string_view p_name, uint64_t p_bytes);
std::string texture_ledger_text();

}
