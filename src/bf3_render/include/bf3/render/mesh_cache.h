#pragma once

#include "bf3/core/cache/cached_convert.h"
#include "bf3/core/res/mesh_set.h"
#include "bf3/core/res/res_loader.h"
#include "bf3/core/result.h"
#include "bf3/core/superbundle_mount.h"
#include "bf3/render/mesh_prep.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::render {

inline constexpr uint32_t kMeshBlobFormatVersion = 1;

inline constexpr uint32_t kMeshConverterVersion = 1;
inline constexpr const char *kMeshCacheKind = "mesh";

struct PreparedMeshSet {
	std::string name;
	std::vector<uint32_t> lods;
	std::vector<PreparedMesh> meshes;

	uint64_t vertices() const;
	uint64_t triangles() const;
};

std::vector<uint8_t> encode_mesh_blob(const PreparedMeshSet &p_mesh);
core::Result<PreparedMeshSet> decode_mesh_blob(std::span<const uint8_t> p_blob);

struct MeshRequest {
	std::vector<uint32_t> lods{ 0 };
	core::res::SubsetFilter filter = core::res::SubsetFilter::Drawn;

	const std::vector<std::vector<int32_t>> *lane_binds = nullptr;
};

struct MeshHead {
	std::string name;
	core::res::RawRes res;
	core::res::MeshSet mesh;
	core::Sha1 res_sha1;
	bool digests = false;
};
core::Result<MeshHead> read_mesh_head(const core::LevelMount &p_mount, std::string_view p_name);

core::CacheKeyInputs mesh_key_inputs(const std::vector<core::Sha1Digest> &p_sources, const MeshRequest &p_request,
		uint32_t p_converter_version = kMeshConverterVersion);

std::optional<core::CacheKey> mesh_cache_key(const core::LevelMount &p_mount, const MeshHead &p_head, const MeshRequest &p_request);

core::Result<core::res::LoadedMeshSet> load_mesh_lods(const core::LevelMount &p_mount, const MeshHead &p_head, const std::vector<uint32_t> &p_lods);

core::Result<PreparedMeshSet> prepare_mesh_set(const core::res::LoadedMeshSet &p_mesh, const MeshRequest &p_request);

struct CachedMesh {
	PreparedMeshSet mesh;
	core::CachedItem item;
	bool key_known = false;
};

core::Result<CachedMesh> load_mesh_cached(core::Cache *p_cache, const core::LevelMount &p_mount, const MeshHead &p_head, const MeshRequest &p_request);
core::Result<CachedMesh> load_mesh_cached(core::Cache *p_cache, const core::LevelMount &p_mount, std::string_view p_name, const MeshRequest &p_request);

bool same_prepared(const PreparedMesh &p_a, const PreparedMesh &p_b);

}
