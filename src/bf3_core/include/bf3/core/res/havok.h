#pragma once
// HavokPhysicsData (0x91043F65) and HavokDestructionPhysicsData (0x4864737B) res, handoff
// 02-resources §4 and §16; port of IW4L havok/{mod,geometry,hull}.rs.
//
// resMeta = [packfileOffset, packfile32Size, packfile64Size, tailSize]. The res holds per-part
// tables, then the 32-bit Havok packfile (hk_2010.2.0-r1) whose pointer slots are all 0: pointers
// come from the external fixup block in the tail. One root shape per PhysicsEntityData.RigidBodies
// entry, same order. Destruction files expose the intact shapes the same way.
//
// Shape walk: MoppBvTree -> child (the MOPP code is ignored) | List -> children |
// ConvexTranslate / ConvexTransform -> child | ExtendedMesh -> ShapesSubparts (children at their
// QT transform) + TrianglesSubparts (storage triangles) | CompressedMesh -> chunk strips + list +
// big triangles | Box / Sphere / Capsule / Cylinder / ConvexVertices = convex leaves, whose
// `radius` is the convex skin (the collision surface lies that far outside the core).

#include "bf3/core/res/res_math.h"
#include "bf3/core/res/res_types.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace bf3::core::res {

struct PackObject {
	uint32_t offset = 0; // in the data section
	std::string class_name;
};

// The 32-bit copy of the packfile with its pointers resolved.
class Packfile {
public:
	std::string version;
	std::vector<uint8_t> data; // the data section up to its object table
	std::vector<PackObject> objects; // every serialized object in data order
	uint32_t contents_offset = 0;

	static ResError parse(Bytes p_bytes, Bytes p_fixups, Packfile &r_packfile);

	// Class of the object starting at p_offset, or nullptr.
	const std::string *class_at(uint32_t p_offset) const;
	// The object a pointer slot points at; false for a null slot.
	bool pointer(uint32_t p_slot, uint32_t &r_target) const;
	// (data offset, element count) of the hkArray at p_field; (0, 0) when empty.
	ResError array(uint32_t p_field, uint32_t &r_data, uint32_t &r_count) const;
	// A char* member; false when null or unreadable.
	bool string(uint32_t p_slot, std::string &r_text) const;
	// Root shape offsets of the HavokPhysicsContainer, one per rigid body.
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
	uint32_t part = 0; // physics part
	uint16_t material = 0; // index into material_flags_and_indices
	int32_t transform_index = -1; // -1 = none
};

struct CompressedChunk {
	Vec3 offset{};
	std::vector<std::array<uint16_t, 3>> vertices; // quantized: offset + v * error
	std::vector<uint16_t> indices; // strips first (one per strip_lengths entry), then a list
	std::vector<uint16_t> strip_lengths;
	uint32_t part = 0;
	uint32_t material = 0; // index into material_flags_and_indices
	int32_t reference = -1; // chunk whose geometry this one reuses, -1 = none
	int32_t transform_index = -1;
};

struct CompressedMesh {
	float error = 0.0f; // quantization step of chunk vertices
	Vec3 bounds_min{};
	Vec3 bounds_max{};
	std::vector<Vec3> big_vertices;
	std::vector<BigTriangle> big_triangles;
	std::vector<CompressedChunk> chunks;
	std::vector<Transform> transforms; // referenced by transform_index
	uint32_t convex_piece_count = 0;
};

struct Shape;

struct ShapesSubpart {
	Vec4 rotation{}; // quaternion (x, y, z, w)
	Vec3 translation{};
	uint32_t user_data = 0; // static model groups: the first physics part of the instance
	std::vector<Shape> children;

	Transform transform() const { return Transform::from_rotation_translation(rotation, translation); }
};

struct TrianglesSubpart {
	Transform transform;
	std::vector<Vec3> vertices; // empty when the packfile does not carry the vertex data
	std::vector<std::array<uint32_t, 3>> triangles;
	std::vector<uint32_t> materials; // per triangle when the storage carries it
	uint32_t user_data = 0;
};

struct ExtendedMesh {
	std::vector<TrianglesSubpart> triangle_subparts;
	std::vector<ShapesSubpart> shape_subparts;
};

struct Shape {
	ShapeType type = ShapeType::Unsupported;
	// MoppBvTree, ConvexTranslate, ConvexTransform: one child. List: the children.
	std::vector<Shape> children;
	std::vector<uint32_t> collision_filter_infos; // List
	Vec3 translation{}; // ConvexTranslate
	Transform transform; // ConvexTransform
	Vec3 half_extents{}; // Box
	Vec3 a{}; // Capsule, Cylinder
	Vec3 b{};
	float radius = 0.0f; // convex skin; Sphere / Capsule: the radius itself
	float cylinder_radius = 0.0f; // Cylinder
	std::vector<Vec3> points; // ConvexVertices
	std::vector<Vec4> planes; // ConvexVertices: n.p + w = 0, n out, as stored
	uint32_t user_data = 0; // convex leaves
	std::unique_ptr<CompressedMesh> compressed;
	std::unique_ptr<ExtendedMesh> extended;
	std::string class_name; // Unsupported
};

// A triangle in the space the walk's transform maps to. Convex primitives wind counter-clockwise
// seen from outside; mesh triangles keep their stored winding.
struct Tri {
	std::array<Vec3, 3> v{};
	uint32_t material = 0; // mesh triangles: index into material_flags_and_indices
	uint32_t part = 0; // mesh triangles: physics part; convex leaves in a group subpart: its user_data
	uint32_t user_data = 0; // convex leaves: the leaf's user_data
};

// A convex primitive in parent space: planes (n out) bound the hull of points, the core; the
// collision surface lies `radius` further out.
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
	std::vector<std::array<Vec3, 2>> local_aabbs; // (min, max) per part
	std::vector<uint8_t> material_indices;
	std::vector<uint32_t> material_flags_and_indices; // u32 (Rime reads u16: wrong)
	float scale = 0.0f;
	uint8_t material_count_used = 0;
	uint8_t highest_material_index = 0;
	Packfile packfile;

	static ResError parse(Bytes p_res, const ResMeta &p_res_meta, HavokPhysicsData &r_data);

	// One root shape per rigid body, in PhysicsEntityData.RigidBodies order.
	ResError shapes(std::vector<Shape> &r_shapes) const;
	// Pairs of parts joined in the first hkdBreakableShapeConnectionList; empty when none.
	ResError part_connections(std::vector<std::array<uint32_t, 2>> &r_pairs) const;
	// Instance transforms of a static model group, in subpart order; empty when none.
	ResError group_subparts(std::vector<SubpartTransform> &r_subparts) const;
};

// Every triangle of p_shape in the space p_transform maps to; convex primitives are tessellated
// (boxes 12 triangles, round shapes low-poly) without their convex radius.
std::vector<Tri> havok_triangles(const Shape &p_shape, const Transform &p_transform);
// The convex primitives of p_shape in the space p_transform maps to; mesh shapes add nothing.
std::vector<ConvexPiece> havok_convex_pieces(const Shape &p_shape, const Transform &p_transform);

} // namespace bf3::core::res
