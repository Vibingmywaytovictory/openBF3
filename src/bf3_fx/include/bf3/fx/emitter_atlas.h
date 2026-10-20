#pragma once

#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/work_budget.h"
#include "bf3/core/res/dx_texture.h"
#include "bf3/core/result.h"
#include "bf3/core/superbundle_mount.h"
#include "bf3/fx/fx_types.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::fx {

enum class AtlasKind : uint8_t {
	Base,
	Normal,
};

struct AtlasTextureInfo {
	std::string partition;
	core::res::TextureFormat format = core::res::TextureFormat::Dxt5;
	bool srgb = false;
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t mips = 0;
};

struct AtlasEntry {
	std::string name;
	AtlasKind kind = AtlasKind::Base;
	Vec2 min_uv;
	Vec2 max_uv;
	bool degenerate = false;

	uint32_t px_min_x = 0, px_min_y = 0, px_max_x = 0, px_max_y = 0;
	bool pixel_exact = false;
};

class EmitterAtlas {
public:

	static core::Result<EmitterAtlas> load(const core::EbxResolver &p_db, const core::LevelMount &p_mount, std::string_view p_system,
			core::gameplay::WorkBudget &r_budget);

	using ResSource = std::function<core::Result<std::vector<uint8_t>>(const std::string &p_partition)>;
	static core::Result<EmitterAtlas> load(const core::EbxResolver &p_db, const ResSource &p_res, std::string_view p_system,
			core::gameplay::WorkBudget &r_budget);

	static core::Result<AtlasTextureInfo> texture_info(const std::string &p_partition, std::span<const uint8_t> p_res);

	static std::string emitter_system_of(std::string_view p_level);

	static AtlasKind kind_of_name(std::string_view p_name);

	const AtlasTextureInfo &base() const { return base_; }
	const AtlasTextureInfo &normal() const { return normal_; }
	const std::vector<AtlasEntry> &entries() const { return entries_; }

	const AtlasEntry *resolve(std::string_view p_name) const;

	static void frame_rect(const AtlasEntry &p_entry, uint32_t p_frame, uint32_t p_frame_count, uint32_t p_frame_columns, Vec2 &r_min, Vec2 &r_max);

private:
	AtlasTextureInfo base_;
	AtlasTextureInfo normal_;
	std::vector<AtlasEntry> entries_;
	std::vector<uint32_t> by_name_;
};

}
