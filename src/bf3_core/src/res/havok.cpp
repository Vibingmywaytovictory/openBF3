#include "bf3/core/res/havok.h"

#include "res_reader.h"

#include <cstring>

namespace bf3::core::res {

using detail::align16;
using detail::mul;
using detail::off;
using detail::Reader;

namespace {

constexpr uint8_t kMagic[8] = { 0x57, 0xE0, 0xE0, 0x57, 0x10, 0xC0, 0xC0, 0x10 };
constexpr uint64_t kSectionHeaderBytes = 48;
constexpr uint32_t kMaxShapeDepth = 64;

constexpr uint64_t kTreeBytesPerDataByte = 128;
constexpr uint64_t kTreeBytesFloor = uint64_t(64) << 20;
constexpr uint64_t kHullWorkPerDataByte = 64;
constexpr uint64_t kHullWorkFloor = uint64_t(1) << 25;

constexpr uint64_t kCapsulePoints = 2 * (3 * 12 + 1);
constexpr uint64_t kCylinderPoints = 2 * 12;
constexpr uint64_t kBoxPoints = 8;

constexpr uint64_t kGroupVisitsFloor = 4096;

bool slot_in_data(const Packfile &p_pack, uint32_t p_base, uint64_t p_stride, uint64_t p_index) {
	return uint64_t(p_base) + p_stride * p_index < p_pack.data.size();
}

int32_t le_i32(const uint8_t *p) {
	return int32_t(uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24));
}

uint64_t clamp0(int32_t p_value) {
	return p_value < 0 ? 0 : uint64_t(p_value);
}

int32_t opt16(uint16_t p_value) {
	return p_value == 0xFFFF ? -1 : int32_t(p_value);
}

class PackReader {
public:
	PackReader(const Packfile &p_pack) :
			pack(p_pack), r(Bytes(p_pack.data.data(), p_pack.data.size())) {
		tree_budget = kTreeBytesFloor + kTreeBytesPerDataByte * uint64_t(p_pack.data.size());
		hull_budget = kHullWorkFloor + kHullWorkPerDataByte * uint64_t(p_pack.data.size());
	}

	const Packfile &pack;
	Reader r;
	uint64_t tree_bytes = 0;
	uint64_t tree_budget = 0;
	uint64_t hull_work = 0;
	uint64_t hull_budget = 0;
	std::array<uint32_t, kMaxShapeDepth + 1> path{};

	bool ok() const { return r.ok(); }
	ResError error() const { return r.error(); }
	bool fail(const ResError &p_e) { return r.fail(p_e); }

	bool spend(uint64_t p_bytes, uint64_t p_at) {
		tree_bytes += p_bytes;
		return tree_bytes <= tree_budget || r.fail(ResError::malformed("shape tree size", p_at));
	}

	bool spend_hull(uint64_t p_points, uint64_t p_at) {
		hull_work += p_points * p_points;
		return hull_work <= hull_budget || r.fail(ResError::malformed("convex hull work", p_at));
	}
	bool array(uint32_t p_field, uint32_t &r_data, uint32_t &r_count) {
		if (!r.ok()) {
			return false;
		}
		const ResError e = pack.array(p_field, r_data, r_count);
		return e.ok() || r.fail(e);
	}
	bool u16_array(uint32_t p_field, std::vector<uint16_t> &r_out) {
		uint32_t data = 0, count = 0;
		if (!array(p_field, data, count) || !spend(2 * uint64_t(count), p_field)) {
			return false;
		}
		for (uint32_t i = 0; i < count; i++) {
			const uint16_t v = r.u16(uint32_t(data + 2 * i));
			if (!r.ok()) {
				return false;
			}
			r_out.push_back(v);
		}
		return true;
	}

