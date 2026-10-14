#pragma once
// UTF-8 <-> UTF-16 helpers for the Win32 file APIs (internal to bf3_core).

#include <filesystem>
#include <string>
#include <string_view>

namespace bf3::core::detail {

std::wstring widen(std::string_view p_utf8);
std::string narrow(std::wstring_view p_wide);
inline std::string path_utf8(const std::filesystem::path &p_path) { return narrow(std::wstring_view(p_path.native())); }

} // namespace bf3::core::detail
