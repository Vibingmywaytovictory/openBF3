#pragma once

#include "bf3/core/install_snapshot.h"
#include "bf3/core/path_guard.h"
#include "bf3/core/read_only_file.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core {

struct InstallLocateOptions {
	std::string cmdline_path;
	std::string setting_path;
	bool use_environment = true;
	bool use_default = true;
};

struct XpackInfo {
	std::string folder;
	std::string name;
	int mount_order = -1;
	int require_version = -1;
	bool has_data_win32 = false;
};

struct InstallStatus {
	bool located = false;
	bool layout_ok = false;
	std::string root_utf8;
	std::string source;
	bool has_layout_toc = false;
	bool has_cas_cat = false;
	bool has_patch = false;
	bool patch_authoritative = false;
	int patch_version = -1;
	std::vector<XpackInfo> xpacks;
	std::vector<std::string> problems;
};

class InstallReader {
public:
	static constexpr const char *kEnvVar = "OPENBF3_INSTALL";
	static constexpr const wchar_t *kDefaultRoot = L"C:\\Program Files\\EA Games\\Battlefield 3";

	bool locate(const InstallLocateOptions &p_options);

	bool mount(const std::filesystem::path &p_root, std::string p_source);

	bool is_mounted() const { return status_.layout_ok; }
	const InstallStatus &status() const { return status_; }
	const std::filesystem::path &root() const { return root_; }
	std::string describe() const;

	bool exists(std::string_view p_relative) const;
	ReadOnlyFile open(std::string_view p_relative) const;

	std::vector<std::string> list_subdirectories(std::string_view p_relative) const;

	PathGuard write_guard() const;
	InstallSnapshot snapshot() const;

private:
	bool resolve(std::string_view p_relative, std::filesystem::path &r_path) const;
	void read_manifest(std::string_view p_relative, std::vector<std::pair<std::string, std::string>> &r_pairs) const;

	std::filesystem::path root_;
	InstallStatus status_;
};

}