	ResError shape_at(uint32_t p_at, uint32_t p_depth, Shape &r_shape);
	ResError child(uint32_t p_slot, uint32_t p_depth, Shape &r_shape) {
		uint32_t target = 0;
		if (!pack.pointer(p_slot, target)) {
			return ResError::malformed("null child shape", p_slot);
		}
		return shape_at(target, p_depth + 1, r_shape);
	}
	ResError compressed_mesh(uint32_t p_at, CompressedMesh &r_mesh);
	ResError extended_mesh(uint32_t p_at, bool p_storage, uint32_t p_depth, ExtendedMesh &r_mesh);
	ResError fill_storage(uint32_t p_subpart, uint32_t p_mesh, TrianglesSubpart &r_out);
};

ResError PackReader::shape_at(uint32_t p_at, uint32_t p_depth, Shape &r_shape) {
	if (p_depth > kMaxShapeDepth) {
		return ResError::malformed("shape nesting", p_at);
	}
	for (uint32_t d = 0; d < p_depth; d++) {
		if (path[d] == p_at) {
			return ResError::malformed("shape cycle", p_at);
		}
	}
	path[p_depth] = p_at;
	if (!spend(sizeof(Shape), p_at)) {
		return r.error();
	}
	const std::string *cls = pack.class_at(p_at);
	if (cls == nullptr) {
		return ResError::malformed("pointer to no object", p_at);
	}
	const std::string &c = *cls;
	Shape &s = r_shape;
	const uint32_t user_data = r.u32(uint64_t(p_at) + 8);
	if (!r.ok()) {
		return r.error();
	}
	ResError e;
	if (c == "hkpMoppBvTreeShape") {
		s.type = ShapeType::MoppBvTree;
		s.children.emplace_back();
		e = child(p_at + 68, p_depth, s.children.back());
	} else if (c == "hkpListShape") {
		s.type = ShapeType::List;
		uint32_t data = 0, count = 0;
		if (!array(p_at + 24, data, count) || !spend(4 * uint64_t(count), p_at)) {
			return r.error();
		}
		for (uint32_t i = 0; i < count; i++) {
			s.children.emplace_back();
			e = child(data + 16 * i, p_depth, s.children.back());
			if (!e.ok()) {
				return e;
			}
			s.collision_filter_infos.push_back(r.u32(uint32_t(data + 16 * i + 4)));
			if (!r.ok()) {
				return r.error();
			}
		}
	} else if (c == "hkpConvexTranslateShape") {
		s.type = ShapeType::ConvexTranslate;
		s.translation = r.vec3(uint32_t(p_at + 32));
		if (!r.ok()) {
			return r.error();
		}
		s.children.emplace_back();
		e = child(p_at + 24, p_depth, s.children.back());
	} else if (c == "hkpConvexTransformShape") {
		s.type = ShapeType::ConvexTransform;
		s.transform.right = r.vec3(uint32_t(p_at + 32));
		s.transform.up = r.vec3(uint32_t(p_at + 48));
		s.transform.forward = r.vec3(uint32_t(p_at + 64));
		s.transform.trans = r.vec3(uint32_t(p_at + 80));
		if (!r.ok()) {
			return r.error();
		}
		s.children.emplace_back();
		e = child(p_at + 24, p_depth, s.children.back());
	} else if (c == "hkpBoxShape") {
		s.type = ShapeType::Box;
		s.half_extents = r.vec3(uint32_t(p_at + 32));
		s.radius = r.f32(uint32_t(p_at + 16));
		s.user_data = user_data;
		spend_hull(kBoxPoints, p_at);
	} else if (c == "hkpSphereShape") {
		s.type = ShapeType::Sphere;
		s.radius = r.f32(uint32_t(p_at + 16));
		s.user_data = user_data;
		spend_hull(kCapsulePoints, p_at);
	} else if (c == "hkpCapsuleShape") {
		s.type = ShapeType::Capsule;
		s.a = r.vec3(uint32_t(p_at + 32));
		s.b = r.vec3(uint32_t(p_at + 48));
		s.radius = r.f32(uint32_t(p_at + 16));
		s.user_data = user_data;
		spend_hull(kCapsulePoints, p_at);
	} else if (c == "hkpCylinderShape") {
		s.type = ShapeType::Cylinder;
		s.a = r.vec3(uint32_t(p_at + 32));
		s.b = r.vec3(uint32_t(p_at + 48));
		s.cylinder_radius = r.f32(uint32_t(p_at + 20));
		s.radius = r.f32(uint32_t(p_at + 16));
		s.user_data = user_data;
		spend_hull(kCylinderPoints, p_at);
	} else if (c == "hkpConvexVerticesShape") {
		s.type = ShapeType::ConvexVertices;
		uint32_t rotated = 0, blocks = 0;
		if (!array(p_at + 64, rotated, blocks)) {
			return r.error();
		}
		const int32_t raw_count = r.i32(uint64_t(p_at) + 76);
		if (!r.ok()) {
			return r.error();
		}
		const uint32_t count = raw_count < 0 ? 0u : uint32_t(raw_count);
		if (count > uint32_t(4 * blocks)) {
			return ResError::malformed("convex vertex count", uint64_t(p_at) + 76);
		}
		if (!spend(12 * uint64_t(count), p_at) || !spend_hull(count, p_at)) {
			return r.error();
		}
		for (uint32_t i = 0; i < count; i++) {
			const uint32_t block = rotated + 48 * (i / 4);
			const uint32_t lane = 4 * (i % 4);
			const Vec3 p{ r.f32(uint32_t(block + lane)), r.f32(uint32_t(block + 16 + lane)), r.f32(uint32_t(block + 32 + lane)) };
			if (!r.ok()) {
				return r.error();
			}
			s.points.push_back(p);
		}
		uint32_t planes_at = 0, plane_count = 0;
		if (!array(p_at + 80, planes_at, plane_count) || !spend(16 * uint64_t(plane_count), p_at)) {
			return r.error();
		}
		for (uint32_t i = 0; i < plane_count; i++) {
			const Vec4 plane = r.vec4(uint32_t(planes_at + 16 * i));
			if (!r.ok()) {
				return r.error();
			}
			s.planes.push_back(plane);
		}
		s.radius = r.f32(uint32_t(p_at + 16));
		s.user_data = user_data;
	} else if (c == "hkpCompressedMeshShape") {
		s.type = ShapeType::CompressedMesh;
		s.compressed = std::make_unique<CompressedMesh>();
		e = compressed_mesh(p_at, *s.compressed);
	} else if (c == "hkpExtendedMeshShape" || c == "hkpStorageExtendedMeshShape") {
		s.type = ShapeType::ExtendedMesh;
		s.extended = std::make_unique<ExtendedMesh>();
		e = extended_mesh(p_at, c == "hkpStorageExtendedMeshShape", p_depth, *s.extended);
	} else {
		s.type = ShapeType::Unsupported;
		s.class_name = c;
	}
	if (!e.ok()) {
		return e;
	}
	return r.error();
}

ResError PackReader::compressed_mesh(uint32_t p_at, CompressedMesh &r_mesh) {
	uint32_t transforms_at = 0, transform_count = 0;
	if (!spend(sizeof(CompressedMesh), p_at) || !array(p_at + 84, transforms_at, transform_count) ||
			!spend(sizeof(Transform) * uint64_t(transform_count), p_at)) {
		return r.error();
	}
	for (uint32_t i = 0; i < transform_count; i++) {
		const uint32_t t = transforms_at + 48 * i;
		const Vec3 translation = r.vec3(t);
		const Vec4 rotation = r.vec4(uint32_t(t + 16));
		const Vec3 scale = r.vec3(uint32_t(t + 32));
		if (!r.ok()) {
			return r.error();
		}
		r_mesh.transforms.push_back(Transform::from_translation_rotation_scale(translation, rotation, scale));
	}
	uint32_t vertices_at = 0, vertex_count = 0;
	if (!array(p_at + 96, vertices_at, vertex_count) || !spend(12 * uint64_t(vertex_count), p_at)) {
		return r.error();
	}
	for (uint32_t i = 0; i < vertex_count; i++) {
		const Vec3 v = r.vec3(uint32_t(vertices_at + 16 * i));
		if (!r.ok()) {
			return r.error();
		}
		r_mesh.big_vertices.push_back(v);
	}
	uint32_t triangles_at = 0, triangle_count = 0;
	if (!array(p_at + 108, triangles_at, triangle_count) || !spend(sizeof(BigTriangle) * uint64_t(triangle_count), p_at)) {
		return r.error();
	}
	for (uint32_t i = 0; i < triangle_count; i++) {
		const uint64_t t = uint32_t(triangles_at + 20 * i);
		const uint16_t transform = r.u16(t + 18);
		BigTriangle tri;
		tri.indices = { r.u16(t), r.u16(t + 2), r.u16(t + 4) };
		tri.part = r.u32(t + 8);
		tri.material = r.u16(t + 12);
		tri.transform_index = opt16(transform);
		if (!r.ok()) {
			return r.error();
		}
		r_mesh.big_triangles.push_back(tri);
	}
	uint32_t chunks_at = 0, chunk_count = 0;
	if (!array(p_at + 120, chunks_at, chunk_count)) {
		return r.error();
	}
	for (uint32_t i = 0; i < chunk_count; i++) {
		const uint32_t c = chunks_at + 80 * i;
		std::vector<uint16_t> packed;
		if (!spend(sizeof(CompressedChunk), c) || !u16_array(c + 16, packed)) {
			return r.error();
		}
		if (packed.size() % 3 != 0) {
			return ResError::malformed("chunk vertex array", c);
		}
		const uint16_t reference = r.u16(uint64_t(c) + 72);
		const uint16_t transform = r.u16(uint64_t(c) + 74);
		CompressedChunk chunk;
		chunk.offset = r.vec3(c);
		if (!r.ok()) {
			return r.error();
		}
		chunk.vertices.reserve(packed.size() / 3);
		for (size_t k = 0; k + 2 < packed.size(); k += 3) {
			chunk.vertices.push_back({ packed[k], packed[k + 1], packed[k + 2] });
		}
		if (!u16_array(c + 28, chunk.indices) || !u16_array(c + 40, chunk.strip_lengths)) {
			return r.error();
		}
		chunk.part = r.u32(uint32_t(c + 64));
		chunk.material = r.u32(uint32_t(c + 68));
		chunk.reference = opt16(reference);
		chunk.transform_index = opt16(transform);
		if (!r.ok()) {
			return r.error();
		}
		r_mesh.chunks.push_back(std::move(chunk));
	}

	for (const CompressedChunk &chunk : r_mesh.chunks) {
		if (chunk.reference >= 0 && size_t(chunk.reference) < r_mesh.chunks.size()) {
			const CompressedChunk &g = r_mesh.chunks[size_t(chunk.reference)];
			if (!spend(6 * uint64_t(g.vertices.size()) + 2 * uint64_t(g.indices.size() + g.strip_lengths.size()), p_at)) {
				return r.error();
			}
		}
	}
	r_mesh.error = r.f32(uint32_t(p_at + 144));
	r_mesh.bounds_min = r.vec3(uint32_t(p_at + 160));
	r_mesh.bounds_max = r.vec3(uint32_t(p_at + 176));
	uint32_t pieces_at = 0;
	if (!array(p_at + 132, pieces_at, r_mesh.convex_piece_count)) {
		return r.error();
	}
	return r.error();
}

ResError PackReader::extended_mesh(uint32_t p_at, bool p_storage, uint32_t p_depth, ExtendedMesh &r_mesh) {
	uint32_t shapes_at = 0, shape_count = 0;
	if (!array(p_at + 196, shapes_at, shape_count)) {
		return r.error();
	}
	for (uint32_t i = 0; i < shape_count; i++) {
		const uint32_t subpart = shapes_at + 64 * i;
		uint32_t children_at = 0, child_count = 0;
		if (!array(subpart + 20, children_at, child_count) || !spend(sizeof(ShapesSubpart), subpart)) {
			return r.error();
		}
		ShapesSubpart out;
		for (uint32_t k = 0; k < child_count; k++) {
			out.children.emplace_back();
			const ResError e = child(children_at + 4 * k, p_depth, out.children.back());
			if (!e.ok()) {
				return e;
			}
		}
		out.rotation = r.vec4(uint32_t(subpart + 32));
		out.translation = r.vec3(uint32_t(subpart + 48));
		out.user_data = r.u32(uint32_t(subpart + 16));
		if (!r.ok()) {
			return r.error();
		}
		r_mesh.shape_subparts.push_back(std::move(out));
	}

	uint32_t storage_at = 0, storage_count = 0;
	if (p_storage && !array(p_at + 240, storage_at, storage_count)) {
		return r.error();
	}
	uint32_t triangles_at = 0, triangle_count = 0;
	if (!array(p_at + 184, triangles_at, triangle_count)) {
		return r.error();
	}
	for (uint32_t i = 0; i < triangle_count; i++) {
		const uint32_t subpart = triangles_at + 112 * i;
		if (!spend(sizeof(TrianglesSubpart), subpart)) {
			return r.error();
		}
		const Vec3 translation = r.vec3(uint32_t(subpart + 64));
		const Vec4 rotation = r.vec4(uint32_t(subpart + 80));
		const Vec3 scale = r.vec3(uint32_t(subpart + 96));
		if (!r.ok()) {
			return r.error();
		}
		TrianglesSubpart out;
		out.transform = Transform::from_translation_rotation_scale(translation, rotation, scale);
		out.user_data = r.u32(uint32_t(subpart + 16));
		if (!r.ok()) {
			return r.error();
		}
		uint32_t mesh = 0;
		if (i < storage_count && pack.pointer(storage_at + 4 * i, mesh)) {
			const ResError e = fill_storage(subpart, mesh, out);
			if (!e.ok()) {
				return e;
			}
		}
		r_mesh.triangle_subparts.push_back(std::move(out));
	}
	return ResError();
}

ResError PackReader::fill_storage(uint32_t p_subpart, uint32_t p_mesh, TrianglesSubpart &r_out) {
	const int32_t raw_triangles = r.i32(uint64_t(p_subpart) + 20);
	const uint32_t index_striding = r.u16(uint64_t(p_subpart) + 44);
	const uint8_t striding_type = r.u8(uint64_t(p_subpart) + 46);
	const bool flip_alternate = r.u8(uint64_t(p_subpart) + 47) != 0;
	if (!r.ok()) {
		return r.error();
	}
	const uint32_t triangle_count = raw_triangles < 0 ? 0u : uint32_t(raw_triangles);
	uint32_t vertices_at = 0, vertex_count = 0;
	if (!array(p_mesh + 8, vertices_at, vertex_count) || !spend(12 * uint64_t(vertex_count), p_mesh)) {
		return r.error();
	}
	for (uint32_t i = 0; i < vertex_count; i++) {
		const Vec3 v = r.vec3(uint32_t(vertices_at + 16 * i));
		if (!r.ok()) {
			return r.error();
		}
		r_out.vertices.push_back(v);
	}
	uint32_t field = 0, width = 0;
	switch (striding_type) {
		case 1:
			field = p_mesh + 20;
			width = 1;
			break;
		case 2:
			field = p_mesh + 32;
			width = 2;
			break;
		case 3:
			field = p_mesh + 44;
			width = 4;
			break;
		default:
			return ResError::unsupported("triangle index striding", striding_type);
	}
	uint32_t indices_at = 0, index_count = 0;
	if (!array(field, indices_at, index_count)) {
		return r.error();
	}
	auto read = [&](uint32_t p_element, uint32_t &r_value) -> bool {
		if (p_element >= index_count) {
			return r.fail(ResError::malformed("triangle index beyond storage", p_element));
		}
		const uint64_t at = uint32_t(indices_at + width * p_element);
		r_value = width == 1 ? r.u8(at) : (width == 2 ? r.u16(at) : r.u32(at));
		return r.ok();
	};
	const uint32_t stride_elements = index_striding / width;

	if (stride_elements == 0) {
		if (!r.has(indices_at, uint64_t(width) * index_count)) {
			return r.error();
		}
		if (triangle_count > index_count) {
			return ResError::malformed("triangle index beyond storage", index_count);
		}
	}
	for (uint32_t t = 0; t < triangle_count; t++) {
		const uint32_t first = t * stride_elements;
		std::array<uint32_t, 3> tri{};
		if (!read(first, tri[0]) || !read(first + 1, tri[1]) || !read(first + 2, tri[2])) {
			return r.error();
		}
		if (flip_alternate && t % 2 == 1) {
			std::swap(tri[1], tri[2]);
		}
		if (tri[0] >= vertex_count || tri[1] >= vertex_count || tri[2] >= vertex_count) {
			return ResError::malformed("triangle vertex beyond storage", t);
		}
		if (!spend(12, p_subpart)) {
			return r.error();
		}
		r_out.triangles.push_back(tri);
	}
	uint32_t materials_at = 0, material_count = 0;
	if (!array(p_mesh + 56, materials_at, material_count)) {
		return r.error();
	}
	if (material_count == triangle_count) {
		if (!spend(4 * uint64_t(material_count), p_mesh)) {
			return r.error();
		}
		for (uint32_t i = 0; i < material_count; i++) {
			const uint8_t m = r.u8(uint32_t(materials_at + i));
			if (!r.ok()) {
				return r.error();
			}
			r_out.materials.push_back(m);
		}
	}
	return ResError();
}

}

