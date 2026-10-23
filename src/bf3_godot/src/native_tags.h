#pragma once

#include "iff_frame.h"

#include "bf3/core/ui/nametags.h"

#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector2.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace godot {
class Camera3D;
}

namespace bf3::godot_side {

class Bf3LevelSession;

inline constexpr uint32_t kMaxLosRaysPerFrame = 4;
inline constexpr double kNativeTagsSummaryEveryS = 30.0;

struct NativeTag {
	uint8_t slot = 0;
	core::ui::TagRelation relation = core::ui::TagRelation::None;
	bool icon = false;
	bool name = false;
	float scale = 1.0f;
	float alpha = 1.0f;
	float distance_m = 0.0f;
	uint8_t kit = kIffNoKit;
	godot::Vector2 anchor;
	godot::String text;
};

struct NativeTagStats {
	uint64_t frames = 0;
	uint64_t friendly_icons = 0;
	uint64_t friendly_names = 0;
	uint64_t enemy_names = 0;
	uint64_t los_rays = 0;
	uint64_t los_blocked = 0;
	uint64_t minimap_friendly = 0;
	uint64_t minimap_enemy = 0;
	uint64_t minimap_hidden_enemies = 0;
	uint64_t unknown_team = 0;
	uint64_t frame_ns = 0;
	uint64_t frame_max_ns = 0;
};

struct NativeTagFixture {
	std::vector<IffSoldier> soldiers;
	std::vector<std::string> names;
	uint8_t own_team = 0;
	uint8_t own_squad = 0;
};

class NativeTags {
public:
	explicit NativeTags(Bf3LevelSession *p_session);
	~NativeTags();
	NativeTags(const NativeTags &) = delete;
	NativeTags &operator=(const NativeTags &) = delete;

	void update(const godot::Camera3D *p_camera, bool p_show, godot::Vector2 p_viewport_size);
	void set_fixture(NativeTagFixture p_fixture);

	bool loaded() const { return applied_; }
	const core::ui::NametagStyle &style() const { return style_; }
	const std::array<std::string, 4> &kit_icons() const { return kit_icons_; }
	const std::vector<NativeTag> &tags() const { return tags_; }
	const std::vector<IffMinimapEntry> &minimap() const { return minimap_; }
	float canvas_scale() const { return canvas_scale_; }
	const NativeTagStats &stats() const { return stats_; }

private:
	struct Loaded;
	void run_loader();
	void apply_loaded();
	void log_summary() const;

	Bf3LevelSession *session_ = nullptr;
	std::thread loader_;
	std::atomic<bool> published_{ false };
	std::atomic<bool> stop_{ false };
	std::mutex mutex_;
	std::shared_ptr<const Loaded> loaded_;
	bool applied_ = false;
	core::ui::NametagStyle style_;
	core::ui::SpottingRules spotting_;
	std::array<std::string, 4> kit_icons_;
	IffReveal reveal_;
	NativeTagStats stats_;
	std::unique_ptr<NativeTagFixture> fixture_;
	std::vector<NativeTag> tags_;
	std::vector<IffMinimapEntry> minimap_;
	float canvas_scale_ = 1.0f;
	double summary_at_s_ = -1.0;
};

}
