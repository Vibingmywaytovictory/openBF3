#pragma once

#include "bf3/core/cache/cached_convert.h"
#include "bf3/core/cache/disk_cache.h"
#include "bf3/core/ebx_db.h"
#include "bf3/core/superbundle_mount.h"
#include "bf3/render/look_materials.h"
#include "bf3/render/material_lookup.h"
#include "bf3/render/mesh_prep.h"
#include "bf3/render/texture_prep.h"

#include <memory>
#include <span>
#include <string>
#include <vector>

namespace bf3::render {

struct EffectMesh {
	std::string name;
	MeshMaterials materials;
	std::vector<LookMaterial> looks;
	PreparedMesh prepared;
	core::CachedItem cache;
};

struct EffectMeshSet {
	std::vector<EffectMesh> meshes;
	std::vector<PreparedTexture> textures;
	std::vector<std::string> notes;
	size_t failed = 0;
	size_t shader_textures = 0;
	double seconds = 0.0;

	const EffectMesh *mesh(std::string_view p_name) const;
	const PreparedTexture *texture(const std::string &p_name) const;
};

std::unique_ptr<EffectMeshSet> load_effect_meshes(const core::EbxResolver &p_db, const core::LevelMount &p_mount,
		std::span<const std::string> p_names, core::Cache *p_cache = nullptr);

}