ResError Packfile::parse(Bytes p_bytes, Bytes p_fixups, Packfile &r_packfile) {
	r_packfile = Packfile();
	Reader r(p_bytes);
	const Bytes magic = r.slice(0, 8);
	if (!r.ok()) {
		return r.error();
	}
	if (std::memcmp(magic.data(), kMagic, 8) != 0) {
		return ResError::malformed("packfile magic", 0);
	}
	const uint8_t pointer_bytes = r.u8(16);
	if (!r.ok()) {
		return r.error();
	}
	if (pointer_bytes != 4) {
		return ResError::unsupported("packfile layout", pointer_bytes);
	}
	const uint8_t little_endian = r.u8(17);
	if (!r.ok()) {
		return r.error();
	}
	if (little_endian != 1) {
		return ResError::unsupported("packfile layout", pointer_bytes);
	}
	const uint64_t section_count = clamp0(r.i32(20));
	const int32_t contents_section = r.i32(24);
	const uint32_t contents_offset = r.u32(28);
	const Bytes version_field = r.slice(40, 16);
	if (!r.ok()) {
		return r.error();
	}
	{
		Reader vr(version_field);
		std::string version;
		r_packfile.version = vr.cstr(0, version) ? version : std::string();
	}

	bool have_names = false, have_data = false;
	std::array<int32_t, 7> classnames{}, data_section{};
	for (uint64_t index = 0; index < section_count; index++) {
		const uint64_t at = off(64, mul(kSectionHeaderBytes, index));
		const Bytes tag = r.slice(at, 19);
		std::array<int32_t, 7> fields{};
		for (uint64_t i = 0; i < 7; i++) {
			fields[i] = r.i32(off(at, 20 + 4 * i));
		}
		if (!r.ok()) {
			return r.error();
		}
		size_t tag_len = 0;
		while (tag_len < tag.size() && tag[tag_len] != 0) {
			tag_len++;
		}
		const std::string_view name(reinterpret_cast<const char *>(tag.data()), tag_len);
		if (name == "__classnames__") {
			classnames = fields;
			have_names = true;
		} else if (name == "__data__" && int64_t(index) == int64_t(contents_section)) {
			data_section = fields;
			have_data = true;
		}
	}
	if (!have_names) {
		return ResError::malformed("packfile without class names", 64);
	}
	if (!have_data) {
		return ResError::malformed("packfile without data section", 64);
	}

	const Bytes names = r.slice(clamp0(classnames[0]), clamp0(classnames[6]));
	if (!r.ok()) {
		return r.error();
	}
	std::unordered_map<uint32_t, std::string> class_names;
	{
		Reader nr(names);
		size_t at = 0;
		while (at + 5 < names.size() && names[at] != 0xFF) {
			std::string name;
			if (!nr.cstr(at + 5, name)) {
				return nr.error();
			}
			const size_t len = name.size();
			class_names[uint32_t(at + 5)] = std::move(name);
			at += 5 + len + 1;
		}
	}

	const uint64_t data_at = clamp0(data_section[0]);
	const uint64_t external_len = clamp0(data_section[1]);
	const uint64_t array_table_len = clamp0(data_section[2]);
	const uint64_t objects_end = clamp0(data_section[3]);
	const uint64_t table_end = clamp0(data_section[4]);
	const Bytes data = r.slice(data_at, objects_end);
	if (!r.ok()) {
		return r.error();
	}
	r_packfile.data.assign(data.begin(), data.end());
	if (table_end < objects_end) {
		return ResError::malformed("packfile object table", table_end);
	}
	const Bytes table = r.slice(off(data_at, objects_end), table_end - objects_end);
	if (!r.ok()) {
		return r.error();
	}
	for (size_t at = 0; at + 12 <= table.size(); at += 12) {
		const int32_t object_offset = le_i32(table.data() + at);
		if (object_offset < 0) {
			break;
		}
		const uint32_t name = uint32_t(le_i32(table.data() + at + 8));
		PackObject object;
		object.offset = uint32_t(object_offset);
		const auto it = class_names.find(name);
		if (it != class_names.end()) {
			object.class_name = it->second;
		}
		r_packfile.objects.push_back(std::move(object));
	}

	if (external_len != p_fixups.size() || array_table_len > p_fixups.size()) {
		return ResError::malformed("external fixup size", external_len);
	}
	for (size_t at = 0; at + 8 <= array_table_len; at += 8) {
		const int32_t src = le_i32(p_fixups.data() + at);
		if (src < 0) {
			break;
		}
		r_packfile.array_fixups_[uint32_t(src)] = uint32_t(le_i32(p_fixups.data() + at + 4));
	}
	for (size_t at = size_t(array_table_len); at + 12 <= p_fixups.size(); at += 12) {
		const int32_t src = le_i32(p_fixups.data() + at);
		if (src < 0) {
			break;
		}
		r_packfile.object_fixups_[uint32_t(src)] = uint32_t(le_i32(p_fixups.data() + at + 8));
	}
	r_packfile.contents_offset = contents_offset;
	for (size_t index = 0; index < r_packfile.objects.size(); index++) {
		r_packfile.object_index_[r_packfile.objects[index].offset] = index;
	}
	return ResError();
}

