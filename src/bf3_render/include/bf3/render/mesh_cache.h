#pragma once
// Prepared render meshes through the derived-data cache (plan §2.4 step 3 "render meshes -> Godot surface
// blobs", §3.12). Godot-free; runs on loader threads. The Godot bridge (src/bf3_godot render_bridge) takes the
// PreparedMesh of a LOD exactly as prepare_mesh returns it, whether it came from the cache or was converted.
//
// Unit: one MeshSet res and a request (which LODs, which subsets, the skin lane binds), giving one
// PreparedMesh per requested LOD: the ArrayMesh-layout arrays of every surface (positions, normals, tangents,
// UVs, colours, bones/weights, composite parts, clockwise indices, AABB) with its material binding
// (MeshAsset.Materials index and the subset's material name). What a placement adds on top (the
// MeshVariationDatabase textures, hidden composite parts, a baked non-uniform transform) is applied when the
// level is instanced, so one blob serves every variant and every level that carries the same bytes.
//
// Key (cache_key.h): kind "mesh"; sources = the manifest SHA1 of the res, then of every chunk the requested
// LODs read (LevelMount::chunk_source_sha1s, distinct, in LOD order); the converter version; the engine id;
// the blob format; params "lods=<i,..>;filter=<all|drawn>;binds=<none|SHA-256 of the lane binds>". The key
// is computed from the res payload alone (MeshSet::parse names the chunks), so a warm load reads no chunk.
// Content, never names: two res with the same bytes share one entry (the result carries the requested name).
//
// Blob ("OBF3MESH", kMeshBlobFormatVersion), little endian, arrays 4-byte aligned:
//   magic, u32 version | str name | u8 skinned | u64 lod count
//   per LOD: u32 lod index | u64 surface count
//   per surface: u64 subset | u32 material_index | str material_name | u8 category (0xFF = none)
//                | u32 vertex count n | u32 index count m | u32 array mask | 6 x f32 AABB
//                | arrays present in the mask, in this order: positions 3n f32, normals 3n f32,
//                  tangents 4n f32, uv0 2n f32, uv1 2n f32, colors 4n u8, bones 4n i32, weights 4n f32,
//                  parts n u16; then indices m i32
// decode_mesh_blob checks every count against the bytes left (no allocation a short blob cannot back), that
// m is a multiple of 3, every index is < n and every bone index >= 0, and refuses trailing bytes; the vertex
// and triangle totals are recomputed.

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
// Bump when prepare_mesh's output changes for the same input (winding, tangent sign, weights, ...).
inline constexpr uint32_t kMeshConverterVersion = 1;
inline constexpr const char *kMeshCacheKind = "mesh";

// Several LODs of one MeshSet as prepare_mesh gives them.
struct PreparedMeshSet {
	std::string name;
	std::vector<uint32_t> lods; // LOD index of each entry of meshes
	std::vector<PreparedMesh> meshes;

	uint64_t vertices() const;
	uint64_t triangles() const;
};

std::vector<uint8_t> encode_mesh_blob(const PreparedMeshSet &p_mesh);
core::Result<PreparedMeshSet> decode_mesh_blob(std::span<const uint8_t> p_blob);

struct MeshRequest {
	std::vector<uint32_t> lods{ 0 };
	core::res::SubsetFilter filter = core::res::SubsetFilter::Drawn;
	// Skinned meshes: lane binds as from bf3::anim::SkinBinds::bind_lod (lane_binds[subset][lane]); they are
	// LOD-specific, so a skinned request names one LOD. Null = unskinned.
	const std::vector<std::vector<int32_t>> *lane_binds = nullptr;
};

// The res of a mesh, parsed, and its key sources: what a cache lookup needs (no chunk is read).
struct MeshHead {
	std::string name;
	core::res::RawRes res; // payload + resMeta (a few KB; the geometry is in chunks)
	core::res::MeshSet mesh;
	core::Sha1 res_sha1;
	bool digests = false; // the res has a manifest SHA1 (always, for a res a bundle lists)
};
core::Result<MeshHead> read_mesh_head(const core::LevelMount &p_mount, std::string_view p_name);

// The key inputs from the sources (res SHA1, then the chunks') and the request: a pure function, so the tests
// can show that a converter version or a source change changes the key.
core::CacheKeyInputs mesh_key_inputs(const std::vector<core::Sha1Digest> &p_sources, const MeshRequest &p_request,
		uint32_t p_converter_version = kMeshConverterVersion);
// The key of a request; nullopt when a source has no manifest SHA1 or a requested LOD does not exist.
std::optional<core::CacheKey> mesh_cache_key(const core::LevelMount &p_mount, const MeshHead &p_head, const MeshRequest &p_request);

// The res with the chunks of LODs 0..max(p_lods) only (load_mesh_set reads every LOD's chunk).
core::Result<core::res::LoadedMeshSet> load_mesh_lods(const core::LevelMount &p_mount, const MeshHead &p_head, const std::vector<uint32_t> &p_lods);
// Converts a request without the cache.
core::Result<PreparedMeshSet> prepare_mesh_set(const core::res::LoadedMeshSet &p_mesh, const MeshRequest &p_request);

struct CachedMesh {
	PreparedMeshSet mesh;
	core::CachedItem item;
	bool key_known = false; // false: no key (missing SHA1), converted uncached
};

// The request from p_cache (may be null) when the key matches, else loaded from the install, converted and put.
core::Result<CachedMesh> load_mesh_cached(core::Cache *p_cache, const core::LevelMount &p_mount, const MeshHead &p_head, const MeshRequest &p_request);
core::Result<CachedMesh> load_mesh_cached(core::Cache *p_cache, const core::LevelMount &p_mount, std::string_view p_name, const MeshRequest &p_request);

// Whether two prepared meshes hold identical arrays (bit for bit) and bindings.
bool same_prepared(const PreparedMesh &p_a, const PreparedMesh &p_b);

} // namespace bf3::render
