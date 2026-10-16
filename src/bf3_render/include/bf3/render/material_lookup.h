#pragma once
// Which shader and textures a MeshAsset's materials use (plan §2.5 "world drawing"). Godot-free.
//
// A MeshAsset (RigidMeshAsset / SkinnedMeshAsset ...) lists its MeshMaterials in Materials[]; a MeshSet subset's
// material_index points into that list. A MeshMaterial carries Shader (SurfaceShaderInstanceDataStruct: the
// ShaderGraph reference, VectorParameters, usually empty TextureParameters). The textures a mesh is drawn with
// live in the level's MeshVariationDatabase partitions (levels/<level>/<sublevel>/meshvariationdb_win32,
// weapon_shaderstateassets/<meshes>/meshvariationdb_win32): an entry names the mesh (Mesh) and a variation
// (VariationAssetNameHash = the ObjectVariation's NameHash, 0 = the mesh's default look); its Materials list
// one MeshVariationDatabaseMaterial per MeshMaterial (Material reference) with TextureParameters
// {ParameterName, Value = TextureAsset}. A TextureAsset partition's name is the DxTexture res name.
// The same structure is read by IW4L level_bf3 (geometry/materials.rs, the user's Apache-2.0 code); this is a
// minimal port of its "slots" step: the variation material that names the same MeshMaterial, else the one at
// the same index.

#include "bf3/core/ebx_db.h"
#include "bf3/core/result.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bf3::render {

struct MaterialSlot {
	std::string shader; // ShaderGraph partition name, e.g. "weapons/shaders/weaponpresetfp"
	std::vector<std::pair<std::string, std::string>> textures; // ParameterName -> texture res name
	std::vector<std::pair<std::string, std::array<float, 4>>> vectors; // VectorParameters of the MeshMaterial
	bool from_variation_db = false; // textures came from a MeshVariationDatabase entry
	// L1 look: the ShaderGraph of the entry's MeshMaterialVariation (MeshVariationDatabaseMaterial.MaterialVariation),
	// "" when the entry names none or it has no shader; the look resolver tries it before `shader`.
	std::string variation_shader;

	// Texture res name of a parameter (case-insensitive), or "".
	std::string texture(std::string_view p_parameter) const;
	// Vector parameter (case-insensitive), or nullptr.
	const std::array<float, 4> *vector(std::string_view p_parameter) const;
};

struct MeshMaterials {
	std::string mesh; // partition name
	uint32_t variation_hash = 0; // the variation found (0 = default)
	std::string database; // partition of the entry used ("" when none matched)
	std::vector<MaterialSlot> slots; // one per MeshAsset.Materials entry
};

// Names of the MeshVariationDatabase partitions of the library's first mounted superbundle (by name; cheap).
std::vector<std::string> variation_database_names(const core::EbxLibrary &p_library);

// Materials of p_mesh_partition for variation p_variation_hash (falls back to the default entry, then to the
// MeshMaterials' own TextureParameters), searching p_databases in order.
core::Result<MeshMaterials> mesh_materials(const core::EbxLibrary &p_library, std::string_view p_mesh_partition, uint32_t p_variation_hash,
		const std::vector<std::string> &p_databases);

// The same for a placed variant whose MeshVariationDatabaseEntry is already known (the level census's
// SceneMesh::entry, found through World::mesh_variation): p_asset is the MeshAsset instance, p_entry the entry or
// none (the MeshMaterials' own TextureParameters are then kept). Works on any resolver (the level path's EbxDb,
// an EbxLibrary). MeshMaterials::database names the entry's partition, variation_hash its VariationAssetNameHash.
// Bounded: at most kMaxMaterialSlots slots and kMaxTextureParameters texture parameters per slot are read.
inline constexpr size_t kMaxMaterialSlots = 4096;
inline constexpr size_t kMaxTextureParameters = 256;
core::Result<MeshMaterials> mesh_materials(const core::EbxResolver &p_db, const core::EbxInstance &p_asset, const std::optional<core::EbxInstance> &p_entry);

} // namespace bf3::render
