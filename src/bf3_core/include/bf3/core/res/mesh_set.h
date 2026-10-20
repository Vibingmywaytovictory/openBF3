#pragma once

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
	ZOnly = 3,
};

enum class SubsetFilter : uint8_t {
	All,
	Drawn,
};

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
	TexCoord0 = 33,
	RadiosityTexCoord = 41,
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

int vertex_format_bytes(VertexFormat p_format);

bool vertex_format_decode(VertexFormat p_format, Bytes p_bytes, Vec4 &r_lanes);

struct VertexElement {
	VertexUsage usage = VertexUsage(0);
	VertexFormat format = VertexFormat::None;
	uint8_t offset = 0xFF;
	uint8_t stream = 0;
};

struct VertexDeclaration {
	std::array<VertexElement, 16> elements{};
	uint8_t element_count = 0;
	std::array<uint8_t, 4> stream_strides{};
	uint8_t stream_count = 0;

	size_t used_count() const { return element_count < 16 ? element_count : 16; }

	const VertexElement *find(VertexUsage p_usage) const;
};

struct MeshPart {
	Vec3 bounds_min{};
	Vec3 bounds_max{};
	Transform transform;
};

struct SkeletonBone {
	uint32_t index = 0;
	uint32_t name_hash = 0;
};

struct MeshSubset {
	std::string material_name;
	uint32_t material_index = 0;
	PrimitiveType primitive_type = PrimitiveType::TriangleList;
	uint32_t primitive_count = 0;
	uint32_t start_index = 0;
	uint32_t vertex_offset = 0;
	uint32_t vertex_count = 0;
	uint8_t vertex_stride = 0;
	uint8_t bones_per_vertex = 0;

	std::vector<uint16_t> bone_slots;

	std::array<uint32_t, 6> part_mask{};
	VertexDeclaration declaration;
	std::optional<SubsetCategory> category;
	std::array<float, 6> tex_coord_ratios{};

	bool is_z_only() const { return category == SubsetCategory::ZOnly; }
	bool has_part(uint16_t p_part) const;
};

struct GeometrySource {
	bool is_inline = false;
	ChunkId chunk;
	uint64_t offset = 0;
	uint64_t len = 0;
};

struct DecodedSubset {
	size_t subset = 0;
	uint32_t material_index = 0;
	std::optional<SubsetCategory> category;
	std::vector<Vec3> positions;
	std::optional<std::vector<Vec3>> normals;
	std::optional<std::vector<Vec3>> tangents;
	std::optional<std::vector<float>> binormal_signs;
	std::optional<std::vector<Vec2>> uv0;
	std::optional<std::vector<Vec2>> uv1;
	std::optional<std::vector<Vec2>> uv2;
	std::optional<std::vector<Vec2>> radiosity_uv;
	std::optional<std::vector<std::array<uint8_t, 4>>> colors;
	std::optional<std::vector<uint16_t>> parts;

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
	uint32_t flags = 0;
	IndexFormat index_format = IndexFormat::U16;
	uint32_t vertex_data_size = 0;
	uint32_t index_data_size = 0;
	GeometrySource geometry;
	std::vector<MeshSubset> subsets;
	std::array<std::vector<uint8_t>, 4> category_subsets;
	std::vector<MeshPart> parts;
	std::vector<SkeletonBone> bones;

	ResError inline_geometry(Bytes p_res, Bytes &r_geometry) const;

	ResError decode(Bytes p_geometry, SubsetFilter p_filter, std::vector<DecodedSubset> &r_out) const;
	ResError decode_subset(Bytes p_geometry, size_t p_index, DecodedSubset &r_out) const;

	int64_t skeleton_bone(size_t p_subset, uint16_t p_lane) const;

	const SkeletonBone *find_bone(uint32_t p_skeleton_index) const;
};

struct MeshSet {
	MeshKind kind = MeshKind::Rigid;
	uint32_t flags = 0;
	std::string name;
	std::string short_name;
	uint32_t name_hash = 0;
	Vec3 bounds_min{};
	Vec3 bounds_max{};
	std::vector<MeshLod> lods;

	static ResError parse(Bytes p_res, const ResMeta &p_res_meta, MeshSet &r_mesh);
};

}
