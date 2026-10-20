#include <doctest/doctest.h>

#include "bf3/core/path_guard.h"

#include <windows.h>
#include <winioctl.h>

#include <cstring>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using bf3::core::PathGuard;
using Verdict = bf3::core::PathGuard::Verdict;

namespace {

struct ScratchTree {
	fs::path base;
	fs::path root;
	fs::path file;
	fs::path outside;

	ScratchTree() {
		wchar_t tmp[MAX_PATH];
		GetTempPathW(MAX_PATH, tmp);
		base = fs::path(tmp) / (L"openbf3-guard-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
		root = base / L"root";
		outside = base / L"out";
		CreateDirectoryW(base.c_str(), nullptr);
		CreateDirectoryW(root.c_str(), nullptr);
		CreateDirectoryW((root / L"sub").c_str(), nullptr);
		CreateDirectoryW(outside.c_str(), nullptr);
		file = root / L"sub" / L"file.bin";
		HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		DWORD n = 0;
		WriteFile(h, "data", 4, &n, nullptr);
		CloseHandle(h);
	}
	~ScratchTree() {

		RemoveDirectoryW((base / L"junction").c_str());
		DeleteFileW((outside / L"hardlink.bin").c_str());
		std::error_code ec;
		fs::remove_all(base, ec);
	}
};

bool make_junction(const fs::path &p_link, const fs::path &p_target) {
	if (!CreateDirectoryW(p_link.c_str(), nullptr)) {
		return false;
	}
	HANDLE h = CreateFileW(p_link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
	if (h == INVALID_HANDLE_VALUE) {
		return false;
	}
	const std::wstring substitute = L"\\??\\" + fs::absolute(p_target).native();
	const std::wstring print = fs::absolute(p_target).native();
	const size_t sub_bytes = substitute.size() * sizeof(wchar_t);
	const size_t print_bytes = print.size() * sizeof(wchar_t);

	const size_t path_buffer = sub_bytes + 2 + print_bytes + 2;
	const size_t data_len = 8 + path_buffer;
	std::string buf(8 + data_len, '\0');
	auto put16 = [&](size_t off, uint16_t v) { std::memcpy(&buf[off], &v, 2); };
	const uint32_t tag = IO_REPARSE_TAG_MOUNT_POINT;
	std::memcpy(&buf[0], &tag, 4);
	put16(4, static_cast<uint16_t>(data_len));
	put16(8, 0);
	put16(10, static_cast<uint16_t>(sub_bytes));
	put16(12, static_cast<uint16_t>(sub_bytes + 2));
	put16(14, static_cast<uint16_t>(print_bytes));
	std::memcpy(&buf[16], substitute.data(), sub_bytes);
	std::memcpy(&buf[16 + sub_bytes + 2], print.data(), print_bytes);
	DWORD out = 0;
	const BOOL ok = DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, buf.data(), static_cast<DWORD>(buf.size()), nullptr, 0, &out, nullptr);
	CloseHandle(h);
	return ok != 0;
}

std::wstring upper(std::wstring p_s) {
	for (wchar_t &c : p_s) {
		c = static_cast<wchar_t>(towupper(c));
	}
	return p_s;
}

std::wstring forward_slashes(std::wstring p_s) {
	for (wchar_t &c : p_s) {
		c = c == L'\\' ? L'/' : c;
	}
	return p_s;
}

}

TEST_CASE("guard: a guard without roots refuses everything") {
	PathGuard g;
	CHECK(g.check_write_target(L"C:\\anything.bin") == Verdict::NoRoots);
}

TEST_CASE("guard: canonical form of aliases") {
	ScratchTree t;
	auto a = bf3::core::canonicalize_path(t.root / L"sub" / L"new.bin");
	auto b = bf3::core::canonicalize_path(forward_slashes((t.root / L"sub" / L".." / L"sub" / L"new.bin").native()));
	REQUIRE(a);
	REQUIRE(b);
	CHECK(bf3::core::path_is_within(*a, *bf3::core::canonicalize_path(t.root)));
	CHECK(bf3::core::path_is_within(*b, *a));
	CHECK(bf3::core::path_is_within(*a, *b));
}

TEST_CASE("guard: refuses every alias of a target inside the protected root") {
	ScratchTree t;
	PathGuard g;
	REQUIRE(g.add_protected_root(t.root));

	CHECK(g.check_write_target(t.root) == Verdict::UnderProtectedRoot);
	CHECK(g.check_write_target(t.file) == Verdict::UnderProtectedRoot);
	CHECK(g.check_write_target(t.root / L"sub" / L"new" / L"deeper.bin") == Verdict::UnderProtectedRoot);
	CHECK(g.check_write_target(upper(t.root.native()) + L"\\SUB\\X.BIN") == Verdict::UnderProtectedRoot);
	CHECK(g.check_write_target(forward_slashes(t.root.native()) + L"/sub/x.bin") == Verdict::UnderProtectedRoot);
	CHECK(g.check_write_target(t.outside / L".." / L"root" / L"x.bin") == Verdict::UnderProtectedRoot);
	CHECK(g.check_write_target(t.root.native() + L"\\\\sub\\\\x.bin") == Verdict::UnderProtectedRoot);
	CHECK(g.check_write_target(L"\\\\?\\" + (t.root / L"x.bin").native()) == Verdict::UnderProtectedRoot);

	CHECK(g.check_write_target(t.root.native() + L".\\x.bin") == Verdict::UnderProtectedRoot);
	CHECK(g.check_write_target(t.root.native() + L" \\x.bin") == Verdict::UnderProtectedRoot);

	CHECK(g.check_write_target(t.file.native() + L":evil") == Verdict::InvalidName);
	CHECK(g.check_write_target(t.base.native() + L"\\out\\x.bin:evil") == Verdict::InvalidName);

	wchar_t cwd[MAX_PATH];
	GetCurrentDirectoryW(MAX_PATH, cwd);
	SetCurrentDirectoryW(t.base.c_str());
	CHECK(g.check_write_target(L"root\\sub\\x.bin") == Verdict::UnderProtectedRoot);
	CHECK(g.check_write_target(L"out\\x.bin") == Verdict::Allowed);
	SetCurrentDirectoryW(cwd);

	wchar_t shortp[MAX_PATH];
	const DWORD n = GetShortPathNameW(t.root.c_str(), shortp, MAX_PATH);
	if (n > 0 && n < MAX_PATH && std::wstring(shortp) != t.root.native()) {
		CHECK(g.check_write_target(std::wstring(shortp) + L"\\x.bin") == Verdict::UnderProtectedRoot);
	} else {
		MESSAGE("no distinct 8.3 name for the scratch root; short-name alias not exercised");
	}
}

TEST_CASE("guard: allows targets outside, including look-alike prefixes") {
	ScratchTree t;
	PathGuard g;
	REQUIRE(g.add_protected_root(t.root));
	CHECK(g.check_write_target(t.outside / L"cache.bin") == Verdict::Allowed);
	CHECK(g.check_write_target(t.base / L"root2" / L"x.bin") == Verdict::Allowed);
	CHECK(g.check_write_target(t.base / L"rootx.bin") == Verdict::Allowed);
	CHECK(g.check_write_target(t.base) == Verdict::Allowed);
}

TEST_CASE("guard: resolves junctions and refuses hard links") {
	ScratchTree t;
	PathGuard g;
	REQUIRE(g.add_protected_root(t.root));
	const fs::path junction = t.base / L"junction";
	if (make_junction(junction, t.root)) {
		CHECK(g.check_write_target(junction / L"sub" / L"x.bin") == Verdict::UnderProtectedRoot);
		CHECK(g.check_write_target(junction / L"newdir" / L"x.bin") == Verdict::UnderProtectedRoot);
	} else {
		MESSAGE("could not create a junction (error ", GetLastError(), "); junction alias not exercised");
	}
	const fs::path link = t.outside / L"hardlink.bin";
	if (CreateHardLinkW(link.c_str(), t.file.c_str(), nullptr)) {
		CHECK(g.check_write_target(link) == Verdict::HardLinkedFile);
	} else {
		MESSAGE("could not create a hard link; hard-link check not exercised");
	}
}
