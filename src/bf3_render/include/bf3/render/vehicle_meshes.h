#pragma once

#include "bf3/core/cache/cached_convert.h"
#include "bf3/core/cache/disk_cache.h"
#include "bf3/core/ebx_db.h"
#include "bf3/core/math.h"
#include "bf3/core/result.h"
#include "bf3/core/superbundle_mount.h"
#include "bf3/render/material_lookup.h"
#include "bf3/render/mesh_prep.h"
#include "bf3/render/texture_prep.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace bf3::render {

struct VehicleMeshPart {
	uint16_t part = 0;
	PreparedMesh prepared;
	core::Vec3 bounds_min{};
	core::Vec3 bounds_max{};
	bool component = false;
	core::Transform rest;

	core::Vec3 centre() const;
};

struct VehicleMeshSpec {
	std::string blueprint;
	std::string mesh;
	std::vector<std::pair<std::string, core::Transform>> component_meshes;
};

struct VehicleMeshKind {
	std::string blueprint;
	std::string mesh;
	std::vector<MeshMaterials> materials;
	std::vector<VehicleMeshPart> parts;
	std::vector<uint8_t> part_mesh;
	core::CachedItem cache;
};

struct VehicleMeshSet {
	std::vector<VehicleMeshKind> kinds;
	std::vector<PreparedTexture> textures;
	std::vector<std::string> notes;
	double seconds = 0.0;

	const VehicleMeshKind *kind(const std::string &p_blueprint) const;
	const PreparedTexture *texture(const std::string &p_name) const;
};

std::vector<VehicleMeshPart> split_mesh_parts(const PreparedMesh &p_mesh);

core::Result<std::unique_ptr<VehicleMeshSet>> load_vehicle_meshes(const core::EbxResolver &p_db,
		const core::LevelMount &p_mount, std::span<const VehicleMeshSpec> p_specs, core::Cache *p_cache = nullptr);

}
