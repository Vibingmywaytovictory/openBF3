#pragma once

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

struct RawRes {
	std::string name;
	uint32_t res_type = 0;
	ResMeta res_meta{};
	std::vector<uint8_t> payload;

	Bytes bytes() const { return Bytes(payload.data(), payload.size()); }
};

struct RawChunk {
	ChunkId id;
	std::vector<uint8_t> bytes;

	uint32_t view_offset = 0;

	Bytes whole() const { return Bytes(bytes.data(), bytes.size()); }
};

std::vector<std::string> decodable_res_names(const LevelMount &p_mount);

Result<RawRes> read_raw_res(const LevelMount &p_mount, std::string_view p_name);
Result<RawChunk> read_raw_chunk(const LevelMount &p_mount, const ChunkId &p_id);

struct LoadedMeshSet {
	RawRes res;
	MeshSet mesh;
	std::vector<RawChunk> chunks;
	std::vector<int32_t> lod_chunk;

	ResError geometry(size_t p_lod, Bytes &r_geometry) const;

	ResError decode(size_t p_lod, SubsetFilter p_filter, std::vector<DecodedSubset> &r_subsets) const;
};

struct LoadedTexture {
	RawRes res;
	DxTexture texture;
	RawChunk chunk;

	ResError mips(std::vector<MipSlice> &r_mips) const;
};

struct LoadedHavok {
	RawRes res;
	HavokPhysicsData data;
	std::vector<Shape> shapes;
};

Result<LoadedMeshSet> load_mesh_set(const LevelMount &p_mount, std::string_view p_name);
Result<LoadedTexture> load_texture(const LevelMount &p_mount, std::string_view p_name);

Result<LoadedHavok> load_havok(const LevelMount &p_mount, std::string_view p_name);

}
