// MeshSet decoder: port of IW4L crates/resource_bf3/src/mesh.rs. Reads happen in the same order as
// there, so the first error reported for a damaged payload is the same one.
#include "bf3/core/res/mesh_set.h"

#include "res_reader.h"

#include <cmath>

namespace bf3::core::res {

using detail::align16;
using detail::mul;
using detail::off;
using detail::Reader;

namespace {

constexpr uint32_t kLodSlots = 5;
constexpr uint64_t kSubsetBytes = 148;
constexpr uint64_t kPartMaskBytes = 24;

// Rust `f as u16` / `f as u8`: saturating, NaN to 0.
template <typename T>
T saturate_cast(float p_value) {
	if (!(p_value > 0.0f)) {
		return 0;
	}
	if (p_value >= float(T(~T(0)))) {
		return T(~T(0));
	}
	return T(p_value);
}

ResError parse_subset(Reader &p_r, uint64_t p_at, MeshSubset &r_subset) {
	MeshSubset &s = r_subset;
	s.material_name = p_r.optional_cstr(p_r.u64(off(p_at, 0x08)));
	const uint8_t bone_count = p_r.u8(off(p_at, 0x27));
	const uint64_t bone_slots_at = p_r.u64(off(p_at, 0x28));
	if (!p_r.ok()) {
		return p_r.error();
	}
	if (bone_count > 0 && bone_slots_at != 0) {
		for (uint64_t slot = 0; slot < bone_count; slot++) {
			s.bone_slots.push_back(p_r.u16(off(bone_slots_at, 2 * slot)));
		}
	}
	const uint64_t decl_at = off(p_at, 0x30);
	for (uint64_t i = 0; i < 16; i++) {
		const uint64_t e = off(decl_at, 4 * i);
		VertexElement &element = s.declaration.elements[i];
		element.usage = VertexUsage(p_r.u8(e));
		element.format = VertexFormat(p_r.u8(off(e, 1)));
		element.offset = p_r.u8(off(e, 2));
		element.stream = p_r.u8(off(e, 3));
	}
	for (uint64_t i = 0; i < 4; i++) {
		s.declaration.stream_strides[i] = p_r.u8(off(decl_at, 0x40 + 2 * i));
	}
	for (uint64_t i = 0; i < 6; i++) {
		s.tex_coord_ratios[i] = p_r.f32(off(p_at, 0x7C + 4 * i));
	}
	s.material_index = p_r.u32(off(p_at, 0x10));
	s.primitive_count = p_r.u32(off(p_at, 0x14));
	s.start_index = p_r.u32(off(p_at, 0x18));
	s.vertex_offset = p_r.u32(off(p_at, 0x1C));
	s.vertex_count = p_r.u32(off(p_at, 0x20));
	s.vertex_stride = p_r.u8(off(p_at, 0x24));
	s.primitive_type = PrimitiveType(p_r.u8(off(p_at, 0x25)));
	s.bones_per_vertex = p_r.u8(off(p_at, 0x26));
	s.declaration.element_count = p_r.u8(off(decl_at, 0x48));
	s.declaration.stream_count = p_r.u8(off(decl_at, 0x49));
	return p_r.error();
}

ResError parse_lod(Reader &p_r, uint64_t p_at, MeshKind p_kind, uint64_t &r_inline_cursor, MeshLod &r_lod) {
	MeshLod &lod = r_lod;
	const uint64_t subset_count = p_r.u32(off(p_at, 0x04));
	const uint64_t subsets_at = p_r.u64(off(p_at, 0x08));
	for (uint64_t c = 0; c < 4; c++) {
		const uint64_t field = off(p_at, 0x10 + 12 * c);
		const uint64_t count = p_r.u32(field);
		if (!p_r.ok()) {
			return p_r.error();
		}
		if (count > 0) {
			const uint64_t list_at = p_r.u64(off(field, 4));
			const Bytes list = p_r.slice(list_at, count);
			lod.category_subsets[c].assign(list.begin(), list.end());
		}
	}
	lod.flags = p_r.u32(off(p_at, 0x40));
	const uint32_t index_format = p_r.u32(off(p_at, 0x44));
	if (!p_r.ok()) {
		return p_r.error();
	}
	if (index_format > 1) {
		return ResError::unsupported("index buffer format", index_format);
	}
	lod.index_format = index_format == 0 ? IndexFormat::U16 : IndexFormat::U32;
	lod.index_data_size = p_r.u32(off(p_at, 0x48));
	lod.vertex_data_size = p_r.u32(off(p_at, 0x4C));
	const ChunkId chunk = p_r.chunk_id(off(p_at, 0x54));
	if (!p_r.ok()) {
		return p_r.error();
	}
	if (chunk.is_zero()) {
		const uint64_t len = uint64_t(lod.vertex_data_size) + lod.index_data_size;
		lod.geometry.is_inline = true;
		lod.geometry.offset = r_inline_cursor;
		lod.geometry.len = len;
		if (!p_r.has(r_inline_cursor, len)) {
			return p_r.error();
		}
		r_inline_cursor = off(r_inline_cursor, align16(len));
	} else {
		lod.geometry.chunk = chunk;
	}
	lod.name = p_r.optional_cstr(p_r.u64(off(p_at, 0x78)));
	lod.short_name = p_r.optional_cstr(p_r.u64(off(p_at, 0x80)));
	lod.name_hash = p_r.u32(off(p_at, 0x88));

	bool has_part_masks = false;
	uint64_t part_masks_at = 0;
	if (p_kind != MeshKind::Rigid) {
		const uint64_t count = p_r.u32(off(p_at, 0x94));
		const uint64_t first_at = p_r.u64(off(p_at, 0x98));
		const uint64_t second_at = p_r.u64(off(p_at, 0xA0));
		if (!p_r.ok()) {
			return p_r.error();
		}
		if (p_kind == MeshKind::Composite) {
			for (uint64_t part = 0; part < count; part++) {
				const uint64_t bounds_at = off(first_at, mul(32, part));
				const uint64_t transform_at = off(second_at, mul(64, part));
				MeshPart p;
				p.bounds_min = p_r.vec3(bounds_at);
				p.bounds_max = p_r.vec3(off(bounds_at, 16));
				p.transform.axis[0] = p_r.vec3(transform_at);
				p.transform.axis[1] = p_r.vec3(off(transform_at, 16));
				p.transform.axis[2] = p_r.vec3(off(transform_at, 32));
				p.transform.origin = p_r.vec3(off(transform_at, 48));
				if (!p_r.ok()) {
					return p_r.error();
				}
				lod.parts.push_back(p);
			}
			const uint64_t masks = p_r.u64(off(p_at, 0xA8));
			if (masks != 0) {
				has_part_masks = true;
				part_masks_at = masks;
			}
		} else {
			for (uint64_t bone = 0; bone < count; bone++) {
				SkeletonBone b;
				b.index = p_r.u32(off(first_at, mul(4, bone)));
				b.name_hash = p_r.u32(off(second_at, mul(4, bone)));
				if (!p_r.ok()) {
					return p_r.error();
				}
				lod.bones.push_back(b);
			}
		}
	}
	if (!p_r.ok()) {
		return p_r.error();
	}

	for (uint64_t index = 0; index < subset_count; index++) {
		MeshSubset subset;
		const ResError e = parse_subset(p_r, off(subsets_at, mul(kSubsetBytes, index)), subset);
		if (!e.ok()) {
			return e;
		}
		for (size_t c = 0; c < 4 && !subset.category; c++) {
			for (uint8_t entry : lod.category_subsets[c]) {
				if (entry == index) {
					subset.category = SubsetCategory(c);
					break;
				}
			}
		}
		if (has_part_masks) {
			const uint64_t mask_at = off(part_masks_at, mul(kPartMaskBytes, index));
			for (uint64_t word = 0; word < 6; word++) {
				subset.part_mask[word] = p_r.u32(off(mask_at, 4 * word));
			}
			if (!p_r.ok()) {
				return p_r.error();
			}
		}
		lod.subsets.push_back(std::move(subset));
	}
	return ResError();
}

// Channel reader over one subset's vertices. All per-vertex checks of IW4L's element_bytes are
// vertex-independent once the subset's byte range is known to be inside the vertex region, so they
// run once per channel (and only when there is a vertex, as there).
struct VertexReader {
	Bytes region;
	uint64_t base = 0;
	uint64_t stride = 0;
	uint64_t count = 0;

