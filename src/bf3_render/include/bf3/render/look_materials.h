#pragma once
// L1 look: which base colour and normal map a level material draws, and how (alpha, blend, two-sided, UV set and
// tiling). Godot-free; runs on the loader thread that plans the scene.
//
// Why M1 drew about half of Metro's materials flat: the MeshVariationDatabase entry (and the MeshMaterial) of those
// slots lists no TextureParameters at all. Their textures are not EBX parameters but constants of the surface
// shader graph, which only the level's compiled shader database names (shader_database.h). The resolver ranks, per
// role, the candidates of three tiers (port of IW4L level_bf3 geometry/materials.rs, the user's Apache-2.0 code):
//   1. Parameter: the slot's texture parameters, by name (COLOR / NORMAL parameter lists below, best first), each
//      sampled as the graph's parameter of that name says (UV set, tiling factor);
//   2. Graph: the textures the shader graph samples as constants (ShaderDatabase textures, then the textures its
//      pixel constants bind), by their name's role suffix (_d / _c / _df colour, _n / _nm normal, _m / _rgb masks as
//      a colour of last resort, a name without a role suffix as a weaker colour unless it says mask, dirt, ao, ...);
//      the variation's own graph before the material's;
//   3. Neighbour: for a shader the database does not hold, textures named like the mesh in the shader's or the
//      mesh's folder.
// Ranking within a role: tier, then off the overlay UV set (TEXCOORD2), then quality (proper map, mask, unnamed),
// then the graph, untiled before tiled, streamed before resident, authored order. A colour or specular candidate
// whose texture is stored in a normal-map format becomes a normal candidate; placeholders (black, white, noise,
// default*) and textures that are not 2D are never drawn. The normal map shares the colour's UV set and tiling when one
// does, else it is the best on a mesh UV set (each texture is sampled with its own UV set and tiling).
// Blend and sides come from the shader database (surface type, double-sided solutions), else from the shader name
// as in M1.
//
// Work (hostile shader databases and texture lists): each graph's candidates (its constants' roles, the streamed /
// resident split, its parameter index) are built once per graph in O(n log n) and capped at kLookMaxGraphCandidates
// colour and normal candidates; each folder's neighbour candidates are built once per folder and kept sorted by file
// name, so a slot finds its longest shared prefix by binary search. A slot's resolution is then O(its parameters x
// log + the cap + folders x log), never the product of a graph's or folder's size and the slot count.

#include "bf3/core/res/dx_texture.h"
#include "bf3/core/result.h"
#include "bf3/render/material_lookup.h"
#include "bf3/render/shader_database.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core {
class LevelMount;
}

