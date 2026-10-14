#pragma once
// Resource loading from a mounted superbundle: the bridge between the container layer (LevelMount:
// bundles, CAS, chunk tocs) and the span-based decoders of bf3::core::res. This is the API the
// Godot bridge calls; it stays Godot-free.
//
//   Result<LoadedMeshSet> mesh = load_mesh_set(mount, "weapons/m9/m9_1p_mesh");
//   std::vector<DecodedSubset> subsets;
//   ResError e = mesh->decode(0, SubsetFilter::Drawn, subsets);
//
// Preconditions: the mount's bundles are indexed (LevelMount::load_all_bundles or load_bundles);
// res names are matched like LevelMount::read_res does (case-insensitive, '\' == '/').
//
// Ownership and lifetime: every Loaded* object owns all bytes its decoded data refers to (the res
// payload and the whole decompressed chunks). The decoders' outputs are values (DecodedSubset,
// Shape), except the views MipSlice::data and the Bytes returned by geometry() / bytes(), which
// point into the object's vectors: they stay valid while the object lives, also after it is moved
// (a moved std::vector keeps its buffer), and must not outlive it. Views are never stored inside
// the objects, so copying one is safe too.
//
// Errors: Result / Error (no exceptions). Container failures (no such res, unreadable chunk) keep
// LevelMount's message; decode failures read "res <name>: <ResError::message()>". The loaders are
// strict: a res whose referenced chunk cannot be read fails as a whole. read_raw_res /
// read_raw_chunk give the bytes alone for callers that want per-chunk handling (tools/resdump).
//
// Threads: LevelMount reads are thread-safe, so different res may be loaded in parallel.

#include "bf3/core/res/dx_texture.h"
#include "bf3/core/res/havok.h"
#include "bf3/core/res/mesh_set.h"
#include "bf3/core/result.h"
#include "bf3/core/superbundle_mount.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core::res {

// One res entry, decompressed.
struct RawRes {
	std::string name; // the name it was read by
	uint32_t res_type = 0; // kResType*
	ResMeta res_meta{};
	std::vector<uint8_t> payload;

	Bytes bytes() const { return Bytes(payload.data(), payload.size()); }
};

// One chunk, decompressed and whole (logicalOffset 0) even when the level bundle holds only a
// ranged tail of it: LevelMount::read_chunk completes it from the CAS or the chunk tocs.
struct RawChunk {
	ChunkId id;
	std::vector<uint8_t> bytes;
	// logicalOffset of the level bundle's ranged entry (where the stored range starts in the full
	// chunk; for a texture, sum of the mips before the first resident one), 0 when not ranged.
	uint32_t view_offset = 0;

	Bytes whole() const { return Bytes(bytes.data(), bytes.size()); }
};

// The res a mount's loaded bundles hold of the decodable types (MeshSet, DxTexture,
// HavokPhysicsData, HavokDestructionPhysicsData): one name per lowercase spelling (the first
// indexed spelling is kept), sorted by the lowercase name (byte order).
std::vector<std::string> decodable_res_names(const LevelMount &p_mount);

Result<RawRes> read_raw_res(const LevelMount &p_mount, std::string_view p_name);
Result<RawChunk> read_raw_chunk(const LevelMount &p_mount, const ChunkId &p_id);

struct LoadedMeshSet {
	RawRes res;
	MeshSet mesh;
	std::vector<RawChunk> chunks; // distinct geometry chunks in first-use order
	std::vector<int32_t> lod_chunk; // per LOD: index into chunks, -1 for inline geometry

	// The bytes MeshLod::decode wants for LOD p_lod: its chunk, or the inline span of the payload.
	ResError geometry(size_t p_lod, Bytes &r_geometry) const;
	// Decodes every subset of LOD p_lod (p_filter Drawn leaves out ZOnly subsets).
	ResError decode(size_t p_lod, SubsetFilter p_filter, std::vector<DecodedSubset> &r_subsets) const;
};

struct LoadedTexture {
	RawRes res;
	DxTexture texture;
	RawChunk chunk; // empty (zero id, no bytes) when the texture names no chunk

	// Every image of the whole chunk, face-major, largest mip first; views into chunk.bytes.
	ResError mips(std::vector<MipSlice> &r_mips) const;
};

struct LoadedHavok {
	RawRes res; // HavokPhysicsData or HavokDestructionPhysicsData
	HavokPhysicsData data; // owns a copy of the packfile; no views into res.payload
	std::vector<Shape> shapes; // one root per rigid body (HavokPhysicsData::shapes)
};

Result<LoadedMeshSet> load_mesh_set(const LevelMount &p_mount, std::string_view p_name);
Result<LoadedTexture> load_texture(const LevelMount &p_mount, std::string_view p_name);
// Accepts both Havok res types.
Result<LoadedHavok> load_havok(const LevelMount &p_mount, std::string_view p_name);

} // namespace bf3::core::res
