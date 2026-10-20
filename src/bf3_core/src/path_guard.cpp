#include "bf3/core/path_guard.h"

#include <windows.h>

#include <deque>
#include <string>

namespace bf3::core {

namespace {

std::wstring full_path_name(const std::wstring &p_in) {
	DWORD need = GetFullPathNameW(p_in.c_str(), 0, nullptr, nullptr);
	if (need == 0) {
		return {};
	}
	std::wstring out(need, L'\0');
	DWORD got = GetFullPathNameW(p_in.c_str(), need, out.data(), nullptr);
	if (got == 0 || got >= need) {
		return {};
	}
	out.resize(got);
	return out;
}

std::wstring strip_verbatim(std::wstring p_path) {
	if (p_path.rfind(L"\\\\?\\UNC\\", 0) == 0) {
		return L"\\\\" + p_path.substr(8);
	}
	if (p_path.rfind(L"\\\\?\\", 0) == 0) {
		return p_path.substr(4);
	}
	return p_path;
}

}

std::filesystem::path to_extended_length(const std::filesystem::path &p_path) {
	const std::wstring &raw = p_path.native();
	if (raw.rfind(L"\\\\?\\", 0) == 0) {
		return p_path;
	}
	const std::wstring full = full_path_name(raw);
	if (full.empty()) {
		return p_path;
	}
	if (full.rfind(L"\\\\", 0) == 0) {
		return std::filesystem::path(L"\\\\?\\UNC\\" + full.substr(2));
	}
	return std::filesystem::path(L"\\\\?\\" + full);
}

namespace {

bool exists_no_follow_fail(const std::wstring &p_path) {
	return GetFileAttributesW(p_path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::wstring final_path(const std::wstring &p_existing) {

	HANDLE h = CreateFileW(p_existing.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
	if (h == INVALID_HANDLE_VALUE) {

		DWORD need = GetLongPathNameW(p_existing.c_str(), nullptr, 0);
		if (need == 0) {
			return p_existing;
		}
		std::wstring out(need, L'\0');
		DWORD got = GetLongPathNameW(p_existing.c_str(), out.data(), need);
		out.resize(got < need ? got : 0);
		return out.empty() ? p_existing : out;
	}
	std::wstring out(512, L'\0');
	DWORD got = GetFinalPathNameByHandleW(h, out.data(), static_cast<DWORD>(out.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
	if (got >= out.size()) {
		out.resize(got + 1);
		got = GetFinalPathNameByHandleW(h, out.data(), static_cast<DWORD>(out.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
	}
	CloseHandle(h);
	if (got == 0 || got >= out.size()) {
		return p_existing;
	}
	out.resize(got);
	return strip_verbatim(out);
}

bool component_equal(const std::wstring &p_a, const std::wstring &p_b) {
	return CompareStringOrdinal(p_a.c_str(), static_cast<int>(p_a.size()), p_b.c_str(), static_cast<int>(p_b.size()), TRUE) ==
			CSTR_EQUAL;
}

bool has_stream_syntax(const std::wstring &p_full) {
	size_t start = 0;
	if (p_full.rfind(L"\\\\?\\", 0) == 0 || p_full.rfind(L"\\\\.\\", 0) == 0) {
		start = 4;
	}
	if (p_full.size() >= start + 2 && p_full[start + 1] == L':') {
		start += 2;
	}
	return p_full.find(L':', start) != std::wstring::npos;
}

}

std::optional<std::filesystem::path> canonicalize_path(const std::filesystem::path &p_path) {
	if (p_path.empty()) {
		return std::nullopt;
	}
	std::wstring full = full_path_name(p_path.native());
	if (full.empty()) {
		return std::nullopt;
	}
	full = strip_verbatim(full);

	std::filesystem::path existing(full);
	std::deque<std::filesystem::path> rest;
	while (!exists_no_follow_fail(existing.native())) {
		const std::filesystem::path parent = existing.parent_path();
		if (parent == existing || parent.empty()) {
			break;
		}
		rest.push_front(existing.filename());
		existing = parent;
	}
	std::filesystem::path out = exists_no_follow_fail(existing.native()) ? std::filesystem::path(final_path(existing.native())) : existing;
	for (const std::filesystem::path &c : rest) {
		out /= c;
	}

	std::wstring s = out.native();
	while (s.size() > 3 && (s.back() == L'\\' || s.back() == L'/')) {
		s.pop_back();
	}
	return std::filesystem::path(s);
}

bool path_is_within(const std::filesystem::path &p_child, const std::filesystem::path &p_root) {
	auto c = p_child.begin();
	for (auto r = p_root.begin(); r != p_root.end(); ++r) {
		if (r->empty()) {
			continue;
		}
		if (c == p_child.end()) {
			return false;
		}

		if (!component_equal(r->native(), c->native())) {
			return false;
		}
		++c;
	}
	return true;
}

bool PathGuard::add_protected_root(const std::filesystem::path &p_root) {
	std::optional<std::filesystem::path> canonical = canonicalize_path(p_root);
	if (!canonical) {
		return false;
	}
	roots_.push_back(*canonical);
	return true;
}

PathGuard::Verdict PathGuard::check_write_target(const std::filesystem::path &p_target) const {
	if (roots_.empty()) {
		return Verdict::NoRoots;
	}
	if (p_target.empty()) {
		return Verdict::InvalidName;
	}
	const std::wstring full = full_path_name(p_target.native());
	if (full.empty()) {
		return Verdict::Unresolvable;
	}
	if (has_stream_syntax(full) || full.rfind(L"\\\\.\\", 0) == 0) {
		return Verdict::InvalidName;
	}
	std::optional<std::filesystem::path> canonical = canonicalize_path(p_target);
	if (!canonical) {
		return Verdict::Unresolvable;
	}
	for (const std::filesystem::path &root : roots_) {
		if (path_is_within(*canonical, root)) {
			return Verdict::UnderProtectedRoot;
		}
	}

	HANDLE h = CreateFileW(canonical->c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
			OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
	if (h != INVALID_HANDLE_VALUE) {
		BY_HANDLE_FILE_INFORMATION info{};
		const bool ok = GetFileInformationByHandle(h, &info) != 0;
		CloseHandle(h);
		if (ok && !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && info.nNumberOfLinks > 1) {
			return Verdict::HardLinkedFile;
		}
	}
	return Verdict::Allowed;
}

const char *PathGuard::verdict_name(Verdict p_verdict) {
	switch (p_verdict) {
		case Verdict::Allowed:
			return "allowed";
		case Verdict::UnderProtectedRoot:
			return "refused: under protected root";
		case Verdict::HardLinkedFile:
			return "refused: hard-linked file";
		case Verdict::InvalidName:
			return "refused: invalid name";
		case Verdict::Unresolvable:
			return "refused: unresolvable";
		case Verdict::NoRoots:
			return "refused: guard has no roots";
	}
	return "?";
}

}