const std::string *Packfile::class_at(uint32_t p_offset) const {
	const auto it = object_index_.find(p_offset);
	return it == object_index_.end() ? nullptr : &objects[it->second].class_name;
}

bool Packfile::pointer(uint32_t p_slot, uint32_t &r_target) const {
	const auto it = object_fixups_.find(p_slot);
	if (it == object_fixups_.end()) {
		return false;
	}
	r_target = it->second;
	return true;
}

ResError Packfile::array(uint32_t p_field, uint32_t &r_data, uint32_t &r_count) const {
	r_data = 0;
	r_count = 0;
	Reader r(Bytes(data.data(), data.size()));
	const int32_t count = r.i32(uint64_t(p_field) + 4);
	if (!r.ok()) {
		return r.error();
	}
	if (count <= 0) {
		return ResError();
	}
	const auto it = array_fixups_.find(p_field);
	if (it == array_fixups_.end()) {
		return ResError::malformed("array without data", p_field);
	}

	if (uint64_t(it->second) + uint64_t(count) > data.size()) {
		return ResError::malformed("array count beyond data", p_field);
	}
	r_data = it->second;
	r_count = uint32_t(count);
	return ResError();
}

bool Packfile::string(uint32_t p_slot, std::string &r_text) const {
	const auto it = array_fixups_.find(p_slot);
	if (it == array_fixups_.end()) {
		return false;
	}
	Reader r(Bytes(data.data(), data.size()));
	return r.cstr(it->second, r_text);
}

