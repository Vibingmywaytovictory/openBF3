#include "bf3/core/install_reader.h"

#include "win32_text.h"

#include <windows.h>

#include <algorithm>
#include <cstdlib>

namespace bf3::core {

namespace {

bool is_dir(const std::filesystem::path &p_path) {
	const DWORD a = GetFileAttributesW(p_path.c_str());
	return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

std::string env_value(const char *p_name) {
	const std::wstring wname = detail::widen(p_name);
	DWORD need = GetEnvironmentVariableW(wname.c_str(), nullptr, 0);
	if (need == 0) {
		return {};
	}
	std::wstring v(need, L'\0');
	DWORD got = GetEnvironmentVariableW(wname.c_str(), v.data(), need);
	v.resize(got < need ? got : 0);
	return detail::narrow(std::wstring_view(v));
}

int to_int(const std::string &p_s) {
	char *end = nullptr;
	const long v = std::strtol(p_s.c_str(), &end, 10);
	return (end != p_s.c_str()) ? static_cast<int>(v) : -1;
}

} // namespace

bool InstallReader::locate(const InstallLocateOptions &p_options) {
	struct Candidate {
		std::string path;
		const char *source;
	};
	std::vector<Candidate> candidates;
	if (!p_options.cmdline_path.empty()) {
		candidates.push_back({ p_options.cmdline_path, "cmdline" });
	}
	if (p_options.use_environment) {
		std::string env = env_value(kEnvVar);
		if (!env.empty()) {
			candidates.push_back({ env, "env" });
		}
	}
	if (!p_options.setting_path.empty()) {
		candidates.push_back({ p_options.setting_path, "setting" });
	}
	if (p_options.use_default) {
		candidates.push_back({ detail::narrow(std::wstring_view(kDefaultRoot)), "default" });
	}
	if (candidates.empty()) {
		status_ = {};
		status_.problems.push_back("no install location given and the default is disabled");
		return false;
	}
	// The first candidate wins: an explicit override that is wrong must be reported, not
	// silently replaced by the default install.
	return mount(std::filesystem::path(detail::widen(candidates.front().path)), candidates.front().source);
}

bool InstallReader::mount(const std::filesystem::path &p_root, std::string p_source) {
	status_ = {};
	root_.clear();
	status_.source = std::move(p_source);
	std::optional<std::filesystem::path> canonical = canonicalize_path(p_root);
	if (!canonical || !is_dir(*canonical)) {
		status_.root_utf8 = detail::path_utf8(p_root);
		status_.problems.push_back("install folder not found: " + status_.root_utf8);
		return false;
	}
	root_ = *canonical;
	status_.root_utf8 = detail::path_utf8(root_);
	status_.located = true;

	status_.has_layout_toc = exists("Data/layout.toc");
	status_.has_cas_cat = exists("Data/cas.cat");
	if (!status_.has_layout_toc) {
		status_.problems.push_back("missing Data/layout.toc");
	}
	if (!status_.has_cas_cat) {
		status_.problems.push_back("missing Data/cas.cat");
	}

	// Update\Patch: "Name patch / Authoritative / Version 7" on this install.
	if (exists("Update/Patch/package.mft")) {
		status_.has_patch = true;
		std::vector<std::pair<std::string, std::string>> kv;
		read_manifest("Update/Patch/package.mft", kv);
		for (const auto &[k, v] : kv) {
			if (k == "Version") {
				status_.patch_version = to_int(v);
			} else if (k == "Authoritative") {
				status_.patch_authoritative = true;
			}
		}
	} else {
		status_.problems.push_back("no Update/Patch (unpatched install?)");
	}

	// Update\Xpack*: list what exists; the engine must work with any subset (plan §2.1).
	WIN32_FIND_DATAW fd{};
	HANDLE h = FindFirstFileExW((root_ / L"Update" / L"Xpack*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
	if (h != INVALID_HANDLE_VALUE) {
		do {
			if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
				continue;
			}
			XpackInfo x;
			x.folder = detail::narrow(std::wstring_view(fd.cFileName));
			x.has_data_win32 = is_dir(root_ / L"Update" / fd.cFileName / L"Data" / L"Win32");
			std::vector<std::pair<std::string, std::string>> kv;
			read_manifest("Update/" + x.folder + "/package.mft", kv);
			for (const auto &[k, v] : kv) {
				if (k == "Name") {
					x.name = v;
				} else if (k == "MountOrder") {
					x.mount_order = to_int(v);
				} else if (k == "RequireVersion") {
					x.require_version = to_int(v);
				}
			}
			if (x.require_version > status_.patch_version) {
				status_.problems.push_back(x.folder + " requires patch version " + std::to_string(x.require_version));
			}
			status_.xpacks.push_back(std::move(x));
		} while (FindNextFileW(h, &fd));
		FindClose(h);
	}
	std::sort(status_.xpacks.begin(), status_.xpacks.end(), [](const XpackInfo &a, const XpackInfo &b) { return a.folder < b.folder; });

	status_.layout_ok = status_.has_layout_toc && status_.has_cas_cat;
	return status_.layout_ok;
}

std::string InstallReader::describe() const {
	if (!status_.located) {
		return "install not found (" + status_.source + "): " + status_.root_utf8;
	}
	std::string s = status_.root_utf8 + " [" + status_.source + "] layout " + (status_.layout_ok ? "OK" : "INCOMPLETE");
	if (status_.has_patch) {
		s += ", patch v" + std::to_string(status_.patch_version);
	}
	s += ", xpacks " + std::to_string(status_.xpacks.size());
	if (!status_.xpacks.empty()) {
		s += " (";
		for (size_t i = 0; i < status_.xpacks.size(); ++i) {
			s += (i ? " " : "") + status_.xpacks[i].folder.substr(std::min<size_t>(5, status_.xpacks[i].folder.size())) + ":req" +
					std::to_string(status_.xpacks[i].require_version);
		}
		s += ")";
	}
	return s;
}

bool InstallReader::resolve(std::string_view p_relative, std::filesystem::path &r_path) const {
	if (root_.empty() || p_relative.empty()) {
		return false;
	}
	std::filesystem::path rel(detail::widen(p_relative));
	if (rel.has_root_name() || rel.has_root_directory()) {
		return false;
	}
	for (const std::filesystem::path &c : rel) {
		const std::wstring &n = c.native();
		if (n == L".." || n.find(L':') != std::wstring::npos) {
			return false;
		}
	}
	r_path = (root_ / rel).make_preferred();
	return true;
}

bool InstallReader::exists(std::string_view p_relative) const {
	std::filesystem::path p;
	return resolve(p_relative, p) && GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

ReadOnlyFile InstallReader::open(std::string_view p_relative) const {
	std::filesystem::path p;
	if (!resolve(p_relative, p)) {
		return {};
	}
	return ReadOnlyFile::open_absolute(p);
}

std::vector<std::string> InstallReader::list_subdirectories(std::string_view p_relative) const {
	std::vector<std::string> out;
	std::filesystem::path dir;
	if (!resolve(p_relative, dir)) {
		return out;
	}
	WIN32_FIND_DATAW fd{};
	HANDLE h = FindFirstFileExW((dir / L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
	if (h == INVALID_HANDLE_VALUE) {
		return out;
	}
	do {
		const std::wstring_view name(fd.cFileName);
		if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && name != L"." && name != L"..") {
			out.push_back(detail::narrow(name));
		}
	} while (FindNextFileW(h, &fd));
	FindClose(h);
	std::sort(out.begin(), out.end());
	return out;
}

void InstallReader::read_manifest(std::string_view p_relative, std::vector<std::pair<std::string, std::string>> &r_pairs) const {
	ReadOnlyFile f = open(p_relative);
	if (!f.is_open() || f.size() > 64 * 1024) {
		return;
	}
	std::vector<uint8_t> bytes;
	if (!f.read_all(bytes)) {
		return;
	}
	std::string text(bytes.begin(), bytes.end());
	size_t pos = 0;
	while (pos < text.size()) {
		size_t eol = text.find('\n', pos);
		if (eol == std::string::npos) {
			eol = text.size();
		}
		std::string line = text.substr(pos, eol - pos);
		pos = eol + 1;
		while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
			line.pop_back();
		}
		if (line.empty()) {
			continue;
		}
		const size_t sp = line.find_first_of(" \t");
		if (sp == std::string::npos) {
			r_pairs.emplace_back(line, std::string());
		} else {
			r_pairs.emplace_back(line.substr(0, sp), line.substr(line.find_first_not_of(" \t", sp)));
		}
	}
}

PathGuard InstallReader::write_guard() const {
	PathGuard guard;
	if (!root_.empty()) {
		guard.add_protected_root(root_);
	}
	// The default location too, so a failed or overridden mount never leaves the retail install unguarded.
	guard.add_protected_root(std::filesystem::path(kDefaultRoot));
	return guard;
}

InstallSnapshot InstallReader::snapshot() const {
	return root_.empty() ? InstallSnapshot{} : take_install_snapshot(root_);
}

} // namespace bf3::core