	ResError check(const VertexElement &p_e, int &r_size) const {
		if (p_e.stream != 0) {
			return ResError::unsupported("vertex stream", p_e.stream);
		}
		r_size = vertex_format_bytes(p_e.format);
		if (r_size < 0) {
			return ResError::unsupported("vertex format", uint32_t(p_e.format));
		}
		if (uint64_t(p_e.offset) + uint64_t(r_size) > stride) {
			return ResError::malformed("vertex element outside its stride", p_e.offset);
		}
		return ResError();
	}
	Bytes element(const VertexElement &p_e, uint64_t p_vertex, int p_size) const {
		return region.subspan(size_t(base + p_vertex * stride + p_e.offset), size_t(p_size));
	}
	template <size_t N>
	ResError collect(const VertexElement *p_e, std::optional<std::vector<std::array<float, N>>> &r_out) const {
		if (p_e == nullptr) {
			return ResError();
		}
		std::vector<std::array<float, N>> out;
		out.reserve(size_t(count));
		int size = 0;
		if (count > 0) {
			const ResError e = check(*p_e, size);
			if (!e.ok()) {
				return e;
			}
		}
		for (uint64_t v = 0; v < count; v++) {
			Vec4 lanes;
			vertex_format_decode(p_e->format, element(*p_e, v, size), lanes);
			std::array<float, N> item;
			for (size_t k = 0; k < N; k++) {
				item[k] = lanes[k];
			}
			out.push_back(item);
		}
		r_out = std::move(out);
		return ResError();
	}
};

ResError decode_vertices(const MeshSubset &p_subset, Bytes p_region, DecodedSubset &r_out) {
	Reader r(p_region);
	VertexReader vr;
	vr.region = p_region;
	vr.base = p_subset.vertex_offset;
	vr.stride = p_subset.vertex_stride;
	vr.count = p_subset.vertex_count;
	if (!r.has(vr.base, vr.stride * vr.count)) {
		return r.error();
	}
	const VertexDeclaration &decl = p_subset.declaration;
	ResError e;

	std::optional<std::vector<Vec3>> positions;
	e = vr.collect<3>(decl.find(VertexUsage::Position), positions);
	if (!e.ok()) {
		return e;
	}
	if (!positions) {
		return ResError::malformed("subset without positions", vr.base);
	}
	r_out.positions = std::move(*positions);
	e = vr.collect<3>(decl.find(VertexUsage::Normal), r_out.normals);
	if (!e.ok()) {
		return e;
	}
	e = vr.collect<3>(decl.find(VertexUsage::Tangent), r_out.tangents);
	if (!e.ok()) {
		return e;
	}
	if (const VertexElement *sign = decl.find(VertexUsage::BinormalSign)) {
		std::optional<std::vector<std::array<float, 1>>> lanes;
		e = vr.collect<1>(sign, lanes);
		if (!e.ok()) {
			return e;
		}
		std::vector<float> signs;
		signs.reserve(lanes->size());
		for (const auto &l : *lanes) {
			signs.push_back(l[0] < 0.0f ? -1.0f : 1.0f);
		}
		r_out.binormal_signs = std::move(signs);
	} else {
		std::optional<std::vector<Vec3>> binormals;
		e = vr.collect<3>(decl.find(VertexUsage::Binormal), binormals);
		if (!e.ok()) {
			return e;
		}
		if (r_out.normals && r_out.tangents && binormals) {
			std::vector<float> signs;
			signs.reserve(size_t(vr.count));
			for (size_t v = 0; v < size_t(vr.count); v++) {
				const float d = vec::dot(vec::cross((*r_out.normals)[v], (*r_out.tangents)[v]), (*binormals)[v]);
				signs.push_back(d < 0.0f ? -1.0f : 1.0f);
			}
			r_out.binormal_signs = std::move(signs);
		}
	}
	if (const VertexElement *color = decl.find(VertexUsage::Color0)) {
		std::vector<std::array<uint8_t, 4>> colors;
		colors.reserve(size_t(vr.count));
		int size = 0;
		if (vr.count > 0) {
			e = vr.check(*color, size);
			if (!e.ok()) {
				return e;
			}
		}
		const bool raw = color->format == VertexFormat::UByte4N || color->format == VertexFormat::UByte4;
		for (uint64_t v = 0; v < vr.count; v++) {
			const Bytes b = vr.element(*color, v, size);
			std::array<uint8_t, 4> c{};
			if (raw) {
				c = { b[0], b[1], b[2], b[3] };
			} else {
				Vec4 lanes;
				vertex_format_decode(color->format, b, lanes);
				for (int k = 0; k < 4; k++) {
					float x = lanes[k];
					x = x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
					c[k] = saturate_cast<uint8_t>(std::round(x * 255.0f));
				}
			}
			colors.push_back(c);
		}
		r_out.colors = std::move(colors);
	}
	bool any_part = false;
	for (uint32_t word : p_subset.part_mask) {
		any_part = any_part || word != 0;
	}
	const VertexElement *bone_indices = decl.find(VertexUsage::BoneIndices);
	if (bone_indices != nullptr && any_part) {
		std::vector<uint16_t> parts;
		parts.reserve(size_t(vr.count));
		int size = 0;
		if (vr.count > 0) {
			e = vr.check(*bone_indices, size);
			if (!e.ok()) {
				return e;
			}
		}
		for (uint64_t v = 0; v < vr.count; v++) {
			parts.push_back(vr.element(*bone_indices, v, size)[0]);
		}
		r_out.parts = std::move(parts);
	}
	e = vr.collect<2>(decl.find(tex_coord_usage(0)), r_out.uv0);
	if (!e.ok()) {
		return e;
	}
	e = vr.collect<2>(decl.find(tex_coord_usage(1)), r_out.uv1);
	if (!e.ok()) {
		return e;
	}
	e = vr.collect<2>(decl.find(tex_coord_usage(2)), r_out.uv2);
	if (!e.ok()) {
		return e;
	}
	e = vr.collect<2>(decl.find(VertexUsage::RadiosityTexCoord), r_out.radiosity_uv);
	if (!e.ok()) {
		return e;
	}
	// Bone channels (not decoded by IW4L; checked after everything it decodes).
	const VertexUsage index_usages[2] = { VertexUsage::BoneIndices, VertexUsage::BoneIndices2 };
	std::optional<std::vector<std::array<uint16_t, 4>>> *index_outs[2] = { &r_out.bone_indices, &r_out.bone_indices2 };
	for (int k = 0; k < 2; k++) {
		std::optional<std::vector<Vec4>> lanes;
		e = vr.collect<4>(decl.find(index_usages[k]), lanes);
		if (!e.ok()) {
			return e;
		}
		if (lanes) {
			std::vector<std::array<uint16_t, 4>> out;
			out.reserve(lanes->size());
			for (const Vec4 &l : *lanes) {
				out.push_back({ saturate_cast<uint16_t>(l[0]), saturate_cast<uint16_t>(l[1]), saturate_cast<uint16_t>(l[2]),
						saturate_cast<uint16_t>(l[3]) });
			}
			*index_outs[k] = std::move(out);
		}
	}
	e = vr.collect<4>(decl.find(VertexUsage::BoneWeights), r_out.bone_weights);
	if (!e.ok()) {
		return e;
	}
	return vr.collect<4>(decl.find(VertexUsage::BoneWeights2), r_out.bone_weights2);
}

ResError decode_indices(const MeshSubset &p_subset, Bytes p_region, IndexFormat p_format, std::vector<uint32_t> &r_indices) {
	Reader r(p_region);
	const uint64_t width = uint64_t(index_format_bytes(p_format));
	auto read = [&](uint64_t p_element) -> uint32_t {
		const uint64_t at = p_element * width;
		return width == 2 ? r.u16(at) : r.u32(at);
	};
	const uint64_t start = p_subset.start_index;
	const uint64_t primitives = p_subset.primitive_count;
	switch (p_subset.primitive_type) {
		case PrimitiveType::TriangleList: {
			const uint64_t end = start + primitives * 3;
			if (end > start && r.has(start * width, (end - start) * width)) {
				r_indices.reserve(size_t(end - start));
				for (uint64_t element = start; element < end; element++) {
					r_indices.push_back(read(element));
				}
			} else {
				// Short region: find the element IW4L stops at.
				Reader probe(p_region);
				for (uint64_t element = start; element < end; element++) {
					if (!probe.has(element * width, width)) {
						return probe.error();
					}
				}
			}
			break;
		}
		case PrimitiveType::TriangleStrip: {
			std::vector<uint32_t> strip;
			for (uint64_t element = start; element < start + primitives + 2; element++) {
				const uint32_t v = read(element);
				if (!r.ok()) {
					return r.error();
				}
				strip.push_back(v);
			}
			for (size_t t = 0; t + 2 < strip.size(); t++) {
				const uint32_t a = strip[t], b = strip[t + 1], c = strip[t + 2];
				if (a == b || b == c || a == c) {
					continue;
				}
				if (t % 2 == 0) {
					r_indices.insert(r_indices.end(), { a, b, c });
				} else {
					r_indices.insert(r_indices.end(), { b, a, c });
				}
			}
			break;
		}
		default:
			return ResError::unsupported("primitive type", uint32_t(p_subset.primitive_type));
	}
	for (uint32_t index : r_indices) {
		if (index >= p_subset.vertex_count) {
			return ResError::malformed("index beyond the subset's vertices", index);
		}
	}
	return ResError();
}

} // namespace

int vertex_format_bytes(VertexFormat p_format) {
	switch (p_format) {
		case VertexFormat::None:
			return 0;
		case VertexFormat::Float:
			return 4;
		case VertexFormat::Float2:
			return 8;
		case VertexFormat::Float3:
			return 12;
		case VertexFormat::Float4:
			return 16;
		case VertexFormat::Half:
		case VertexFormat::UByteN:
			return 2;
		case VertexFormat::Half2:
			return 4;
		case VertexFormat::Half3:
			return 6;
		case VertexFormat::Half4:
			return 8;
		case VertexFormat::Byte4:
		case VertexFormat::Byte4N:
		case VertexFormat::UByte4:
		case VertexFormat::UByte4N:
			return 4;
		case VertexFormat::Short:
		case VertexFormat::ShortN:
			return 2;
		case VertexFormat::Short2:
		case VertexFormat::Short2N:
		case VertexFormat::UShort2:
		case VertexFormat::UShort2N:
			return 4;
		case VertexFormat::Short3:
		case VertexFormat::Short3N:
			return 6;
		case VertexFormat::Short4:
		case VertexFormat::Short4N:
		case VertexFormat::UShort4:
		case VertexFormat::UShort4N:
			return 8;
	}
	return -1;
}

bool vertex_format_decode(VertexFormat p_format, Bytes p_bytes, Vec4 &r_lanes) {
	r_lanes = { 0.0f, 0.0f, 0.0f, 0.0f };
	const int size = vertex_format_bytes(p_format);
	if (size < 0 || p_bytes.size() < size_t(size)) {
		return false;
	}
	const uint8_t *b = p_bytes.data();
	auto le16 = [b](int i) { return uint16_t(b[2 * i] | (b[2 * i + 1] << 8)); };
	switch (p_format) {
		case VertexFormat::None:
			break;
		case VertexFormat::Float:
		case VertexFormat::Float2:
		case VertexFormat::Float3:
		case VertexFormat::Float4:
			for (int i = 0; i < size / 4; i++) {
				const uint32_t bits = uint32_t(b[4 * i]) | (uint32_t(b[4 * i + 1]) << 8) | (uint32_t(b[4 * i + 2]) << 16) |
						(uint32_t(b[4 * i + 3]) << 24);
				r_lanes[i] = std::bit_cast<float>(bits);
			}
			break;
		case VertexFormat::Half:
		case VertexFormat::Half2:
		case VertexFormat::Half3:
		case VertexFormat::Half4:
			for (int i = 0; i < size / 2; i++) {
				r_lanes[i] = half_to_f32(le16(i));
			}
			break;
		case VertexFormat::Byte4:
			for (int i = 0; i < 4; i++) {
				r_lanes[i] = float(int8_t(b[i]));
			}
			break;
		case VertexFormat::Byte4N:
			for (int i = 0; i < 4; i++) {
				const float v = float(int8_t(b[i])) / 127.0f;
				r_lanes[i] = v < -1.0f ? -1.0f : v;
			}
			break;
		case VertexFormat::UByte4:
			for (int i = 0; i < 4; i++) {
				r_lanes[i] = float(b[i]);
			}
			break;
		case VertexFormat::UByte4N:
		case VertexFormat::UByteN: {
			const int lanes = p_format == VertexFormat::UByteN ? 1 : 4;
			for (int i = 0; i < lanes; i++) {
				r_lanes[i] = float(b[i]) / 255.0f;
			}
			break;
		}
		case VertexFormat::Short:
		case VertexFormat::Short2:
		case VertexFormat::Short3:
		case VertexFormat::Short4:
			for (int i = 0; i < size / 2; i++) {
				r_lanes[i] = float(int16_t(le16(i)));
			}
			break;
		case VertexFormat::ShortN:
		case VertexFormat::Short2N:
		case VertexFormat::Short3N:
		case VertexFormat::Short4N:
			for (int i = 0; i < size / 2; i++) {
				const float v = float(int16_t(le16(i))) / 32767.0f;
				r_lanes[i] = v < -1.0f ? -1.0f : v;
			}
			break;
		case VertexFormat::UShort2:
		case VertexFormat::UShort4:
			for (int i = 0; i < size / 2; i++) {
				r_lanes[i] = float(le16(i));
			}
			break;
		case VertexFormat::UShort2N:
		case VertexFormat::UShort4N:
			for (int i = 0; i < size / 2; i++) {
				r_lanes[i] = float(le16(i)) / 65535.0f;
			}
			break;
		default:
			return false;
	}
	return true;
}

const VertexElement *VertexDeclaration::find(VertexUsage p_usage) const {
	for (size_t i = 0; i < used_count(); i++) {
		if (elements[i].usage == p_usage) {
			return &elements[i];
		}
	}
	return nullptr;
}

bool MeshSubset::has_part(uint16_t p_part) const {
	const size_t word = p_part / 32;
	return word < part_mask.size() && ((part_mask[word] >> (p_part % 32)) & 1) != 0;
}

ResError MeshSet::parse(Bytes p_res, const ResMeta &p_res_meta, MeshSet &r_mesh) {
	r_mesh = MeshSet();
	Reader r(p_res);
	const uint64_t inline_base = uint64_t(p_res_meta[0]) | (uint64_t(p_res_meta[1]) << 8) | (uint64_t(p_res_meta[2]) << 16) |
			(uint64_t(p_res_meta[3]) << 24);
	const uint64_t inline_size = uint64_t(p_res_meta[4]) | (uint64_t(p_res_meta[5]) << 8) | (uint64_t(p_res_meta[6]) << 16) |
			(uint64_t(p_res_meta[7]) << 24);
	const uint32_t kind = r.u32(0x00);
	if (!r.ok()) {
		return r.error();
	}
	if (kind > 2) {
		return ResError::unsupported("mesh type", kind);
	}
	r_mesh.kind = MeshKind(kind);
	r_mesh.flags = r.u32(0x04);
	const uint32_t lod_count = r.u32(0x08);
	if (!r.ok()) {
		return r.error();
	}
	if (lod_count > kLodSlots) {
		return ResError::unsupported("lod count", lod_count);
	}
	r_mesh.bounds_min = r.vec3(0x10);
	r_mesh.bounds_max = r.vec3(0x20);
	r_mesh.name = r.optional_cstr(r.u64(0x58));
	r_mesh.short_name = r.optional_cstr(r.u64(0x60));
	r_mesh.name_hash = r.u32(0x68);
	if (!r.ok()) {
		return r.error();
	}
	uint64_t inline_cursor = inline_base;
	for (uint64_t slot = 0; slot < lod_count; slot++) {
		const uint64_t lod_at = r.u64(0x30 + 8 * slot);
		if (!r.ok()) {
			return r.error();
		}
		MeshLod lod;
		const ResError e = parse_lod(r, lod_at, r_mesh.kind, inline_cursor, lod);
		if (!e.ok()) {
			return e;
		}
		r_mesh.lods.push_back(std::move(lod));
	}
	if (inline_cursor != inline_base && inline_cursor - inline_base != inline_size) {
		return ResError::malformed("inline geometry size", inline_base);
	}
	return ResError();
}

ResError MeshLod::inline_geometry(Bytes p_res, Bytes &r_geometry) const {
	r_geometry = Bytes();
	if (!geometry.is_inline) {
		return ResError();
	}
	Reader r(p_res);
	r_geometry = r.slice(geometry.offset, geometry.len);
	return r.error();
}

ResError MeshLod::decode(Bytes p_geometry, SubsetFilter p_filter, std::vector<DecodedSubset> &r_out) const {
	r_out.clear();
	for (size_t index = 0; index < subsets.size(); index++) {
		if (p_filter == SubsetFilter::Drawn && subsets[index].is_z_only()) {
			continue;
		}
		DecodedSubset decoded;
		const ResError e = decode_subset(p_geometry, index, decoded);
		if (!e.ok()) {
			return e;
		}
		r_out.push_back(std::move(decoded));
	}
	return ResError();
}

ResError MeshLod::decode_subset(Bytes p_geometry, size_t p_index, DecodedSubset &r_out) const {
	r_out = DecodedSubset();
	if (p_index >= subsets.size()) {
		return ResError::malformed("subset index", p_index);
	}
	const MeshSubset &subset = subsets[p_index];
	Reader r(p_geometry);
	const Bytes vertex_region = r.slice(0, vertex_data_size);
	const Bytes index_region = r.slice(vertex_data_size, index_data_size);
	if (!r.ok()) {
		return r.error();
	}
	ResError e = decode_vertices(subset, vertex_region, r_out);
	if (!e.ok()) {
		return e;
	}
	r_out.subset = p_index;
	r_out.material_index = subset.material_index;
	r_out.category = subset.category;
	return decode_indices(subset, index_region, index_format, r_out.indices);
}

const SkeletonBone *MeshLod::resolve_bone(size_t p_subset, uint16_t p_slot) const {
	if (p_subset >= subsets.size() || p_slot >= subsets[p_subset].bone_slots.size()) {
		return nullptr;
	}
	const uint16_t bone = subsets[p_subset].bone_slots[p_slot];
	return bone < bones.size() ? &bones[bone] : nullptr;
}

} // namespace bf3::core::res
