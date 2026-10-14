// AntExportDirSource and read_whole_file; see ant_export_source.h. Clean-room implementation from
// research/probes/ant-animation-probe.md (§2). Reads only the export directory it is given (never the install).
#include "ant_export_source.h"

#include "bf3/core/ids.h"

#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace bf3::tools::antdump {

using namespace bf3::core::ant;
using bf3::core::to_lower_ascii;

namespace {

std::vector<std::string_view> split(std::string_view p_line, char p_sep) {
	std::vector<std::string_view> out;
	size_t at = 0;
	while (true) {
		const size_t next = p_line.find(p_sep, at);
		out.push_back(p_line.substr(at, next == std::string_view::npos ? std::string_view::npos : next - at));
		if (next == std::string_view::npos) {
			break;
		}
		at = next + 1;
	}
	return out;
}

// A file name from res.tsv / packages.tsv must stay inside the export directory: relative, no root,
// no ".." component (the tsv files are data, so they are checked like any other input).
bool is_contained_relative(std::string_view p_name) {
	if (p_name.empty() || p_name.find(':') != std::string_view::npos) {
		return false;
	}
	const std::filesystem::path path{ std::u8string(p_name.begin(), p_name.end()) };
	if (path.has_root_name() || path.has_root_directory()) {
		return false;
	}
	for (const std::filesystem::path &part : path) {
		if (part == "..") {
			return false;
		}
	}
	return true;
}

bool read_lines(const std::filesystem::path &p_path, std::vector<std::string> &r_lines, std::string &r_error) {
	std::vector<uint8_t> bytes;
	if (!read_whole_file(p_path, bytes, r_error)) {
		return false;
	}
	r_lines.clear();
	std::string cur;
	for (uint8_t b : bytes) {
		if (b == '\n') {
			r_lines.push_back(cur);
			cur.clear();
		} else if (b != '\r') {
			cur.push_back(char(b));
		}
	}
	if (!cur.empty()) {
		r_lines.push_back(cur);
	}
	return true;
}

} // namespace

bool read_whole_file(const std::filesystem::path &p_path, std::vector<uint8_t> &r_bytes, std::string &r_error) {
	// Long names (the export keeps res names as file names) need the extended-length form past MAX_PATH.
	std::error_code ec;
	std::filesystem::path full = std::filesystem::absolute(p_path, ec);
	std::wstring name = (ec ? p_path : full).lexically_normal().make_preferred().wstring();
	constexpr const wchar_t *kLongPrefix = L"\\\\?\\";
	if (name.size() >= 260 && name.rfind(kLongPrefix, 0) != 0) {
		name = kLongPrefix + name;
	}
	FILE *f = nullptr;
	if (_wfopen_s(&f, name.c_str(), L"rb") != 0 || f == nullptr) {
		r_error = "cannot open " + p_path.string();
		return false;
	}
	_fseeki64(f, 0, SEEK_END);
	const int64_t size = _ftelli64(f);
	_fseeki64(f, 0, SEEK_SET);
	if (size < 0 || size > (int64_t(1) << 32)) {
		fclose(f);
		r_error = "bad size of " + p_path.string();
		return false;
	}
	r_bytes.resize(size_t(size));
	const size_t got = r_bytes.empty() ? 0 : fread(r_bytes.data(), 1, r_bytes.size(), f);
	fclose(f);
	if (got != r_bytes.size()) {
		r_error = "short read of " + p_path.string();
		return false;
	}
	return true;
}