ResError Packfile::root_shapes(std::vector<uint32_t> &r_roots) const {
	r_roots.clear();
	const uint32_t root = contents_offset;
	uint32_t variants = 0, count = 0;
	ResError e = array(root, variants, count);
	if (!e.ok()) {
		return e;
	}
	bool found = false;
	uint32_t container = 0;
	for (uint32_t i = 0; i < count && !found && slot_in_data(*this, variants + 8, 12, i); i++) {
		uint32_t object = 0;
		if (pointer(variants + 12 * i + 8, object)) {
			const std::string *cls = class_at(object);
			if (cls != nullptr && *cls == "HavokPhysicsContainer") {
				container = object;
				found = true;
			}
		}
	}
	if (!found) {
		return ResError::malformed("packfile without a physics container", root);
	}
	uint32_t shapes = 0;
	e = array(container + 8, shapes, count);
	if (!e.ok()) {
		return e;
	}
	for (uint32_t i = 0; i < count; i++) {
		uint32_t shape = 0;
		if (!pointer(shapes + 4 * i, shape)) {
			return ResError::malformed("null root shape", uint32_t(shapes + 4 * i));
		}
		r_roots.push_back(shape);
	}
	return ResError();
}

ResError HavokPhysicsData::parse(Bytes p_res, const ResMeta &p_res_meta, HavokPhysicsData &r_data) {
	r_data = HavokPhysicsData();
	auto meta = [&](int i) -> uint64_t {
		return uint64_t(p_res_meta[4 * i]) | (uint64_t(p_res_meta[4 * i + 1]) << 8) | (uint64_t(p_res_meta[4 * i + 2]) << 16) |
				(uint64_t(p_res_meta[4 * i + 3]) << 24);
	};
	const uint64_t packfile_at = meta(0), packfile32_len = meta(1), packfile64_len = meta(2), tail_len = meta(3);
	Reader r(p_res);
	auto table = [&](uint64_t p_count_at, uint64_t p_pointer_at, uint64_t &r_count, uint64_t &r_pointer) {
		r_count = clamp0(r.i32(p_count_at));
		r_pointer = r.u32(p_pointer_at);
	};
	r_data.part_count = r.u32(0x00);
	uint64_t translations = 0, translations_at = 0, aabbs = 0, aabbs_at = 0, materials = 0, materials_at = 0, flags = 0, flags_at = 0;
	table(0x04, 0x08, translations, translations_at);
	table(0x10, 0x14, aabbs, aabbs_at);
	table(0x1C, 0x20, materials, materials_at);
	table(0x28, 0x2C, flags, flags_at);
	if (!r.ok()) {
		return r.error();
	}
	uint64_t scalars_at = 0;
	if (translations_at == 0x40) {
		scalars_at = 0x34;
	} else if (translations_at == 0x50) {
		uint64_t extra = 0, extra_at = 0;
		table(0x34, 0x38, extra, extra_at);
		if (!r.ok()) {
			return r.error();
		}
		if (extra != 0) {
			return ResError::unsupported("havok header table", uint32_t(extra));
		}
		scalars_at = 0x40;
	} else {
		return ResError::unsupported("havok header size", uint32_t(translations_at));
	}
	for (uint64_t i = 0; i < translations; i++) {
		const Vec3 v = r.vec3(off(translations_at, mul(16, i)));
		if (!r.ok()) {
			return r.error();
		}
		r_data.part_translations.push_back(v);
	}
	for (uint64_t i = 0; i < aabbs; i++) {
		const uint64_t at = off(aabbs_at, mul(32, i));
		const Vec3 lo = r.vec3(at);
		const Vec3 hi = r.vec3(off(at, 16));
		if (!r.ok()) {
			return r.error();
		}
		r_data.local_aabbs.push_back({ lo, hi });
	}
	const Bytes material_bytes = r.slice(materials_at, materials);
	if (!r.ok()) {
		return r.error();
	}
	r_data.material_indices.assign(material_bytes.begin(), material_bytes.end());
	for (uint64_t i = 0; i < flags; i++) {
		const uint32_t v = r.u32(off(flags_at, mul(4, i)));
		if (!r.ok()) {
			return r.error();
		}
		r_data.material_flags_and_indices.push_back(v);
	}

	const uint64_t expected_packfile_at = align16(translations_at + align16(16 * translations) + align16(32 * aabbs) +
			align16(materials) + align16(4 * flags));
	if (packfile_at != expected_packfile_at) {
		return ResError::malformed("packfile offset", packfile_at);
	}
	const uint64_t tail_at = packfile_at + packfile32_len + packfile64_len;
	if (tail_at + tail_len != p_res.size()) {
		return ResError::malformed("resource length", p_res.size());
	}
	const uint64_t fixups32_len = r.u32(tail_at);
	const Bytes fixups = r.slice(tail_at + 8, fixups32_len);
	const Bytes packfile = r.slice(packfile_at, packfile32_len);
	if (!r.ok()) {
		return r.error();
	}
	const ResError e = Packfile::parse(packfile, fixups, r_data.packfile);
	if (!e.ok()) {
		return e;
	}
	r_data.scale = r.f32(scalars_at);
	r_data.material_count_used = r.u8(scalars_at + 4);
	r_data.highest_material_index = r.u8(scalars_at + 5);
	return r.error();
}

