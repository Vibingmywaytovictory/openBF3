#pragma once

#include "bf3/core/res/mesh_set.h"
#include "bf3/core/res/res_loader.h"
#include "bf3/core/result.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace bf3::render {

struct PreparedSurface {
	size_t subset = 0;
	uint32_t material_index = 0;
	std::string material_name;
	std::optional<core::res::SubsetCategory> category;
	std::vector<float> positions;
	std::vector<float> normals;
	std::vector<float> tangents;
	std::vector<float> uv0;
	std::vector<float> uv1;
	std::vector<uint8_t> colors;
	std::vector<int32_t> bones;
	std::vector<float> weights;
	std::vector<uint16_t> parts;
	std::vector<int32_t> indices;
	std::array<float, 3> aabb_min{};
	std::array<float, 3> aabb_max{};

	size_t vertex_count() const { return positions.size() / 3; }
};

struct PreparedMesh {
	std::string name;
	bool skinned = false;
	std::vector<PreparedSurface> surfaces;
	size_t vertices = 0;
	size_t triangles = 0;
};

core::Result<PreparedMesh> prepare_mesh(const core::res::LoadedMeshSet &p_mesh, size_t p_lod, core::res::SubsetFilter p_filter,
		const std::vector<std::vector<int32_t>> *p_lane_binds);

}
