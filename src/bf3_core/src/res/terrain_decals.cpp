#include "bf3/core/res/terrain_decals.h"

#include "res_reader.h"

#include <algorithm>
#include <cmath>

namespace bf3::core::res {

namespace {

using detail::Reader;

enum class Layout : uint8_t { Planar, Surface, Water };

size_t vertex_bytes(Layout p_layout) {
	switch (p_layout) {
		case Layout::Planar:
			return kTerrainDecalPlanarVertexBytes;
		case Layout::Surface:
			return kTerrainDecalSurfaceVertexBytes;
		case Layout::Water:
			return kTerrainDecalWaterVertexBytes;
	}
	return 0;
}

float half(Reader &r, uint64_t p_at) {
	return half_to_f32(r.u16(p_at));
}

Vec4 half4(Reader &r, uint64_t p_at) {
	return { half(r, p_at), half(r, p_at + 2), half(r, p_at + 4), half(r, p_at + 6) };
}

TerrainDecalVertex read_vertex(Reader &r, uint64_t p_at, Layout p_layout) {
	TerrainDecalVertex v;
	switch (p_layout) {
		case Layout::Planar:
			v.position = { r.f32(p_at), 0.0f, r.f32(p_at + 4) };
			v.uv = { r.f32(p_at + 8), r.f32(p_at + 12) };
			v.mask = half4(r, p_at + 16);
			break;
		case Layout::Surface:
			v.position = r.vec3(p_at);
			v.uv = { r.f32(p_at + 12), r.f32(p_at + 16) };
			v.mask = half4(r, p_at + 20);
			break;
		case Layout::Water: {
			const Vec4 p = half4(r, p_at);
			const Vec4 n = half4(r, p_at + 8);
			const Vec4 t = half4(r, p_at + 16);
			v.position = { p[0], p[1], p[2] };
			v.normal = { n[0], n[1], n[2] };
			v.tangent = { t[0], t[1], t[2] };
			v.uv = { half(r, p_at + 24), half(r, p_at + 26) };
			v.mask = half4(r, p_at + 28);
			break;
		}
	}
	return v;
}

bool read_set(Reader &r, uint64_t &r_at, Layout p_layout, TerrainDecalSet &r_set) {
	const uint32_t count = r.u32(r_at);
	r_at += 4;
	if (!r.ok()) {
		return false;
	}
	if (count > kMaxTerrainDecalRecords) {
		return r.fail(ResError::unsupported("terrain decal record count", count));
	}
	if (count == 0) {
		return true;
	}
	r_set.records.reserve(count);
	uint32_t vertices = 0;
	uint64_t indices = 0;
	for (uint32_t i = 0; i < count; ++i) {
		TerrainDecalRecord rec;
		if (!r.cstr(r_at, rec.shader)) {
			return false;
		}
		r_at += rec.shader.size() + 1;
		rec.flag = r.u8(r_at);
		rec.first_index = r.u32(r_at + 1);
		rec.triangle_count = r.u32(r_at + 5);
		rec.bounds_min = r.vec3(r_at + 9);
		rec.bounds_max = r.vec3(r_at + 21);
		rec.patch_min = r.vec3(r_at + 33);
		rec.patch_max = r.vec3(r_at + 45);
		rec.inverse_order = r.f32(r_at + 57);
		rec.first_vertex = r.u32(r_at + 61);
		rec.end_vertex = r.u32(r_at + 65);
		if (!r.ok()) {
			return false;
		}
		if (rec.end_vertex < rec.first_vertex || rec.end_vertex > kMaxTerrainDecalVertices) {
			return r.fail(ResError::malformed("terrain decal vertex range", r_at + 61));
		}
		if (uint64_t(rec.first_index) + uint64_t(rec.triangle_count) * 3 > kMaxTerrainDecalIndices) {
			return r.fail(ResError::malformed("terrain decal index range", r_at + 1));
		}
		r_at += 69;
		vertices = std::max(vertices, rec.end_vertex);
		indices = std::max(indices, uint64_t(rec.first_index) + uint64_t(rec.triangle_count) * 3);
		r_set.records.push_back(std::move(rec));
	}
	const uint32_t vertex_size = r.u32(r_at);
	const uint32_t index_size = r.u32(r_at + 4);
	r_at += 8;
	if (!r.ok()) {
		return false;
	}
	const size_t stride = vertex_bytes(p_layout);
	if (vertex_size != uint64_t(vertices) * stride) {
		return r.fail(ResError::malformed("terrain decal vertex bytes", r_at - 8));
	}
	if (index_size != indices * 4) {
		return r.fail(ResError::malformed("terrain decal index bytes", r_at - 4));
	}
	if (!r.has(r_at, uint64_t(vertex_size) + index_size)) {
		return false;
	}
	r_set.vertices.reserve(vertices);
	for (uint32_t v = 0; v < vertices; ++v) {
		r_set.vertices.push_back(read_vertex(r, r_at + uint64_t(v) * stride, p_layout));
	}
	r_at += vertex_size;
	r_set.indices.reserve(size_t(indices));
	for (uint64_t i = 0; i < indices; ++i) {
		r_set.indices.push_back(r.u32(r_at + i * 4));
	}
	r_at += index_size;
	for (const TerrainDecalRecord &rec : r_set.records) {
		for (uint64_t i = 0; i < uint64_t(rec.triangle_count) * 3; ++i) {
			const uint32_t index = r_set.indices[size_t(rec.first_index + i)];
			if (index < rec.first_vertex || index >= rec.end_vertex) {
				return r.fail(ResError::malformed("terrain decal index outside its record", r_at));
			}
		}
	}
	return r.ok();
}

}

int32_t TerrainDecalRecord::draw_order() const {
	if (!(inverse_order > 0.0f) || !std::isfinite(inverse_order)) {
		return 0;
	}
	return int32_t(std::lround(1.0 / double(inverse_order))) - 1;
}

ResError TerrainDecals::parse(Bytes p_res, TerrainDecals &r_out) {
	Reader r(p_res);
	TerrainDecals out;
	out.head_a = r.f32(0);
	out.head_b = r.f32(4);
	out.head_c = r.u32(8);
	out.head_d = r.u8(12);
	uint64_t at = 13;
	if (!r.ok()) {
		return r.error();
	}
	if (!read_set(r, at, Layout::Planar, out.planar) || !read_set(r, at, Layout::Surface, out.surface) ||
			!read_set(r, at, Layout::Water, out.water)) {
		return r.error();
	}
	if (at != r.size()) {
		return ResError::malformed("terrain decals trailing bytes", at);
	}
	r_out = std::move(out);
	return ResError{};
}

}
