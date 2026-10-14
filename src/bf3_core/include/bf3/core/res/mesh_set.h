#pragma once
// MeshSet res (resType 0x49B156D4), handoff 02-resources §2; port of IW4L mesh.rs.
//
// Inputs: the res payload and its resMeta = [inlineGeometryOffset, inlineGeometrySize,
// relocTableSize, 0x00940070]. A LOD's geometry is `vertices || indices || pad to 16`, either in the
// chunk named by MeshLod::geometry.chunk (all-zero id = inline) or inline in the res.
//
// Skinned meshes (M9 integration notes; measured on every skinned MeshSet of MP_Subway and
// XP2_Skybar: 561 meshes, 1,679 LODs, see README "Resource decoders"):
// * Vertex bone lane -> subset.bone_slots[lane] = *skeleton* bone index (skeleton_bone()); every
//   weighted lane is < bone_slots.size() and every slot value appears in MeshLod::bones.
// * MeshLod::bones is the LOD's table {skeleton index, name hash} (u32 arrays at MeshLayout +0x98
//   and +0xA0); name_hash = hash_quick(lowercase(bone name)), e.g. "Wep_Root", "LeftHand".
//   find_bone() maps a skeleton index to its entry.
// * BoneIndices are UByte4, BoneWeights UByte4N (weights sum to 1); bones_per_vertex is 1, 2 or 4.
//   Composite meshes reuse BoneIndices byte 0 as the part index instead.
// * Skinning is not applied by the decoder: positions are the bind pose in mesh space.

#include "bf3/core/res/res_math.h"
#include "bf3/core/res/res_types.h"

#include <optional>
#include <string>
#include <vector>

namespace bf3::core::res {

enum class MeshKind : uint8_t {
	Rigid = 0,
	Skinned = 1,
	Composite = 2,
};

enum class IndexFormat : uint8_t {
	U16,
	U32,
};
inline int index_format_bytes(IndexFormat p_format) {
	return p_format == IndexFormat::U16 ? 2 : 4;
}

enum class SubsetCategory : uint8_t {
	Opaque = 0,
	Transparent = 1,
	TransparentDecal = 2,
	ZOnly = 3, // depth and shadow proxies; never drawn in colour
};

enum class SubsetFilter : uint8_t {
	All,
	Drawn, // every subset except the ZOnly ones
};

// Raw ids (any u8 value is representable; unnamed values are "other").
enum class PrimitiveType : uint8_t {
	PointList = 0,
	LineList = 1,
	LineStrip = 2,
	TriangleList = 3,
	TriangleStrip = 5,
};

enum class VertexUsage : uint8_t {
	Position = 1,
	BoneIndices = 2,
	BoneIndices2 = 3,
	BoneWeights = 4,
	BoneWeights2 = 5,
	Normal = 6,
	Tangent = 7,
	Binormal = 8,
	BinormalSign = 9,
	Color0 = 30,
	Color1 = 31,
	TexCoord0 = 33, // .. TexCoord7 = 40
	RadiosityTexCoord = 41, // Enlighten lightmap chart UV
	SubMaterialIndex = 50,
};
inline VertexUsage tex_coord_usage(int p_set) {
	return VertexUsage(33 + p_set);
}

enum class VertexFormat : uint8_t {
	None = 0,
	Float = 1,
	Float2 = 2,
	Float3 = 3,
	Float4 = 4,
	Half = 5,
	Half2 = 6,
	Half3 = 7,
	Half4 = 8,
	Byte4 = 10,
	Byte4N = 11,
	UByte4 = 12,
	UByte4N = 13,
	Short = 14,
	Short2 = 15,
	Short3 = 16,
	Short4 = 17,
	ShortN = 18,
	Short2N = 19,
	Short3N = 20,
	Short4N = 21,
	UShort2 = 22,
	UShort4 = 23,
	UShort2N = 24,
	UShort4N = 25,
	UByteN = 50,
};
// Bytes of one element, or -1 for an unknown format id.
int vertex_format_bytes(VertexFormat p_format);
// Components as floats: halves and floats as stored, N formats normalized, integer formats as
// their integer value; missing lanes are 0. False for an unknown format or too few bytes.
bool vertex_format_decode(VertexFormat p_format, Bytes p_bytes, Vec4 &r_lanes);

struct VertexElement {
	VertexUsage usage = VertexUsage(0);
	VertexFormat format = VertexFormat::None;
	uint8_t offset = 0xFF;
	uint8_t stream = 0;
};

struct VertexDeclaration {
	std::array<VertexElement, 16> elements{}; // only the first element_count are meaningful
	uint8_t element_count = 0;
	std::array<uint8_t, 4> stream_strides{};
	uint8_t stream_count = 0;