bool AntExportDirSource::open(const std::filesystem::path &p_dir, std::string &r_error) {
	dir_ = p_dir;
	packages_.clear();
	banks_.clear();
	res_.clear();
	chunk_files_.clear();
	std::vector<std::string> lines;
	if (!read_lines(p_dir / "res.tsv", lines, r_error)) {
		return false;
	}
	for (size_t i = 1; i < lines.size(); ++i) {
		const auto cols = split(lines[i], '\t');
		if (cols.size() < 2) {
			r_error = "res.tsv: bad row " + std::to_string(i);
			return false;
		}
		if (!is_contained_relative(cols[1])) {
			r_error = "res.tsv: file name escapes the export directory in row " + std::to_string(i);
			return false;
		}
		ResEntry e{ to_lower_ascii(cols[0]), std::string(cols[1]), {}, {} };
		if (cols.size() >= 6) {
			e.superbundle = std::string(cols[4]);
			e.bundle = std::string(cols[5]);
		}
		res_.push_back(std::move(e));
	}
	if (!read_lines(p_dir / "packages.tsv", lines, r_error)) {
		return false;
	}
	for (size_t i = 1; i < lines.size(); ++i) {
		const auto cols = split(lines[i], '\t');
		if (cols.size() < 6) {
			r_error = "packages.tsv: bad row " + std::to_string(i);
			return false;
		}
		if (cols[1] != "AntPackageAsset") {
			continue;
		}
		AntPackageInfo info;
		info.partition = to_lower_ascii(cols[0]);
		info.superbundle = std::string(cols[2]);
		info.bundle = std::string(cols[3]);
		for (std::string_view kv : split(cols[5], ';')) {
			const size_t eq = kv.find('=');
			if (eq == std::string_view::npos) {
				continue;
			}
			const std::string_view k = kv.substr(0, eq), v = kv.substr(eq + 1);
			if (k == "Name") {
				info.name = std::string(v);
			} else if (k == "PackagingType") {
				info.packaging = packaging_from_enum_name(v);
			} else if (k == "StreamingGuid") {
				info.streaming_guid = std::string(v);
			} else if (k == "ChunkSize") {
				info.chunk_size = std::strtoull(std::string(v).c_str(), nullptr, 10);
			}
		}
		info.short_name = package_short_name(info.name.empty() ? info.partition : info.name);
		if (!cols[4].empty() && !is_contained_relative(cols[4])) {
			r_error = "packages.tsv: file name escapes the export directory in row " + std::to_string(i);
			return false;
		}
		// Res-backed packages report the res's site (as AntInstallSource does); chunk packages their partition's.
		if (info.packaging != AntPackagingType::Chunk) {
			const std::string key = to_lower_ascii(info.name);
			for (const ResEntry &e : res_) {
				if (e.name == key) {
					info.superbundle = e.superbundle;
					info.bundle = e.bundle;
					break;
				}
			}
		}
		packages_.push_back(info);
		chunk_files_.push_back(std::string(cols[4]));
	}
	for (const ResEntry &e : res_) {
		AntPackageInfo info;
		info.partition = e.name;
		info.name = e.name;
		info.short_name = package_short_name(e.name);
		info.is_asset_package = false;
		for (const AntPackageInfo &p : packages_) {
			if (to_lower_ascii(p.name) == e.name) {
				info = p;
				break;
			}
		}
		info.superbundle = e.superbundle;
		info.bundle = e.bundle;
		banks_.push_back(info);
	}
	return true;
}

bool AntExportDirSource::read(const AntPackageInfo &p_info, std::vector<uint8_t> &r_bytes, std::string &r_error) const {
	if (p_info.packaging == AntPackagingType::Chunk && p_info.is_asset_package) {
		for (size_t i = 0; i < packages_.size(); ++i) {
			if (packages_[i].partition == p_info.partition) {
				if (chunk_files_[i].empty()) {
					r_error = p_info.partition + ": chunk " + p_info.streaming_guid + " was not exported";
					return false;
				}
				return read_whole_file(dir_ / chunk_files_[i], r_bytes, r_error);
			}
		}
		r_error = p_info.partition + ": unknown package";
		return false;
	}
	// Static / Bundle / animation sets: the res named lowercase(Name).
	const std::string key = to_lower_ascii(p_info.name);
	for (const ResEntry &e : res_) {
		if (e.name == key) {
			return read_whole_file(dir_ / e.file, r_bytes, r_error);
		}
	}
	r_error = p_info.name + ": res not exported";
	return false;
}

} // namespace bf3::tools::antdump
