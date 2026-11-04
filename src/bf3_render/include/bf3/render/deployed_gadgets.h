#pragma once

#include "bf3/core/cache/cached_convert.h"
#include "bf3/core/cache/disk_cache.h"
#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/deployed_object.h"
#include "bf3/core/result.h"
#include "bf3/core/superbundle_mount.h"
#include "bf3/render/material_lookup.h"
#include "bf3/render/mesh_prep.h"
#include "bf3/render/texture_prep.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace bf3::render {

struct DeployedGadgetSpec {
	uint16_t def = 0xFFFF;
	std::string firing_data;
};

struct DeployedGadgetPart {
	uint16_t def = 0xFFFF;
	core::gameplay::DeployedObject object;
	MeshMaterials materials;
	PreparedMesh prepared;
	core::CachedItem cache;
};

struct DeployedGadgetSet {
	std::vector<DeployedGadgetPart> parts;
	std::vector<PreparedTexture> textures;
	std::vector<std::string> notes;
	double seconds = 0.0;

	const DeployedGadgetPart *part(uint16_t p_def) const;
	const PreparedTexture *texture(const std::string &p_name) const;
};

core::Result<std::unique_ptr<DeployedGadgetSet>> load_deployed_gadgets(const core::EbxResolver &p_db,
		const core::LevelMount &p_mount, std::span<const DeployedGadgetSpec> p_specs, core::Cache *p_cache = nullptr);

}
