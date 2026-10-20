#pragma once

#include "bf3/core/res/res_math.h"
#include "bf3/core/res/res_types.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace bf3::core::res {

struct PackObject {
	uint32_t offset = 0;
	std::string class_name;
};

class Packfile {
public:
	std::string version;
	std::vector<uint8_t> data;
	std::vector<PackObject> objects;
	uint32_t contents_offset = 0;

	static ResError parse(Bytes p_bytes, Bytes p_fixups, Packfile &r_packfile);

	const std::string *class_at(uint32_t p_offset) const;

	bool pointer(uint32_t p_slot, uint32_t &r_target) const;

	ResError array(uint32_t p_field, uint32_t &r_data, uint32_t &r_count) const;

	bool string(uint32_t p_slot, std::string &r_text) const;

	ResError root_shapes(std::vector<uint32_t> &r_roots) const;

private:
	std::unordered_map<uint32_t, uint32_t> array_fixups_;
	std::unordered_map<uint32_t, uint32_t> object_fixups_;
	std::unordered_map<uint32_t, size_t> object_index_;
};

enum class ShapeType : uint8_t {
	MoppBvTree,
	List,
	ConvexTranslate,
	ConvexTransform,
	Box,
	Sphere,
	Capsule,
	Cylinder,
	ConvexVertices,
	CompressedMesh,
	ExtendedMesh,
	Unsupported,
};

struct BigTriangle {
	std::array<uint16_t, 3> indices{};
	uint32_t part = 0;
	uint16_t material = 0;
	int32_t transform_index = -1;
};

struct CompressedChunk {
	Vec3 offset{};
	std::vector<std::array<uint16_t, 3>> vertices;
	std::vector<uint16_t> indices;
	std::vector<uint16_t> strip_lengths;
	uint32_t part = 0;
	uint32_t material = 0;
	int32_t reference = -1;
	int32_t transform_index = -1;
};

struct CompressedMesh {
	float error = 0.0f;
	Vec3 bounds_min{};
	Vec3 bounds_max{};
	std::vector<Vec3> big_vertices;
	std::vector<BigTriangle> big_triangles;
	std::vector<CompressedChunk> chunks;
	std::vector<Transform> transforms;
	uint32_t convex_piece_count = 0;
};

struct Shape;

struct ShapesSubpart {
	Vec4 rotation{};
	Vec3 translation{};
	uint32_t user_data = 0;
	std::vector<Shape> children;

	Transform transform() const { return Transform::from_rotation_translation(rotation, translation); }
};

struct TrianglesSubpart {
	Transform transform;
	std::vector<Vec3> vertices;
	std::vector<std::array<uint32_t, 3>> triangles;
	std::vector<uint32_t> materials;
	uint32_t user_data = 0;
};

struct ExtendedMesh {
	std::vector<TrianglesSubpart> triangle_subparts;
	std::vector<ShapesSubpart> shape_subparts;
};

struct Shape {
	ShapeType type = ShapeType::Unsupported;

	std::vector<Shape> children;
	std::vector<uint32_t> collision_filter_infos;
	Vec3 translation{};
	Transform transform;
	Vec3 half_extents{};
	Vec3 a{};
	Vec3 b{};
	float radius = 0.0f;
	float cylinder_radius = 0.0f;
	std::vector<Vec3> points;
	std::vector<Vec4> planes;
	uint32_t user_data = 0;
	std::unique_ptr<CompressedMesh> compressed;
	std::unique_ptr<ExtendedMesh> extended;
	std::string class_name;
};

struct Tri {
	std::array<Vec3, 3> v{};
	uint32_t material = 0;
	uint32_t part = 0;
	uint32_t user_data = 0;
};

struct ConvexPiece {
	std::vector<Vec3> points;
	std::vector<Vec4> planes;
	float radius = 0.0f;
	uint32_t part = 0;
	uint32_t user_data = 0;
};

struct SubpartTransform {
	Vec4 rotation{};
	Vec3 translation{};
	uint32_t user_data = 0;

	Transform transform() const { return Transform::from_rotation_translation(rotation, translation); }
};

struct HavokPhysicsData {
	uint32_t part_count = 0;
	std::vector<Vec3> part_translations;
	std::vector<std::array<Vec3, 2>> local_aabbs;
	std::vector<uint8_t> material_indices;
	std::vector<uint32_t> material_flags_and_indices;
	float scale = 0.0f;
	uint8_t material_count_used = 0;
	uint8_t highest_material_index = 0;
	Packfile packfile;

	static ResError parse(Bytes p_res, const ResMeta &p_res_meta, HavokPhysicsData &r_data);

	ResError shapes(std::vector<Shape> &r_shapes) const;

	ResError part_connections(std::vector<std::array<uint32_t, 2>> &r_pairs) const;

	ResError group_subparts(std::vector<SubpartTransform> &r_subparts) const;
};

std::vector<Tri> havok_triangles(const Shape &p_shape, const Transform &p_transform);

std::vector<ConvexPiece> havok_convex_pieces(const Shape &p_shape, const Transform &p_transform);

}
