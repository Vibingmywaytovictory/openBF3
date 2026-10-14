// Read-only checks against the retail install (OPENBF3_INSTALL or the default path). Every case
// skips when no install is found. Asserts numbers only; nothing from the install is stored.
#include <doctest/doctest.h>

#include "bf3/core/install_reader.h"
#include "test_support.h"

#include <windows.h>

#include <cstring>

using bf3::core::InstallReader;
using Verdict = bf3::core::PathGuard::Verdict;

namespace {

bool mount_or_skip(InstallReader &r_reader) {
	if (!r_reader.locate({})) {
		bf3::test::skip_install_case(r_reader.describe().c_str());
		return false;
	}
	return true;
}

} // namespace

TEST_SUITE("install") {
	TEST_CASE("install: layout, patch and xpacks") {
		InstallReader reader;
		if (!mount_or_skip(reader)) {
			return;
		}
		const auto &st = reader.status();
		MESSAGE(reader.describe());
		CHECK(st.layout_ok);
		CHECK(st.has_patch);
		CHECK(st.patch_version > 0);
		for (const auto &x : st.xpacks) {
			CHECK(x.require_version > 0);
			CHECK(x.require_version <= st.patch_version);
			CHECK(x.has_data_win32);
		}
	}

	TEST_CASE("install: read-only access") {
		InstallReader reader;
		if (!mount_or_skip(reader)) {
			return;
		}
		bf3::core::ReadOnlyFile a = reader.open("Data/layout.toc");
		bf3::core::ReadOnlyFile b = reader.open("Data\\layout.toc"); // a second reader at the same time
		REQUIRE(a.is_open());
		REQUIRE(b.is_open());
		CHECK(a.size() > 16);
		uint8_t magic[4] = {};
		REQUIRE(a.read_at(0, magic, 4) == 4);
		// Obfuscated/plain toc header 00 D1 CE 0x.
		CHECK(magic[0] == 0x00);
		CHECK(magic[1] == 0xD1);
		CHECK(magic[2] == 0xCE);
		CHECK(magic[3] <= 0x01);
		CHECK(!reader.open("../bf3.exe").is_open());
		CHECK(!reader.open("Data/../../x").is_open());
		CHECK(!reader.open("C:/Windows/win.ini").is_open());
		CHECK(!reader.open("Data/layout.toc:stream").is_open());
		CHECK(!reader.open("Data/does-not-exist.toc").is_open());
	}

	TEST_CASE("install: write guard refuses the install") {
		InstallReader reader;
		if (!mount_or_skip(reader)) {
			return;
		}
		bf3::core::PathGuard guard = reader.write_guard();
		const std::filesystem::path &root = reader.root();
		CHECK(guard.check_write_target(root / L"Data" / L"Win32" / L"new.cache") == Verdict::UnderProtectedRoot);
		CHECK(guard.check_write_target(root / L"Data" / L"layout.toc") == Verdict::UnderProtectedRoot);
		CHECK(guard.check_write_target(root / L"Update" / L"Patch" / L"x" / L"y.bin") == Verdict::UnderProtectedRoot);
		wchar_t shortp[MAX_PATH];
		const DWORD n = GetShortPathNameW(root.c_str(), shortp, MAX_PATH);
		if (n > 0 && n < MAX_PATH && std::wstring(shortp) != root.native()) {
			MESSAGE("8.3 alias exercised");
			CHECK(guard.check_write_target(std::wstring(shortp) + L"\\Data\\x.bin") == Verdict::UnderProtectedRoot);
		}
		wchar_t tmp[MAX_PATH];
		GetTempPathW(MAX_PATH, tmp);
		CHECK(guard.check_write_target(std::filesystem::path(tmp) / L"openbf3-cache-probe.bin") == Verdict::Allowed);
	}

	TEST_CASE("install: snapshot is stable across reads") {
		InstallReader reader;
		if (!mount_or_skip(reader)) {
			return;
		}
		const bf3::core::InstallSnapshot before = reader.snapshot();
		// Read every byte of layout.toc and a few headers, as a session would.
		std::vector<uint8_t> bytes;
		REQUIRE(reader.open("Data/layout.toc").read_all(bytes));
		const bf3::core::InstallSnapshot after = reader.snapshot();
		REQUIRE(before.valid);
		MESSAGE("snapshot: ", before.files.size(), " files (", before.toc_count, " toc, ", before.sb_count, " sb, ", before.cat_count,
				" cat), ", before.total_bytes, " bytes, ", before.scan_ms, " ms");
		CHECK(before.toc_count > 0);
		CHECK(before.sb_count > 0);
		CHECK(before.cat_count > 0);
		const bf3::core::SnapshotDiff d = bf3::core::diff_snapshots(before, after);
		CHECK_MESSAGE(d.clean(), d.summary());
	}
}
