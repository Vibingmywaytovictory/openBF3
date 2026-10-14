#pragma once
// Read-only self-test of the install (plan §2.1): size and last-write time of Data/layout.toc, cas.cat,
// package.mft and every .toc/.sb under Data/Win32 and Update/*/Data/Win32, recorded before and after
// a session. Any difference means something wrote to the install.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace bf3::core {

struct FileStamp {
	std::string relative; // install-relative, '/' separated, as found on disk
	uint64_t size = 0;
	uint64_t mtime = 0; // FILETIME (100 ns since 1601, UTC)
};

struct InstallSnapshot {
	bool valid = false;
	std::vector<FileStamp> files; // sorted by relative name
	uint64_t total_bytes = 0;
	uint64_t newest_mtime = 0;
	double scan_ms = 0.0;
	size_t toc_count = 0;
	size_t sb_count = 0;
	size_t cat_count = 0;
};

struct SnapshotDiff {
	std::vector<std::string> changed; // size or mtime differs
	std::vector<std::string> added;
	std::vector<std::string> removed;
	bool clean() const { return changed.empty() && added.empty() && removed.empty(); }
	std::string summary() const;
};

// Scans the canonical install root. Uses directory enumeration only (no file handles).
InstallSnapshot take_install_snapshot(const std::filesystem::path &p_root);
SnapshotDiff diff_snapshots(const InstallSnapshot &p_before, const InstallSnapshot &p_after);

} // namespace bf3::core