ResError HavokPhysicsData::shapes(std::vector<Shape> &r_shapes) const {
	r_shapes.clear();
	std::vector<uint32_t> roots;
	const ResError e = packfile.root_shapes(roots);
	if (!e.ok()) {
		return e;
	}
	PackReader pr(packfile);
	for (uint32_t root : roots) {
		Shape shape;
		const ResError se = pr.shape_at(root, 0, shape);
		if (!se.ok()) {
			r_shapes.clear();
			return se;
		}
		r_shapes.push_back(std::move(shape));
	}
	return ResError();
}

ResError HavokPhysicsData::part_connections(std::vector<std::array<uint32_t, 2>> &r_pairs) const {
	r_pairs.clear();
	const PackObject *list = nullptr;
	for (const PackObject &object : packfile.objects) {
		if (object.class_name == "hkdBreakableShapeConnectionList") {
			list = &object;
			break;
		}
	}
	if (list == nullptr) {
		return ResError();
	}
	uint32_t entries = 0, count = 0;
	ResError e = packfile.array(list->offset + 8, entries, count);
	if (!e.ok()) {
		return e;
	}
	Reader r(Bytes(packfile.data.data(), packfile.data.size()));

	auto part = [&](uint32_t p_slot, bool &r_some, uint32_t &r_part) -> bool {
		r_some = false;
		uint32_t shape = 0;
		if (!packfile.pointer(p_slot, shape)) {
			return true;
		}
		const std::string *cls = packfile.class_at(shape);
		if (cls == nullptr || *cls != "hkdBreakableShape") {
			return true;
		}
		r_part = r.u32(uint32_t(shape + 92));
		r_some = r.ok();
		return r.ok();
	};
	for (uint32_t entry = 0; entry < count && slot_in_data(packfile, entries + 52, 64, entry); entry++) {
		const uint32_t at = entries + 64 * entry;
		bool some_a = false, some_b = false;
		uint32_t a = 0, b = 0;
		if (!part(at + 52, some_a, a) || !part(at + 56, some_b, b)) {
			r_pairs.clear();
			return r.error();
		}
		if (some_a && some_b) {
			r_pairs.push_back({ a, b });
		}
	}
	return ResError();
}

