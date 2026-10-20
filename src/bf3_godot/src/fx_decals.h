#pragma once

#include "bf3/core/ebx_db.h"
#include "bf3/core/result.h"
#include "bf3/core/superbundle_mount.h"
#include "bf3/fx/triggers.h"
#include "bf3/render/texture_prep.h"

#include <godot_cpp/classes/decal.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/node3d.hpp>

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::godot_side {

inline constexpr float kDecalDepth = 0.12f;
inline constexpr float kDecalFadeOutSeconds = 5.0f;

struct FxDecalAssets {
	std::vector<fx::DecalTemplate> templates;
	std::map<std::string, render::PreparedTexture, std::less<>> textures;
	std::vector<std::string> errors;
	uint32_t without_texture = 0;
	uint32_t unkeyed = 0;
	uint32_t refused = 0;
	uint64_t texture_bytes = 0;

	static FxDecalAssets load(const core::EbxLibrary &p_level, const core::LevelMount &p_mount, std::span<const std::string> p_names);
	const fx::DecalTemplate *find(std::string_view p_name) const;
};

struct FxDecalStats {
	uint64_t placed = 0;
	uint64_t replaced = 0;
	uint64_t no_template = 0;
	uint64_t untextured = 0;
	uint64_t faded = 0;
	uint32_t live = 0;
	uint32_t textures = 0;
};

class FxDecalPool {
public:
	FxDecalPool() = default;
	~FxDecalPool();
	FxDecalPool(const FxDecalPool &) = delete;
	FxDecalPool &operator=(const FxDecalPool &) = delete;

	bool build(godot::Node3D *p_parent, const FxDecalAssets &p_assets, std::string *r_error);
	bool built() const { return root_ != nullptr; }
	void place(const fx::DecalCue &p_cue);

	void step(double p_dt);

	void clear();
	const FxDecalStats &stats() const { return stats_; }

	std::vector<std::string> texture_report(const std::string &p_dump_dir = {}) const;

private:
	struct Slot {
		godot::Decal *node = nullptr;
		double age = 0;
		bool live = false;
		uint64_t order = 0;
	};
	const FxDecalAssets *assets_ = nullptr;
	godot::Node3D *root_ = nullptr;
	uint64_t root_id_ = 0;
	std::map<std::string, godot::Ref<godot::ImageTexture>, std::less<>> textures_;
	std::vector<Slot> slots_;
	uint64_t next_order_ = 0;
	FxDecalStats stats_;
};

}