namespace bf3::render {

// Colour and normal candidates kept per shader graph (authored order: streamed constants, then resident ones). Retail
// Metro's widest graph has far fewer (bf3dump looks prints the maximum); more are dropped and counted.
inline constexpr size_t kLookMaxGraphCandidates = 64;

enum class LookTier : uint8_t {
	Parameter,
	Graph,
	Neighbour,
};
const char *look_tier_name(LookTier p_tier);

// UV source of a drawn texture.
inline constexpr uint8_t kLookUv0 = 0;
inline constexpr uint8_t kLookUv1 = 1;
inline constexpr uint8_t kLookUvOverlay = 2; // TEXCOORD2: not in the prepared meshes; drawn from UV0 at scale 1
inline constexpr uint8_t kLookUvProjected = 3; // world-projected graph textures: drawn from UV0 [not reproduced]

struct LookTexture {
	std::string name; // DxTexture res name (lowercase)
	std::string parameter; // the slot parameter it came from, or "" for a graph / neighbour texture
	LookTier tier = LookTier::Parameter;
	uint8_t channel = kLookUv0;
	float scale = 1.0f; // UV multiplier (1 / the graph's factor, clamped to 1/256..64)
	uint8_t quality = 0; // 0 a proper map, 1 a mask standing in for a colour, 2 a graph texture without a role suffix
};

enum class LookBlend : uint8_t {
	Opaque,
	AlphaTest,
	Blend,
};

struct LookMaterial {
	std::optional<LookTexture> color;
	std::optional<LookTexture> normal;
	LookBlend blend = LookBlend::Opaque;
	bool double_sided = false;
	bool shader_in_database = false; // the slot's shader graph (or its variation's) is in the level shader database
	bool drawn = true; // false for the undrawn shader words (invisible, occluder, shadowmesh, ...) and mask-only blends
	size_t color_candidates = 0; // candidates of each role the resolver ranked (the census: whether BF3 gives one)
	size_t normal_candidates = 0;
	bool glass = false; // a glass / window shader without colour alpha: drawn translucent at a fixed opacity, colour as tint
};

struct LookStats {
	size_t shader_databases = 0; // shaderdb res read
	size_t shaders = 0; // surface shaders of the first render path
	uint64_t shader_work = 0; // parse_shader_database work units
	size_t texture_headers = 0; // DxTexture headers read for candidates
	size_t texture_headers_failed = 0;
	size_t graph_candidates_max = 0; // the most colour and normal candidates one graph gave (before the cap)
	size_t graph_candidates_dropped = 0; // candidates past kLookMaxGraphCandidates, summed over graphs
	uint64_t resolve_work = 0; // entries visited building graph and folder indices and resolving slots
	double seconds_database = 0.0;
	double seconds_resolve = 0.0;
};

// Resolves material slots of one level. Not thread-safe (it caches texture headers); one per plan.
class LookResolver {
public:
	// Reads every `*/shaderdb` res of the mount (res type IShaderDatabase; the first holding a shader wins). A
	// database that does not parse is listed in failures() and left out.
	static std::unique_ptr<LookResolver> open(const core::LevelMount &p_mount);
	// A resolver without a mount (tests): the shader lookup and a fixed header table.
	struct HeaderInfo {
		core::res::TextureKind kind = core::res::TextureKind::Texture2D;
		core::res::TextureFormat format = core::res::TextureFormat::Dxt1;
	};
	LookResolver(ShaderLookup p_shaders, std::map<std::string, HeaderInfo> p_headers, std::vector<std::string> p_texture_names = {});
	~LookResolver();

	LookMaterial resolve(const MaterialSlot &p_slot, std::string_view p_mesh);

	const ShaderLookup &shaders() const { return shaders_; }
	const std::vector<std::pair<std::string, std::string>> &failures() const { return failures_; }
	LookStats stats() const { return stats_; }

	struct GraphInfo; // look_materials.cpp
	struct FolderIndex;

private:
	LookResolver();
	const HeaderInfo *header(const std::string &p_name);
	const std::vector<std::string> &folder(const std::string &p_folder);
	const GraphInfo &graph_info(const SurfaceShader &p_shader);
	const FolderIndex &folder_index(const std::string &p_folder);

	const core::LevelMount *mount_ = nullptr;
	ShaderLookup shaders_;
	std::map<std::string, std::optional<HeaderInfo>> headers_;
	std::vector<std::string> texture_names_; // every DxTexture res of the mount, lowercase, sorted
	std::map<std::string, std::vector<std::string>> folders_;
	bool folders_built_ = false;
	std::map<const SurfaceShader *, std::unique_ptr<GraphInfo>> graphs_;
	std::map<std::string, std::unique_ptr<FolderIndex>> folder_indices_;
	std::vector<std::pair<std::string, std::string>> failures_;
	LookStats stats_;
};

// Exposed for the tests: the role a graph constant's name says (0 colour, 1 normal, 2 specular, 3 emissive) and its
// quality, or nullopt; and whether a name is an engine placeholder.
struct NameRole {
	uint8_t role = 0;
	uint8_t quality = 0;
};
std::optional<NameRole> suffix_role(std::string_view p_name);
std::optional<NameRole> graph_role(std::string_view p_name);
bool placeholder_texture(std::string_view p_name);

} // namespace bf3::render
