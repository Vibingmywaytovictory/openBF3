#pragma once

#include "hud_view_paint.h"
#include "minimap_debug.h"
#include "native_tags.h"
#include "overview_map.h"
#include "swf_view.h"
#include "ui_screen_host.h"
#include "ui_screen_records.h"

#include "bf3/core/ui/hud_messages.h"
#include "bf3/core/ui/hud_model.h"
#include "bf3/core/ui/hud_screen.h"
#include "bf3/core/ui/minimap.h"
#include "bf3/core/ui/screen_adjust.h"
#include "bf3/core/ui/ui_drawn_screens.h"

#include <godot_cpp/classes/font.hpp>
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

class Bf3LevelSession;
class Bf3MinimapHud;

inline constexpr size_t kNativeHudViews = 4;
inline constexpr size_t kNativeHudSharedView = 3;

struct NativeHudClipState {
	float x = 0.0f;
	float y = 0.0f;
	float xscale = 100.0f;
	float alpha = 100.0f;
	bool visible = false;
	uint32_t frame = 0;
};
inline constexpr std::array<std::string_view, kNativeHudViews> kNativeHudPartitions =
		bf3::core::ui::kHudDrawnPartitions;

inline constexpr std::string_view kUiMinimapCompPartition = "ui/uicomponents/uiminimapcomp";
inline constexpr int32_t kUiMinimapIsSmallKey = -1358262034;
inline constexpr int32_t kUiMinimapSmallZoomKey = -1127323954;
inline constexpr int32_t kUiMinimapLargeZoomKey = -1095821268;

struct NativeHudMinimapPose {
	float x = 0.0f;
	float z = 0.0f;
	float yaw = 0.0f;
	bool valid = false;
};

struct NativeHudDownedMate {
	uint8_t slot = 0;
	float x = 0.0f;
	float z = 0.0f;
};

struct NativeHudMinimapState {
	bool small = true;
	int32_t step = 0;
	float zoom = 0.0f;
	bool player_is_center = true;
	bool from_flow = false;
};

struct NativeHudPaintStats {
	uint32_t bg_panels = 0;
	uint32_t ekg_traces = 0;
	uint32_t minimap_maps = 0;
	uint32_t minimap_icons = 0;
	uint32_t minimap_downed = 0;
	uint32_t tag_icons = 0;
	uint32_t tag_names = 0;
	uint32_t paints = 0;
};

struct NativeHudReadout {
	uint64_t serial = 0;
	bool crosshair_widget = false;
	float crosshair_alpha = 0.0f;
	float crosshair_arm = 0.0f;
	double crosshair_base_offset = 0.0;
	float stage_height = 0.0f;
	float vfov_deg = 0.0f;
	bool ammo_visible = false;
	std::string clip_text;
	std::string reserve_text;
	std::string health_text;
	bool hit_shown = false;
};

class Bf3NativeHud : public godot::Node2D, public UiScreenHost, public UiScreenRecords {
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
	void set_safe_area(const bf3::core::ui::ScreenAdjustFractions &p_safe) { safe_area_ = p_safe; }
	std::optional<bf3::core::ui::HudStageRects> stage_rects() const;
	void set_minimap_pose(const NativeHudMinimapPose &p_pose);
	void set_minimap_pose_override(std::optional<NativeHudMinimapPose> p_pose) { pose_override_ = p_pose; }
	void set_minimap_debug_uv(bool p_on) { minimap_debug_uv_ = p_on; }
	void set_minimap_debug_record(bool p_on) { minimap_debug_record_ = p_on; }
	const MinimapDebugFrame &minimap_debug() const { return minimap_debug_; }
	void set_downed_mates(std::vector<NativeHudDownedMate> p_mates) { downed_ = std::move(p_mates); }
	const std::vector<NativeHudDownedMate> &downed_mates() const { return downed_; }
	std::vector<OverviewMark> downed_marks() const;
	void set_minimap_flow(bool p_small, std::optional<int32_t> p_step);
	const NativeHudMinimapState &minimap_state() const { return minimap_state_; }
	void set_manual(bool p_manual);
	void set_world(Bf3LevelSession *p_session, const godot::Camera3D *p_camera, bool p_show);
	NativeTags *tags() const { return tags_.get(); }
	const godot::Camera3D *world_camera() const { return world_camera_; }
	void set_fixture(const bf3::core::ui::HudModel &p_model, const NativeHudMinimapPose &p_pose);
	bool fixture() const { return fixture_; }
	void step(double p_seconds);

