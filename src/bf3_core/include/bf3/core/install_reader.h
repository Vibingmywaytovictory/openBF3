#pragma once
// InstallReader: the ONLY code that resolves paths of the retail BF3 install (plan §2.1).
// The install is read as-is, in place: files are opened with GENERIC_READ and full sharing, no
// write handle is ever created, and write_guard() protects the root for any code that writes.

#include "bf3/core/install_snapshot.h"
#include "bf3/core/path_guard.h"
#include "bf3/core/read_only_file.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core {

// Where to look, in priority order: command line, environment, project setting, default.
// The first non-empty candidate is used; an invalid override is reported, not skipped.
struct InstallLocateOptions {
	std::string cmdline_path; // e.g. from "--bf3-install=<dir>"
	std::string setting_path; // e.g. ProjectSettings "openbf3/install/path"
	bool use_environment = true; // OPENBF3_INSTALL
	bool use_default = true; // C:\Program Files\EA Games\Battlefield 3
};

struct XpackInfo {
	std::string folder; // "Xpack1"
	std::string name; // package.mft "Name"
	int mount_order = -1;
	int require_version = -1;
	bool has_data_win32 = false;
};

struct InstallStatus {
	bool located = false; // a candidate directory exists
	bool layout_ok = false; // Data/layout.toc and Data/cas.cat present (layout.toc is not under Win32)
	std::string root_utf8; // canonical root
	std::string source; // "cmdline" | "env" | "setting" | "default"
	bool has_layout_toc = false;
	bool has_cas_cat = false;
	bool has_patch = false;
	bool patch_authoritative = false;
	int patch_version = -1;
	std::vector<XpackInfo> xpacks; // sorted by folder name
	std::vector<std::string> problems;
};

class InstallReader {
public:
	static constexpr const char *kEnvVar = "OPENBF3_INSTALL";
	static constexpr const wchar_t *kDefaultRoot = L"C:\\Program Files\\EA Games\\Battlefield 3";

	// Picks a candidate (see InstallLocateOptions) and mounts it.
	bool locate(const InstallLocateOptions &p_options);
	// Verifies the layout under p_root. Returns status().layout_ok.
	bool mount(const std::filesystem::path &p_root, std::string p_source);

	bool is_mounted() const { return status_.layout_ok; }
	const InstallStatus &status() const { return status_; }
	const std::filesystem::path &root() const { return root_; }
	std::string describe() const; // one line for logs and overlays

	// Install-relative access ('/' or '\' separated; absolute names, "..", ':' are refused).
	bool exists(std::string_view p_relative) const;
	ReadOnlyFile open(std::string_view p_relative) const;
	// Names of the sub-directories of an install-relative directory, sorted (byte order).
	std::vector<std::string> list_subdirectories(std::string_view p_relative) const;

	// A guard that refuses every write target inside this install and inside kDefaultRoot, so it is
	// never empty even when nothing is mounted.
	PathGuard write_guard() const;
	InstallSnapshot snapshot() const;

private:
	bool resolve(std::string_view p_relative, std::filesystem::path &r_path) const;
	void read_manifest(std::string_view p_relative, std::vector<std::pair<std::string, std::string>> &r_pairs) const;

	std::filesystem::path root_;
	InstallStatus status_;
};

} // namespace bf3::core
