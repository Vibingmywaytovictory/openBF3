#pragma once

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

inline constexpr size_t kLookMaxGraphCandidates = 64;

enum class LookTier : uint8_t {
	Parameter,
	Graph,
	Neighbour,
};
const char *look_tier_name(LookTier p_tier);

inline constexpr uint8_t kLookUv0 = 0;
inline constexpr uint8_t kLookUv1 = 1;
inline constexpr uint8_t kLookUvOverlay = 2;
inline constexpr uint8_t kLookUvProjected = 3;

inline constexpr float kLookGlassOpacity = 0.3f;
inline constexpr float kLookGlassTint = 0.25f;

struct LookTexture {
	std::string name;
	std::string parameter;
	LookTier tier = LookTier::Parameter;
	uint8_t channel = kLookUv0;
	float scale = 1.0f;
	uint8_t quality = 0;
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
	bool shader_in_database = false;
	bool drawn = true;
	size_t color_candidates = 0;
	size_t normal_candidates = 0;
	bool glass = false;
};

struct LookStats {
	size_t shader_databases = 0;
	size_t shaders = 0;
	uint64_t shader_work = 0;
	size_t texture_headers = 0;
	size_t texture_headers_failed = 0;
	size_t graph_candidates_max = 0;
	size_t graph_candidates_dropped = 0;
	uint64_t resolve_work = 0;
	double seconds_database = 0.0;
	double seconds_resolve = 0.0;
};

class LookResolver {
public:

	static std::unique_ptr<LookResolver> open(const core::LevelMount &p_mount);

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

	struct GraphInfo;
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
	std::vector<std::string> texture_names_;
	std::map<std::string, std::vector<std::string>> folders_;
	bool folders_built_ = false;
	std::map<const SurfaceShader *, std::unique_ptr<GraphInfo>> graphs_;
	std::map<std::string, std::unique_ptr<FolderIndex>> folder_indices_;
	std::vector<std::pair<std::string, std::string>> failures_;
	LookStats stats_;
};

struct NameRole {
	uint8_t role = 0;
	uint8_t quality = 0;
};
std::optional<NameRole> suffix_role(std::string_view p_name);
std::optional<NameRole> graph_role(std::string_view p_name);
bool placeholder_texture(std::string_view p_name);

}