	bool is_ready() const { return built_; }
	bool has_failed() const { return failed_; }
	const std::string &error() const { return error_; }
	const bf3::core::ui::HudModel &model() const { return model_; }
	const bf3::core::ui::HudClientState &client() const { return client_; }
	bf3::core::Status open_screen(std::string_view p_partition) override;
	bool show_screen(std::string_view p_partition) override;
	bool hide_screen(std::string_view p_partition) override;
	bool order_screen(std::string_view p_partition, int32_t p_order) override;
	bool close_screen(std::string_view p_partition) override;
	bool set_screen_modal(std::string_view p_partition, bool p_modal) override;
	bool focus_screen(std::string_view p_partition) override;
	void clear_focus() override {}
	size_t screen_count() const override { return kNativeHudViews; }
	UiScreenSlot screen_slot(size_t p_index) const override;
	bool reports(std::string_view p_partition) const override;
	std::optional<UiScreenRecord> screen_record(std::string_view p_partition) const override;
	const NativeHudPaintStats &paint_stats() const { return paint_; }
	NativeHudReadout readout() const;
	std::optional<bf3::core::ui::SwfRect> window_bounds(size_t p_view, std::string_view p_path) const;
	std::optional<NativeHudClipState> clip_state(size_t p_view, std::string_view p_path);
	const bf3::core::ui::HudWorldTags &world_tags() const { return world_tags_; }
	double ekg_clock() const { return ekg_clock_; }
	std::optional<bf3::core::ui::MinimapLevel> minimap_level();
	uint32_t minimap_combat_area_side();
	bool minimap_ready();
	const NativeHudMinimapPose &minimap_pose() const { return pose_; }
	void set_ekg_clock(double p_seconds) { ekg_clock_ = p_seconds; }
	void set_additive_coverage(bool p_coverage);
	std::array<godot::Ref<godot::Image>, kNativeHudViews> capture_views() const;
	static godot::Ref<godot::Image> capture_layer(const std::array<godot::Ref<godot::Image>, kNativeHudViews> &p_colour,
			const std::array<godot::Ref<godot::Image>, kNativeHudViews> &p_coverage);
	uint64_t kill_feed_rows_added() const;
	const bf3::core::ui::HudMessageComp *messages() const { return messages_.get(); }
	double message_clock() const { return message_clock_; }
	const bf3::core::ui::HudDamageIndicatorComp *damage() const { return damage_.get(); }

protected:
	static void _bind_methods();
	void _notification(int p_what);

private:
	struct Loaded;
	void join_loader();
	void start_loader();
	void build();
	bf3::core::Status open_view(size_t p_index);
	void drop_view(size_t p_index);
	void arrange();
	void paint(size_t p_index);
	void paint_ekg(const SwfEngineTarget &p_target);
	void paint_minimap(const SwfEngineTarget &p_target, const bf3::core::ui::SwfScene &p_scene);
	void paint_tags();

	std::string level_ = "MP_Subway";
	bool manual_ = false;
	bool fixture_ = false;
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
	std::array<bool, kNativeHudViews> shown_{};
	std::array<uint64_t, kNativeHudViews> shown_count_{};
	std::array<int32_t, kNativeHudViews> order_{ 0, 1, 2, 3 };
	bf3::core::ui::HudModel model_{};
	uint64_t stepped_serial_ = 0;
	bf3::core::ui::HudClientState client_;
	bf3::core::ui::HudWorldTags world_tags_;
	bool minimap_profile_applied_ = false;
	std::unique_ptr<bf3::core::ui::HudMessageComp> messages_;
	std::unique_ptr<bf3::core::ui::HudDamageIndicatorComp> damage_;
	double message_clock_ = 0.0;
	double vfov_deg_ = 0.0;
	bf3::core::ui::ScreenAdjustFractions safe_area_;
	NativeHudMinimapPose pose_;
	std::optional<NativeHudMinimapPose> pose_override_;
	bool minimap_debug_uv_ = false;
	bool minimap_debug_record_ = false;
	MinimapDebugFrame minimap_debug_;
	std::vector<NativeHudDownedMate> downed_;
	NativeHudMinimapState minimap_state_;
	bool minimap_client_dirty_ = false;
	double ekg_clock_ = 0.0;
	HudBgPainter bg_;
	NativeHudPaintStats paint_;

	Bf3MinimapHud *minimap_source_ = nullptr;
	godot::Ref<godot::ShaderMaterial> minimap_material_;
	godot::Ref<godot::ImageTexture> minimap_mask_;
	godot::Ref<godot::ImageTexture> minimap_hatch_;
	godot::RID minimap_map_item_;
	godot::RID minimap_icon_item_;
	godot::RID minimap_parent_;
	std::unique_ptr<NativeTags> tags_;
	const godot::Camera3D *world_camera_ = nullptr;
	bool world_show_ = false;
	godot::RID tags_item_;
	godot::Ref<godot::Font> tag_font_;
};

}