	size_t used_count() const { return element_count < 16 ? element_count : 16; }
	// First used element with this usage, or nullptr.
	const VertexElement *find(VertexUsage p_usage) const;
};

// Composite meshes: bounds in mesh space, as are the part's vertices; the transform is not needed
// to place them (identity in the data).
struct MeshPart {
	Vec3 bounds_min{};
	Vec3 bounds_max{};
	Transform transform;
};

struct SkeletonBone {
	uint32_t index = 0; // skeleton bone index
	uint32_t name_hash = 0;
};

struct MeshSubset {
	std::string material_name;
	uint32_t material_index = 0; // into MeshAsset.Materials
	PrimitiveType primitive_type = PrimitiveType::TriangleList;
	uint32_t primitive_count = 0;
	uint32_t start_index = 0; // first index, in index units, in the LOD's index region
	uint32_t vertex_offset = 0; // bytes into the LOD's vertex region
	uint32_t vertex_count = 0;
	uint8_t vertex_stride = 0; // authoritative (not the element sum)
	uint8_t bones_per_vertex = 0;
	// Skinned: vertex bone lane -> skeleton bone index. Composite: the parts this subset draws.
	std::vector<uint16_t> bone_slots;
	// Composite: bit p set when part p has geometry in this subset.
	std::array<uint32_t, 6> part_mask{};
	VertexDeclaration declaration;
	std::optional<SubsetCategory> category;
	std::array<float, 6> tex_coord_ratios{};

	bool is_z_only() const { return category == SubsetCategory::ZOnly; }
	bool has_part(uint16_t p_part) const;
};

struct GeometrySource {
	bool is_inline = false;
	ChunkId chunk; // !is_inline: `vertices || indices || pad16` in this chunk
	uint64_t offset = 0; // is_inline: the same bytes inside the res
	uint64_t len = 0;
};

// One decoded subset; vertex arrays have vertex_count entries, indices are a triangle list
// relative to the subset's first vertex (front faces counter-clockwise, right-handed).
struct DecodedSubset {
	size_t subset = 0;
	uint32_t material_index = 0;
	std::optional<SubsetCategory> category;
	std::vector<Vec3> positions;
	std::optional<std::vector<Vec3>> normals;
	std::optional<std::vector<Vec3>> tangents;
	std::optional<std::vector<float>> binormal_signs; // bitangent = cross(normal, tangent) * sign
	std::optional<std::vector<Vec2>> uv0; // Direct3D convention: v grows downward
	std::optional<std::vector<Vec2>> uv1;
	std::optional<std::vector<Vec2>> uv2;
	std::optional<std::vector<Vec2>> radiosity_uv;
	std::optional<std::vector<std::array<uint8_t, 4>>> colors; // R, G, B, A bytes
	std::optional<std::vector<uint16_t>> parts; // composite: the part of each vertex
	// Skinned/composite bone channels as their decoded lanes (extension over IW4L, which leaves
	// them undecoded): indices index bone_slots (see header comment), weights normalized.
	std::optional<std::vector<std::array<uint16_t, 4>>> bone_indices;
	std::optional<std::vector<std::array<uint16_t, 4>>> bone_indices2;
	std::optional<std::vector<Vec4>> bone_weights;
	std::optional<std::vector<Vec4>> bone_weights2;
	std::vector<uint32_t> indices;
};

struct MeshLod {
	std::string name;
	std::string short_name;
	uint32_t name_hash = 0;
	uint32_t flags = 0; // 0x1 base LOD, 0x40 streaming, 0x10 ignore; key on the chunk id instead
	IndexFormat index_format = IndexFormat::U16;
	uint32_t vertex_data_size = 0;
	uint32_t index_data_size = 0;
	GeometrySource geometry;
	std::vector<MeshSubset> subsets;
	std::array<std::vector<uint8_t>, 4> category_subsets; // subset indices per SubsetCategory
	std::vector<MeshPart> parts; // composite only
	std::vector<SkeletonBone> bones; // skinned only

	// The inline geometry inside p_res (empty span and ok for a chunk LOD).
	ResError inline_geometry(Bytes p_res, Bytes &r_geometry) const;
	// p_geometry: the decompressed chunk or inline_geometry().
	ResError decode(Bytes p_geometry, SubsetFilter p_filter, std::vector<DecodedSubset> &r_out) const;
	ResError decode_subset(Bytes p_geometry, size_t p_index, DecodedSubset &r_out) const;
	// Skinned: skeleton bone index of bone lane value p_lane in subset p_subset, or -1.
	int64_t skeleton_bone(size_t p_subset, uint16_t p_lane) const;
	// The bone table entry of a skeleton bone index, or nullptr.
	const SkeletonBone *find_bone(uint32_t p_skeleton_index) const;
};

struct MeshSet {
	MeshKind kind = MeshKind::Rigid;
	uint32_t flags = 0;
	std::string name;
	std::string short_name;
	uint32_t name_hash = 0; // hash_quick(lowercase asset name) == MeshAsset.NameHash
	Vec3 bounds_min{};
	Vec3 bounds_max{};
	std::vector<MeshLod> lods;

	static ResError parse(Bytes p_res, const ResMeta &p_res_meta, MeshSet &r_mesh);
};

} // namespace bf3::core::res
