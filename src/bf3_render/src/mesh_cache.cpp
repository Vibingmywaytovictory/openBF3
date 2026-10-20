#include "bf3/render/mesh_cache.h"

#include "bf3/core/cache/blob_io.h"
#include "bf3/core/ids.h"

#include <algorithm>
#include <cstring>

namespace bf3::render {

namespace {

constexpr const char *kMeshMagic = "OBF3MESH";
constexpr size_t kMaxName = 4096;
constexpr uint8_t kNoCategory = 0xFF;
constexpr uint32_t kMaxLodIndex = 64;

enum ArrayBit : uint32_t {
	kNormals = 1u << 0,
	kTangents = 1u << 1,
	kUv0 = 1u << 2,
	kUv1 = 1u << 3,
	kColors = 1u << 4,
	kBones = 1u << 5,
	kWeights = 1u << 6,
	kParts = 1u << 7,
	kKnownBits = (1u << 8) - 1,
};

template <class T>
bool same_bytes(const std::vector<T> &p_a, const std::vector<T> &p_b) {
	return p_a.size() == p_b.size() && (p_a.empty() || std::memcmp(p_a.data(), p_b.data(), p_a.size() * sizeof(T)) == 0);
}

template <class T>
void read_array(core::BlobReader &r_r, uint32_t p_mask, uint32_t p_bit, size_t p_per_vertex, size_t p_n, std::vector<T> &r_out) {
	if ((p_mask & p_bit) == 0) {
		r_out.clear();
		return;
	}
	r_r.align(4);
	r_r.array<T>(p_per_vertex * p_n, r_out);
}

template <class T>
void write_array(core::BlobWriter &r_w, const std::vector<T> &p_values, bool p_always = false) {
	if (p_values.empty() && !p_always) {
		return;
	}
	r_w.align(4);
	r_w.array(p_values);
}

}

uint64_t PreparedMeshSet::vertices() const {
	uint64_t n = 0;
	for (const PreparedMesh &m : meshes) {
		n += m.vertices;
	}
	return n;
}

uint64_t PreparedMeshSet::triangles() const {
	uint64_t n = 0;
	for (const PreparedMesh &m : meshes) {
		n += m.triangles;
	}
	return n;
}

std::vector<uint8_t> encode_mesh_blob(const PreparedMeshSet &p_mesh) {
	core::BlobWriter w;
	w.magic(kMeshMagic, kMeshBlobFormatVersion);
	w.str(p_mesh.name);
	w.boolean(!p_mesh.meshes.empty() && p_mesh.meshes[0].skinned);
	w.u64(p_mesh.meshes.size());
	for (size_t l = 0; l < p_mesh.meshes.size(); ++l) {
		const PreparedMesh &m = p_mesh.meshes[l];
		w.u32(l < p_mesh.lods.size() ? p_mesh.lods[l] : uint32_t(l));
		w.u64(m.surfaces.size());
		for (const PreparedSurface &s : m.surfaces) {
			w.u64(s.subset);
			w.u32(s.material_index);
			w.str(s.material_name);
			w.u8(s.category ? uint8_t(*s.category) : kNoCategory);
			w.u32(uint32_t(s.vertex_count()));
			w.u32(uint32_t(s.indices.size()));
			uint32_t mask = 0;
			mask |= s.normals.empty() ? 0 : kNormals;
			mask |= s.tangents.empty() ? 0 : kTangents;
			mask |= s.uv0.empty() ? 0 : kUv0;
			mask |= s.uv1.empty() ? 0 : kUv1;
			mask |= s.colors.empty() ? 0 : kColors;
			mask |= s.bones.empty() ? 0 : kBones;
			mask |= s.weights.empty() ? 0 : kWeights;
			mask |= s.parts.empty() ? 0 : kParts;
			w.u32(mask);
			for (float v : s.aabb_min) {
				w.f32(v);
			}
			for (float v : s.aabb_max) {
				w.f32(v);
			}
			write_array(w, s.positions, true);
			write_array(w, s.normals);
			write_array(w, s.tangents);
			write_array(w, s.uv0);
			write_array(w, s.uv1);
			write_array(w, s.colors);
			write_array(w, s.bones);
			write_array(w, s.weights);
			write_array(w, s.parts);
			write_array(w, s.indices, true);
		}
	}
	return w.take();
}

core::Result<PreparedMeshSet> decode_mesh_blob(std::span<const uint8_t> p_blob) {
	core::BlobReader r(p_blob);
	const uint32_t version = r.magic(kMeshMagic);
	if (r.ok() && version != kMeshBlobFormatVersion) {
		return core::make_error("mesh blob: format version " + std::to_string(version) + ", expected " + std::to_string(kMeshBlobFormatVersion));
	}
	PreparedMeshSet out;
	out.name = r.str(kMaxName);
	const bool skinned = r.boolean();

	const size_t lods = r.count(12, kMaxLodIndex);
	out.lods.reserve(lods);
	out.meshes.reserve(lods);
	for (size_t l = 0; l < lods && r.ok(); ++l) {
		const uint32_t lod = r.u32();
		if (r.ok() && (lod >= kMaxLodIndex || (!out.lods.empty() && lod <= out.lods.back()))) {
			r.fail("LOD index " + std::to_string(lod) + " out of order");
		}
		PreparedMesh m;
		m.name = out.name;
		m.skinned = skinned;
		const size_t surfaces = r.count(53);
		m.surfaces.reserve(surfaces);
		for (size_t i = 0; i < surfaces && r.ok(); ++i) {
			PreparedSurface s;
			s.subset = static_cast<size_t>(r.u64());
			s.material_index = r.u32();
			s.material_name = r.str(kMaxName);
			const uint8_t category = r.u8();
			if (category != kNoCategory) {
				if (category > uint8_t(core::res::SubsetCategory::ZOnly)) {
					r.fail("subset category " + std::to_string(category));
				}
				s.category = core::res::SubsetCategory(category);
			}
			const uint32_t n = r.u32();
			const uint32_t index_count = r.u32();
			const uint32_t mask = r.u32();
			if (r.ok() && (mask & ~uint32_t(kKnownBits)) != 0) {
				r.fail("unknown array bits " + std::to_string(mask));
			}
			if (r.ok() && index_count % 3 != 0) {
				r.fail("index count " + std::to_string(index_count) + " not a multiple of 3");
			}
			if (r.ok() && skinned != ((mask & kBones) != 0 && (mask & kWeights) != 0)) {
				r.fail(skinned ? "skinned mesh surface without bones/weights" : "bones/weights on an unskinned mesh");
			}
			for (float &v : s.aabb_min) {
				v = r.f32();
			}
			for (float &v : s.aabb_max) {
				v = r.f32();
			}
			read_array(r, ~0u, ~0u, 3, n, s.positions);
			read_array(r, mask, kNormals, 3, n, s.normals);
			read_array(r, mask, kTangents, 4, n, s.tangents);
			read_array(r, mask, kUv0, 2, n, s.uv0);
			read_array(r, mask, kUv1, 2, n, s.uv1);
			read_array(r, mask, kColors, 4, n, s.colors);
			read_array(r, mask, kBones, 4, n, s.bones);
			read_array(r, mask, kWeights, 4, n, s.weights);
			read_array(r, mask, kParts, 1, n, s.parts);
			r.align(4);
			r.array<int32_t>(index_count, s.indices);
			if (!r.ok()) {
				break;
			}
			for (int32_t index : s.indices) {
				if (index < 0 || uint32_t(index) >= n) {
					r.fail("index " + std::to_string(index) + " outside the surface's " + std::to_string(n) + " vertices");
					break;
				}
			}
			for (int32_t bone : s.bones) {
				if (bone < 0) {
					r.fail("negative bone index");
					break;
				}
			}
			m.vertices += n;
			m.triangles += index_count / 3;
			m.surfaces.push_back(std::move(s));
		}
		out.lods.push_back(lod);
		out.meshes.push_back(std::move(m));
	}
	if (r.ok() && !r.at_end()) {
		r.fail(std::to_string(r.remaining()) + " trailing bytes");
	}
	if (!r.ok()) {
		return r.error("mesh blob");
	}
	return out;
}

core::Result<MeshHead> read_mesh_head(const core::LevelMount &p_mount, std::string_view p_name) {
	core::Result<core::res::RawRes> raw = core::res::read_raw_res(p_mount, p_name);
	if (!raw) {
		return raw.error();
	}
	if (raw->res_type != core::res::kResTypeMeshSet) {
		return core::make_error("res " + std::string(p_name) + ": not a MeshSet");
	}
	MeshHead head;
	head.name = std::string(p_name);
	head.res = std::move(*raw);
	const core::res::ResError e = core::res::MeshSet::parse(head.res.bytes(), head.res.res_meta, head.mesh);
	if (!e.ok()) {
		return core::make_error("res " + std::string(p_name) + ": " + e.message());
	}
	if (const std::optional<core::Sha1> sha1 = p_mount.entry_sha1(core::EntryRef::res(p_name))) {
		head.res_sha1 = *sha1;
		head.digests = true;
	}
	return head;
}

core::CacheKeyInputs mesh_key_inputs(const std::vector<core::Sha1Digest> &p_sources, const MeshRequest &p_request, uint32_t p_converter_version) {
	core::CacheKeyInputs in;
	in.kind = kMeshCacheKind;
	in.sources = p_sources;
	std::string lods;
	for (const uint32_t l : p_request.lods) {
		lods += (lods.empty() ? "" : ",") + std::to_string(l);
	}
	std::string binds = "none";
	if (p_request.lane_binds != nullptr) {
		core::BlobWriter w;
		w.u64(p_request.lane_binds->size());
		for (const std::vector<int32_t> &lanes : *p_request.lane_binds) {
			w.u64(lanes.size());
			w.array(lanes);
		}
		binds = core::sha256_of(w.take()).hex();
	}
	in.converter_version = p_converter_version;
	in.engine = std::string(core::cache_engine_id());
	in.format_version = kMeshBlobFormatVersion;
	in.params = "lods=" + lods + ";filter=" + (p_request.filter == core::res::SubsetFilter::All ? "all" : "drawn") + ";binds=" + binds;
	return in;
}

std::optional<core::CacheKey> mesh_cache_key(const core::LevelMount &p_mount, const MeshHead &p_head, const MeshRequest &p_request) {
	if (!p_head.digests || p_request.lods.empty()) {
		return std::nullopt;
	}
	std::vector<core::Sha1Digest> sources{ core::source_digest(p_head.res_sha1) };
	std::vector<core::Guid> chunks;
	for (const uint32_t l : p_request.lods) {
		if (l >= p_head.mesh.lods.size()) {
			return std::nullopt;
		}
		const core::res::GeometrySource &g = p_head.mesh.lods[l].geometry;
		if (g.is_inline) {
			continue;
		}
		const core::Guid id = core::Guid::from_bytes(g.chunk.bytes.data());
		if (std::find(chunks.begin(), chunks.end(), id) != chunks.end()) {
			continue;
		}
		chunks.push_back(id);
		const std::vector<core::Sha1> sha1s = p_mount.chunk_source_sha1s(id);
		if (sha1s.empty()) {
			return std::nullopt;
		}
		for (const core::Sha1 &s : sha1s) {
			sources.push_back(core::source_digest(s));
		}
	}
	return core::make_cache_key(mesh_key_inputs(sources, p_request));
}

core::Result<core::res::LoadedMeshSet> load_mesh_lods(const core::LevelMount &p_mount, const MeshHead &p_head, const std::vector<uint32_t> &p_lods) {
	core::res::LoadedMeshSet out;
	out.res = p_head.res;
	out.mesh = p_head.mesh;
	size_t count = 0;
	for (const uint32_t l : p_lods) {
		if (l >= out.mesh.lods.size()) {
			return core::make_error("mesh " + p_head.name + ": no LOD " + std::to_string(l));
		}
		count = std::max<size_t>(count, size_t(l) + 1);
	}
	for (size_t l = 0; l < count; ++l) {
		const core::res::MeshLod &lod = out.mesh.lods[l];
		if (lod.geometry.is_inline) {
			out.lod_chunk.push_back(-1);
			continue;
		}
		const auto same = std::find_if(out.chunks.begin(), out.chunks.end(), [&](const core::res::RawChunk &c) { return c.id == lod.geometry.chunk; });
		if (same != out.chunks.end()) {
			out.lod_chunk.push_back(int32_t(same - out.chunks.begin()));
			continue;
		}
		core::Result<core::res::RawChunk> chunk = core::res::read_raw_chunk(p_mount, lod.geometry.chunk);
		if (!chunk) {
			return core::error_at("res " + p_head.name, chunk.error());
		}
		out.lod_chunk.push_back(int32_t(out.chunks.size()));
		out.chunks.push_back(std::move(*chunk));
	}
	return out;
}

core::Result<PreparedMeshSet> prepare_mesh_set(const core::res::LoadedMeshSet &p_mesh, const MeshRequest &p_request) {
	if (p_request.lane_binds != nullptr && p_request.lods.size() != 1) {
		return core::make_error("mesh " + p_mesh.res.name + ": lane binds belong to one LOD");
	}
	PreparedMeshSet out;
	out.name = p_mesh.res.name;
	for (size_t i = 0; i < p_request.lods.size(); ++i) {
		if (i > 0 && p_request.lods[i] <= p_request.lods[i - 1]) {
			return core::make_error("mesh " + p_mesh.res.name + ": LODs not ascending");
		}
		core::Result<PreparedMesh> m = prepare_mesh(p_mesh, p_request.lods[i], p_request.filter, p_request.lane_binds);
		if (!m) {
			return m.error();
		}
		out.lods.push_back(p_request.lods[i]);
		out.meshes.push_back(std::move(*m));
	}
	return out;
}

core::Result<CachedMesh> load_mesh_cached(core::Cache *p_cache, const core::LevelMount &p_mount, const MeshHead &p_head, const MeshRequest &p_request) {
	CachedMesh out;
	const std::optional<core::CacheKey> key = p_cache != nullptr ? mesh_cache_key(p_mount, p_head, p_request) : std::nullopt;
	auto produce = [&]() -> core::Result<std::vector<uint8_t>> {
		core::Result<core::res::LoadedMeshSet> loaded = load_mesh_lods(p_mount, p_head, p_request.lods);
		if (!loaded) {
			return loaded.error();
		}
		core::Result<PreparedMeshSet> prepared = prepare_mesh_set(*loaded, p_request);
		if (!prepared) {
			return prepared.error();
		}
		out.mesh = std::move(*prepared);
		return encode_mesh_blob(out.mesh);
	};
	if (!key) {
		core::Result<std::vector<uint8_t>> blob = produce();
		if (!blob) {
			return blob.error();
		}
		out.item.outcome = core::CacheOutcome::Uncached;
		out.item.payload_bytes = blob->size();
		return out;
	}
	out.key_known = true;
	auto accept = [&](std::span<const uint8_t> p_blob) -> core::Status {
		core::Result<PreparedMeshSet> decoded = decode_mesh_blob(p_blob);
		if (!decoded) {
			return decoded.error();
		}
		if (decoded->lods != p_request.lods || (!decoded->meshes.empty() && decoded->meshes[0].skinned != (p_request.lane_binds != nullptr))) {
			return core::make_error("mesh blob: holds other LODs than its key names");
		}
		out.mesh = std::move(*decoded);
		return core::Status::success();
	};
	core::Result<core::CachedItem> item = core::cached_convert(p_cache, *key, accept, produce);
	if (!item) {
		return item.error();
	}
	out.item = std::move(*item);

	out.mesh.name = p_head.name;
	for (PreparedMesh &m : out.mesh.meshes) {
		m.name = p_head.name;
	}
	return out;
}

core::Result<CachedMesh> load_mesh_cached(core::Cache *p_cache, const core::LevelMount &p_mount, std::string_view p_name, const MeshRequest &p_request) {
	core::Result<MeshHead> head = read_mesh_head(p_mount, p_name);
	if (!head) {
		return head.error();
	}
	return load_mesh_cached(p_cache, p_mount, *head, p_request);
}

bool same_prepared(const PreparedMesh &p_a, const PreparedMesh &p_b) {
	if (p_a.skinned != p_b.skinned || p_a.surfaces.size() != p_b.surfaces.size() || p_a.vertices != p_b.vertices || p_a.triangles != p_b.triangles) {
		return false;
	}
	for (size_t i = 0; i < p_a.surfaces.size(); ++i) {
		const PreparedSurface &a = p_a.surfaces[i], &b = p_b.surfaces[i];
		if (a.subset != b.subset || a.material_index != b.material_index || a.material_name != b.material_name || a.category != b.category ||
				std::memcmp(a.aabb_min.data(), b.aabb_min.data(), 12) != 0 || std::memcmp(a.aabb_max.data(), b.aabb_max.data(), 12) != 0 ||
				!same_bytes(a.positions, b.positions) || !same_bytes(a.normals, b.normals) || !same_bytes(a.tangents, b.tangents) ||
				!same_bytes(a.uv0, b.uv0) || !same_bytes(a.uv1, b.uv1) || !same_bytes(a.colors, b.colors) || !same_bytes(a.bones, b.bones) ||
				!same_bytes(a.weights, b.weights) || !same_bytes(a.parts, b.parts) || !same_bytes(a.indices, b.indices)) {
			return false;
		}
	}
	return true;
}

}
