#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace bf3::core {

struct FileStamp {
	std::string relative;
	uint64_t size = 0;
	uint64_t mtime = 0;
};

struct InstallSnapshot {
	bool valid = false;
	std::vector<FileStamp> files;
	uint64_t total_bytes = 0;
	uint64_t newest_mtime = 0;
	double scan_ms = 0.0;
	size_t toc_count = 0;
	size_t sb_count = 0;
	size_t cat_count = 0;
};

struct SnapshotDiff {
	std::vector<std::string> changed;
	std::vector<std::string> added;
	std::vector<std::string> removed;
	bool clean() const { return changed.empty() && added.empty() && removed.empty(); }
	std::string summary() const;
};

InstallSnapshot take_install_snapshot(const std::filesystem::path &p_root);
SnapshotDiff diff_snapshots(const InstallSnapshot &p_before, const InstallSnapshot &p_after);

}
