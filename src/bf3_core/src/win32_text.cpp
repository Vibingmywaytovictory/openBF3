#include "win32_text.h"

#include <windows.h>

namespace bf3::core::detail {

std::wstring widen(std::string_view p_utf8) {
	if (p_utf8.empty()) {
		return {};
	}
	const int n = MultiByteToWideChar(CP_UTF8, 0, p_utf8.data(), static_cast<int>(p_utf8.size()), nullptr, 0);
	std::wstring out(static_cast<size_t>(n > 0 ? n : 0), L'\0');
	if (n > 0) {
		MultiByteToWideChar(CP_UTF8, 0, p_utf8.data(), static_cast<int>(p_utf8.size()), out.data(), n);
	}
	return out;
}

std::string narrow(std::wstring_view p_wide) {
	if (p_wide.empty()) {
		return {};
	}
	const int n = WideCharToMultiByte(CP_UTF8, 0, p_wide.data(), static_cast<int>(p_wide.size()), nullptr, 0, nullptr, nullptr);
	std::string out(static_cast<size_t>(n > 0 ? n : 0), '\0');
	if (n > 0) {
		WideCharToMultiByte(CP_UTF8, 0, p_wide.data(), static_cast<int>(p_wide.size()), out.data(), n, nullptr, nullptr);
	}
	return out;
}

}