ResError HavokPhysicsData::group_subparts(std::vector<SubpartTransform> &r_subparts) const {
	r_subparts.clear();
	std::vector<uint32_t> roots;
	ResError e = packfile.root_shapes(roots);
	if (!e.ok()) {
		return e;
	}

	constexpr uint32_t kNoParent = UINT32_MAX;
	struct Visit {
		uint32_t offset = 0;
		uint32_t parent = kNoParent;
	};
	const uint64_t max_visits = kGroupVisitsFloor + uint64_t(packfile.data.size()) / 4;
	if (roots.size() > max_visits) {
		return ResError::malformed("shape tree size", packfile.contents_offset);
	}
	std::vector<Visit> visits;
	for (uint32_t root : roots) {
		visits.push_back({ root, kNoParent });
	}
	auto visit = [&](size_t p_parent, uint32_t p_child) -> ResError {
		for (uint32_t a = uint32_t(p_parent); a != kNoParent; a = visits[a].parent) {
			if (visits[a].offset == p_child) {
				return ResError::malformed("shape cycle", p_child);
			}
		}
		if (visits.size() >= max_visits) {
			return ResError::malformed("shape tree size", p_child);
		}
		visits.push_back({ p_child, uint32_t(p_parent) });
		return ResError();
	};
	Reader r(Bytes(packfile.data.data(), packfile.data.size()));
	size_t level_begin = 0;
	size_t level_end = visits.size();
	for (uint32_t depth = 0; level_begin < level_end; depth++) {
		if (depth > kMaxShapeDepth) {
			return ResError::malformed("shape nesting", visits[level_begin].offset);
		}
		for (size_t v = level_begin; v < level_end; v++) {
			const uint32_t offset = visits[v].offset;
			const std::string *cls = packfile.class_at(offset);
			if (cls == nullptr) {
				continue;
			}
			if (*cls == "hkpExtendedMeshShape" || *cls == "hkpStorageExtendedMeshShape") {
				uint32_t data = 0, count = 0;
				e = packfile.array(offset + 196, data, count);
				if (!e.ok()) {
					return e;
				}
				for (uint32_t i = 0; i < count; i++) {
					const uint32_t at = data + 64 * i;
					SubpartTransform s;
					s.rotation = r.vec4(uint32_t(at + 32));
					s.translation = r.vec3(uint32_t(at + 48));
					s.user_data = r.u32(uint32_t(at + 16));
					if (!r.ok()) {
						r_subparts.clear();
						return r.error();
					}
					r_subparts.push_back(s);
				}
				return ResError();
			}
			if (*cls == "hkpMoppBvTreeShape") {
				uint32_t child = 0;
				if (packfile.pointer(offset + 68, child)) {
					e = visit(v, child);
				}
			} else if (*cls == "hkpListShape") {
				uint32_t data = 0, count = 0;
				e = packfile.array(offset + 24, data, count);
				for (uint32_t i = 0; e.ok() && i < count && slot_in_data(packfile, data, 16, i); i++) {
					uint32_t child = 0;
					if (packfile.pointer(data + 16 * i, child)) {
						e = visit(v, child);
					}
				}
			}
			if (!e.ok()) {
				return e;
			}
		}
		level_begin = level_end;
		level_end = visits.size();
	}
	return ResError();
}

}
