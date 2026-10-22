#pragma once

#include "swf_view.h"

#include "bf3/core/ui/hud_model.h"
#include "bf3/core/ui/hud_screen.h"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/node2d.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/variant/rid.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace bf3::godot_side {

class Bf3MinimapHud;

inline constexpr size_t kNativeHudViews = 3;
inline constexpr std::array<std::string_view, kNativeHudViews> kNativeHudPartitions{ bf3::core::ui::kHudMpScreenPartition,
	bf3::core::ui::kHudTdmScreenPartition, bf3::core::ui::kCrosshairDefaultPartition };

struct NativeHudMinimapPose {
	float x = 0.0f;
	float z = 0.0f;
	float yaw = 0.0f;
	float metres_per_local_unit = 0.0f;
	bool valid = false;
};

struct NativeHudPaintStats {
	uint32_t bg_panels = 0;
	uint32_t ekg_traces = 0;
	uint32_t minimap_maps = 0;
	uint32_t minimap_icons = 0;
	uint32_t paints = 0;
};

struct NativeHudReadout {
	uint64_t serial = 0;
	bool crosshair_widget = false;
	float crosshair_alpha = 0.0f;
	float crosshair_arm = 0.0f;
	float stage_height = 0.0f;
	float vfov_deg = 0.0f;
	bool ammo_visible = false;
	std::string clip_text;
	std::string reserve_text;
	std::string health_text;
	bool hit_shown = false;
};

class Bf3NativeHud : public godot::Node2D {
	GDCLASS(Bf3NativeHud, godot::Node2D)

public:
	Bf3NativeHud();
	~Bf3NativeHud() override;

	void _ready() override;
	void _process(double p_delta) override;
	void _exit_tree() override;

	void set_level(const std::string &p_level);
	void set_model(const bf3::core::ui::HudModel &p_model);
	void set_vfov_deg(double p_deg);
	void set_minimap_pose(const NativeHudMinimapPose &p_pose);
	void set_manual(bool p_manual);
	void step(double p_seconds);

	bool is_ready() const { return built_; }
	bool has_failed() const { return failed_; }
	const std::string &error() const { return error_; }
	const bf3::core::ui::HudModel &model() const { return model_; }
	const bf3::core::ui::HudClientState &client() const { return client_; }
	Bf3SwfView *view(size_t p_index) const { return p_index < kNativeHudViews ? views_[p_index] : nullptr; }
	bf3::core::ui::HudScreen *screen(size_t p_index) const { return p_index < kNativeHudViews ? screens_[p_index].get() : nullptr; }
	const NativeHudPaintStats &paint_stats() const { return paint_; }
	NativeHudReadout readout() const;
	std::optional<bf3::core::ui::SwfRect> window_bounds(size_t p_view, std::string_view p_path) const;
	double ekg_clock() const { return ekg_clock_; }
	void set_ekg_clock(double p_seconds) { ekg_clock_ = p_seconds; }
	godot::Ref<godot::Image> capture_layer() const;

protected:
	static void _bind_methods();
	void _notification(int p_what);

private:
	struct Loaded;
	void join_loader();
	void start_loader();
	void build();
	void apply_effects(size_t p_index);
	void paint(size_t p_index);
	void paint_bg(const SwfEngineTarget &p_target);
	void paint_ekg(const SwfEngineTarget &p_target);
	void paint_minimap(const SwfEngineTarget &p_target, const bf3::core::ui::SwfScene &p_scene);
	bool minimap_ready();

	std::string level_ = "MP_Subway";
	bool manual_ = false;
	bool started_ = false;
	bool built_ = false;
	bool failed_ = false;
	std::string error_;
	std::thread loader_;
	std::atomic<bool> loader_done_{ false };
	std::unique_ptr<Loaded> loaded_;

	std::array<Bf3SwfView *, kNativeHudViews> views_{};
	std::array<std::unique_ptr<bf3::core::ui::HudScreen>, kNativeHudViews> screens_;
	std::array<uint32_t, kNativeHudViews> effects_seen_{};
	bf3::core::ui::HudModel model_{};
	uint64_t stepped_serial_ = 0;
	bf3::core::ui::HudClientState client_;
	double vfov_deg_ = 0.0;
	NativeHudMinimapPose pose_;
	double ekg_clock_ = 0.0;
	std::map<std::string, bf3::core::ui::SwfRect, std::less<>> painted_bounds_;
	NativeHudPaintStats paint_;

	Bf3MinimapHud *minimap_source_ = nullptr;
	godot::Ref<godot::ShaderMaterial> minimap_material_;
	godot::Ref<godot::ImageTexture> minimap_mask_;
	godot::Ref<godot::ImageTexture> minimap_hatch_;
	godot::RID minimap_map_item_;
	godot::RID minimap_icon_item_;
	godot::RID minimap_parent_;
};

}
