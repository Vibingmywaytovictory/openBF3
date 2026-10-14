#pragma once
// One mounted superbundle (a level or any other layout superbundle): its bundles after the
// base+patch merge, the toc-level chunks of the chunk superbundles, and an index of the loaded
// bundles' ebx/res/chunk entries. Ported from IW4L asset_transport::bf3 (mount.rs).
//
// Merge (superbundle_bundles):
//   no patch toc             -> base toc bundles in the base .sb
//   patch toc, layout !delta -> patch toc bundles only (full replacement)
//   patch toc, layout delta  -> per patch entry: base -> base .sb at the base entry; delta ->
//                               DeltaBundle over (patch .sb, base .sb); else patch .sb;
//                               then the base bundles the patch toc does not name
// Chunk lookup (read_chunk): whole copy in a loaded bundle (replaces an earlier tail-only one) ->
//   tail-only entry: whole payload by sha1 in the merged CAS, else the same GUID in the chunk
//   superbundle tocs (its size must equal rangeEnd) -> toc chunk only.
// Names: ebx/res by lowercase name with '\' -> '/'; bundles additionally without "win32/".

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
	std::string_view name; // Ebx, Res
	Guid id; // Chunk
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
	const std::string &bundle_name(size_t p_bundle) const; // toc id
	std::vector<std::string> bundle_names() const;
	const std::vector<std::string> &chunk_superbundles() const;
	size_t toc_chunk_count() const;
	bool has_chunk(const Guid &p_id) const;

	// Reads and parses one bundle's manifest (no index change). Thread-safe.
	Result<BundleManifest> read_manifest(size_t p_bundle) const;
	// Every bundle's own manifest, in bundle order (parallel).
	std::vector<std::pair<std::string, Result<BundleManifest>>> bundle_manifests() const;

	// Index bundles (parallel manifest reads; first loaded bundle wins on a name).
	Status load_all_bundles() const;
	Status load_bundles(const std::vector<std::string> &p_names) const;
	std::vector<BundleSummary> loaded_bundles() const;

	std::vector<std::string> ebx_names() const;
	std::vector<std::string> res_names() const;
	std::vector<Guid> chunk_ids() const;
	bool has_ebx(std::string_view p_name) const;
	std::optional<ResInfo> res_info(std::string_view p_name) const;
	// CAS bundles pair a chunk with its meta; for binary bundles use asset_chunk_meta(owner).
	std::optional<ChunkMeta> chunk_meta(const Guid &p_id) const;
	std::optional<ChunkMeta> asset_chunk_meta(std::string_view p_owner) const;
	std::optional<ChunkInfo> chunk_info(const Guid &p_id) const;

	// Decoded payloads.
	Result<std::vector<uint8_t>> read_ebx(std::string_view p_name) const;
	Result<std::pair<ResInfo, std::vector<uint8_t>>> read_res(std::string_view p_name) const;
	Result<std::vector<uint8_t>> read_chunk(const Guid &p_id) const;
	// Partition GUID -> ebx name over the loaded ebx (80-byte header reads, parallel).
	Result<std::unordered_map<Guid, std::string, GuidHash>> ebx_partition_index() const;
	// The partition GUID of one loaded ebx (reads only its 80-byte header when stored uncompressed).
	Result<Guid> ebx_partition_guid(std::string_view p_name) const;
	// Toc id of the bundle a loaded ebx is indexed from (the first loaded bundle that lists it).
	std::optional<std::string> ebx_bundle(std::string_view p_name) const;

	// Stored (not decoded) bytes and their SHA1 (the manifest sha1 is of these bytes).
	std::optional<Sha1> entry_sha1(const EntryRef &p_entry) const;
	Result<std::vector<uint8_t>> read_stored(const EntryRef &p_entry) const;

	struct Impl;

private:
	explicit LevelMount(std::unique_ptr<Impl> p_impl);
	std::unique_ptr<Impl> impl_;
};

// Index keys: lowercase, '\' -> '/'; bundle keys also drop a leading '/' and "win32/".
std::string asset_key(std::string_view p_name);
std::string bundle_key(std::string_view p_name);

} // namespace bf3::core
