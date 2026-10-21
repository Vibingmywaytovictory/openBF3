#include "bf3/core/res/res_loader.h"

#include "bf3/core/res_types.h"
#include "bf3/core/util/text.h"

#include <algorithm>
#include <map>
#include <utility>

namespace bf3::core::res {

namespace {

bool decodable(uint32_t p_type) {
	return p_type == kResTypeMeshSet || p_type == kResTypeDxTexture || p_type == kResTypeHavokPhysicsData ||
			p_type == kResTypeHavokDestructionPhysicsData;
}

Error res_error(const std::string &p_name, const ResError &p_error) {
	return make_error("res " + p_name + ": " + p_error.message());
}

Result<RawRes> read_typed(const LevelMount &p_mount, std::string_view p_name, uint32_t p_type, uint32_t p_alt_type, const char *p_what) {
	Result<RawRes> raw = read_raw_res(p_mount, p_name);
	if (!raw) {
		return raw;
	}
	if (raw->res_type != p_type && raw->res_type != p_alt_type) {
		return make_error("res " + raw->name + " is a " + res_type_name(raw->res_type) + ", not a " + p_what);
	}
	return raw;
}

}

std::vector<std::string> decodable_res_names(const LevelMount &p_mount) {
	std::map<std::string, std::string> names;
	for (std::string &name : p_mount.res_names()) {
		const std::optional<ResInfo> info = p_mount.res_info(name);
		if (info && decodable(info->res_type)) {
			names.try_emplace(to_lower_ascii(name), std::move(name));
		}
	}
	std::vector<std::string> out;
	out.reserve(names.size());
	for (auto &[key, name] : names) {
		out.push_back(std::move(name));
	}
	return out;
}

Result<RawRes> read_raw_res(const LevelMount &p_mount, std::string_view p_name) {
	Result<std::pair<ResInfo, std::vector<uint8_t>>> read = p_mount.read_res(p_name);
	if (!read) {
		return read.error();
	}
	RawRes raw;
	raw.name = std::string(p_name);
	raw.res_type = read->first.res_type;
	raw.res_meta = read->first.res_meta;
	raw.payload = std::move(read->second);
	return raw;
}

Result<RawChunk> read_raw_chunk(const LevelMount &p_mount, const ChunkId &p_id) {
	const Guid guid = Guid::from_bytes(p_id.bytes.data());
	Result<std::vector<uint8_t>> bytes = p_mount.read_chunk(guid);
	if (!bytes) {
		return bytes.error();
	}
	RawChunk chunk;
	chunk.id = p_id;
	chunk.bytes = std::move(*bytes);
	const std::optional<ChunkInfo> info = p_mount.chunk_info(guid);
	if (info && info->range) {
		chunk.view_offset = info->range->logical_offset;
	}
	return chunk;
}

ResError LoadedMeshSet::geometry(size_t p_lod, Bytes &r_geometry) const {
	r_geometry = Bytes();
	if (p_lod >= mesh.lods.size() || p_lod >= lod_chunk.size()) {
		return ResError::malformed("lod index", p_lod);
	}
	const int32_t chunk = lod_chunk[p_lod];
	if (chunk < 0) {
		return mesh.lods[p_lod].inline_geometry(res.bytes(), r_geometry);
	}
	r_geometry = chunks[size_t(chunk)].whole();
	return ResError();
}

ResError LoadedMeshSet::decode(size_t p_lod, SubsetFilter p_filter, std::vector<DecodedSubset> &r_subsets) const {
	r_subsets.clear();
	Bytes g;
	const ResError e = geometry(p_lod, g);
	if (!e.ok()) {
		return e;
	}
	return mesh.lods[p_lod].decode(g, p_filter, r_subsets);
}

Result<LoadedMeshSet> load_mesh_set(const LevelMount &p_mount, std::string_view p_name) {
	Result<RawRes> raw = read_typed(p_mount, p_name, kResTypeMeshSet, kResTypeMeshSet, "MeshSet");
	if (!raw) {
		return raw.error();
	}
	LoadedMeshSet out;
	out.res = std::move(*raw);
	const ResError e = MeshSet::parse(out.res.bytes(), out.res.res_meta, out.mesh);
	if (!e.ok()) {
		return res_error(out.res.name, e);
	}
	for (const MeshLod &lod : out.mesh.lods) {
		if (lod.geometry.is_inline) {
			out.lod_chunk.push_back(-1);
			continue;
		}
		const auto same = std::find_if(out.chunks.begin(), out.chunks.end(), [&](const RawChunk &c) { return c.id == lod.geometry.chunk; });
		if (same != out.chunks.end()) {
			out.lod_chunk.push_back(int32_t(same - out.chunks.begin()));
			continue;
		}
		Result<RawChunk> chunk = read_raw_chunk(p_mount, lod.geometry.chunk);
		if (!chunk) {
			return error_at("res " + out.res.name, chunk.error());
		}
		out.lod_chunk.push_back(int32_t(out.chunks.size()));
		out.chunks.push_back(std::move(*chunk));
	}
	return out;
}

ResError LoadedTexture::mips(std::vector<MipSlice> &r_mips) const {
	return texture.split_mips(chunk.whole(), 0, r_mips);
}

Result<LoadedTexture> load_texture(const LevelMount &p_mount, std::string_view p_name) {
	Result<RawRes> raw = read_typed(p_mount, p_name, kResTypeDxTexture, kResTypeDxTexture, "DxTexture");
	if (!raw) {
		return raw.error();
	}
	LoadedTexture out;
	out.res = std::move(*raw);
	const ResError e = DxTexture::parse(out.res.bytes(), out.texture);
	if (!e.ok()) {
		return res_error(out.res.name, e);
	}
	if (!out.texture.chunk_id.is_zero()) {
		Result<RawChunk> chunk = read_raw_chunk(p_mount, out.texture.chunk_id);
		if (!chunk) {
			return error_at("res " + out.res.name, chunk.error());
		}
		out.chunk = std::move(*chunk);
	}
	return out;
}

Result<LoadedHavok> load_havok(const LevelMount &p_mount, std::string_view p_name) {
	Result<RawRes> raw = read_typed(p_mount, p_name, kResTypeHavokPhysicsData, kResTypeHavokDestructionPhysicsData, "Havok physics res");
	if (!raw) {
		return raw.error();
	}
	LoadedHavok out;
	out.res = std::move(*raw);
	ResError e = HavokPhysicsData::parse(out.res.bytes(), out.res.res_meta, out.data);
	if (e.ok()) {
		e = out.data.shapes(out.shapes);
	}
	if (!e.ok()) {
		return res_error(out.res.name, e);
	}
	return out;
}

}
