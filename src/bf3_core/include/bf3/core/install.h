#pragma once

#include "bf3/core/ids.h"
#include "bf3/core/install_file.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/result.h"
#include "bf3/core/sb_formats.h"

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace bf3::core {

class LevelMount;

struct Package {
	std::string name;
	std::string data_dir;
	bool authoritative = false;
	std::optional<int64_t> version;
	std::optional<int64_t> require_version;
	int64_t mount_order = 0;
};

struct Level {
	std::string id;
	std::string superbundle;
	std::optional<uint8_t> expansion;
	std::optional<std::string> base_toc;
	std::optional<std::string> patch_toc;
};

struct SuperbundleSite {
	std::string name;
	std::optional<std::string> base_toc;
	std::optional<std::string> patch_toc;
	bool delta = false;
};

struct CasLocation {
	uint8_t catalog = 0;
	uint32_t archive = 0;
	uint32_t offset = 0;
	uint32_t size = 0;
};

class CasStore {
public:
	size_t size() const { return entries_.size(); }
	bool empty() const { return entries_.empty(); }
	bool contains(const Sha1 &p_sha1) const { return entries_.find(p_sha1) != entries_.end(); }
	std::optional<CasLocation> location(const Sha1 &p_sha1) const;
	const std::string *catalog_dir(uint8_t p_catalog) const;
	size_t catalog_count() const { return catalogs_.size(); }

	Result<std::vector<uint8_t>> read(const Sha1 &p_sha1) const;
	Result<std::vector<uint8_t>> read_prefix(const Sha1 &p_sha1, uint64_t p_len) const;

	Status check_entry_header(const Sha1 &p_sha1) const;

private:
	friend class Install;
	Status open(const InstallReader *p_reader, const std::vector<std::string> &p_dirs);
	Result<std::shared_ptr<InstallFile>> archive(const CasLocation &p_location) const;

	const InstallReader *reader_ = nullptr;
	std::unordered_map<Sha1, CasLocation, Sha1Hash> entries_;
	std::vector<std::string> catalogs_;
	mutable std::mutex archives_mutex_;
	mutable std::map<uint64_t, std::shared_ptr<InstallFile>> archives_;
};

struct TocChunkSite {
	bool in_cas = false;
	Sha1 sha1;
	std::shared_ptr<InstallFile> file;
	uint64_t offset = 0;
	uint64_t size = 0;
};
using TocChunkMap = std::unordered_map<Guid, TocChunkSite, GuidHash>;

class Install : public std::enable_shared_from_this<Install> {
public:

	static Result<std::shared_ptr<Install>> open(const InstallReader &p_reader);

	Install(const Install &) = delete;
	Install &operator=(const Install &) = delete;

	const InstallReader &reader() const { return reader_; }
	const std::string &data_dir() const { return data_dir_; }

	const std::vector<Package> &packages() const { return packages_; }
	const std::string &layout_path() const { return layout_path_; }
	const Layout &layout() const { return layout_; }
	const CasStore &cas() const { return cas_; }
	const std::vector<SuperbundleSite> &sites() const { return sites_; }
	std::vector<std::string> superbundle_names() const;
	const SuperbundleSite *site(std::string_view p_name) const;

	std::vector<Level> levels() const;
	std::optional<Level> level(std::string_view p_level_id) const;
	Result<LevelMount> mount_level(std::string_view p_level_id) const;

	Result<LevelMount> mount_superbundle(std::string_view p_name) const;

	Result<std::shared_ptr<InstallFile>> file(std::string_view p_relative) const;
	Result<std::vector<uint8_t>> read_whole(std::string_view p_relative) const;
	Result<Toc> read_toc(std::string_view p_relative) const;

	static std::string superbundle_file(std::string_view p_toc_relative);

	Result<std::shared_ptr<const TocChunkMap>> chunk_toc(const SuperbundleSite &p_site) const;

private:
	Install() = default;

	InstallReader reader_;
	std::string data_dir_;
	std::vector<Package> packages_;
	std::string layout_path_;
	Layout layout_;
	std::vector<SuperbundleSite> sites_;
	CasStore cas_;

	mutable std::mutex files_mutex_;
	mutable std::unordered_map<std::string, std::shared_ptr<InstallFile>> files_;
	mutable std::mutex chunk_tocs_mutex_;
	mutable std::unordered_map<std::string, std::shared_ptr<const TocChunkMap>> chunk_tocs_;
};

std::optional<std::string> multiplayer_level_id(std::string_view p_superbundle);
std::optional<uint8_t> expansion_of(std::string_view p_level_id);

}
