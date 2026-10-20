#pragma once

#include "bf3/core/bundle_manifest.h"
#include "bf3/core/ids.h"
#include "bf3/core/install.h"
#include "bf3/core/result.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bf3::core {

struct ResInfo {
	uint32_t res_type = 0;
	std::array<uint8_t, 16> res_meta{};
};

struct ChunkInfo {
	Sha1 sha1;
	uint64_t stored_size = 0;
	std::optional<ChunkRange> range;
	bool tail_only = false;
	std::optional<ChunkMeta> meta;
};

struct BundleSummary {
	std::string name;
	size_t ebx = 0;
	size_t res = 0;
	size_t chunks = 0;
};

struct EntryRef {
	enum class Kind { Ebx, Res, Chunk };
	Kind kind = Kind::Ebx;
	std::string_view name;
	Guid id;
	static EntryRef ebx(std::string_view p_name) { return { Kind::Ebx, p_name, {} }; }
	static EntryRef res(std::string_view p_name) { return { Kind::Res, p_name, {} }; }
	static EntryRef chunk(const Guid &p_id) { return { Kind::Chunk, {}, p_id }; }
};

class LevelMount {
public:
	static Result<LevelMount> mount(std::shared_ptr<const Install> p_install, Level p_level);

	LevelMount(LevelMount &&) noexcept;
	LevelMount &operator=(LevelMount &&) noexcept;
	~LevelMount();

	const Level &level() const;
	const std::shared_ptr<const Install> &install() const;

	size_t bundle_count() const;
	const std::string &bundle_name(size_t p_bundle) const;
	std::vector<std::string> bundle_names() const;
	const std::vector<std::string> &chunk_superbundles() const;
	size_t toc_chunk_count() const;
	bool has_chunk(const Guid &p_id) const;

	Result<BundleManifest> read_manifest(size_t p_bundle) const;

	std::vector<std::pair<std::string, Result<BundleManifest>>> bundle_manifests() const;

	Status load_all_bundles() const;
	Status load_bundles(const std::vector<std::string> &p_names) const;
	std::vector<BundleSummary> loaded_bundles() const;

	std::vector<std::string> ebx_names() const;
	std::vector<std::string> res_names() const;
	std::vector<Guid> chunk_ids() const;
	bool has_ebx(std::string_view p_name) const;
	std::optional<ResInfo> res_info(std::string_view p_name) const;

	std::optional<ChunkMeta> chunk_meta(const Guid &p_id) const;
	std::optional<ChunkMeta> asset_chunk_meta(std::string_view p_owner) const;
	std::optional<ChunkInfo> chunk_info(const Guid &p_id) const;

	Result<std::vector<uint8_t>> read_ebx(std::string_view p_name) const;
	Result<std::pair<ResInfo, std::vector<uint8_t>>> read_res(std::string_view p_name) const;
	Result<std::vector<uint8_t>> read_chunk(const Guid &p_id) const;

	Result<std::unordered_map<Guid, std::string, GuidHash>> ebx_partition_index() const;

	Result<Guid> ebx_partition_guid(std::string_view p_name) const;

	std::optional<std::string> ebx_bundle(std::string_view p_name) const;

	std::optional<Sha1> entry_sha1(const EntryRef &p_entry) const;
	Result<std::vector<uint8_t>> read_stored(const EntryRef &p_entry) const;

	std::vector<Sha1> chunk_source_sha1s(const Guid &p_id) const;

	struct Impl;

private:
	explicit LevelMount(std::unique_ptr<Impl> p_impl);
	std::unique_ptr<Impl> impl_;
};

std::string asset_key(std::string_view p_name);
std::string bundle_key(std::string_view p_name);

}
