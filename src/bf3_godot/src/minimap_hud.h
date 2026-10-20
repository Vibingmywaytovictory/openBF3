#pragma once

#include "bf3/core/install_reader.h"
#include "bf3/core/ui/hud_icons.h"
#include "bf3/core/ui/minimap.h"

#include <godot_cpp/classes/canvas_layer.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/variant/node_path.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace bf3::godot_side {

class Bf3LevelSession;
class Bf3MinimapControl;
class Bf3WorldIcons;
class MinimapCheck;
class Pt1Iff;

inline constexpr int32_t kM1LocalTeam = 1;

struct MinimapAssets {
	bf3::core::ui::MinimapLevel level;
	bf3::core::ui::HudIconAtlas atlas;
	uint32_t satellite_side = 0;
	uint32_t combat_area_side = 0;
	uint32_t atlas_width = 0;
	uint32_t atlas_height = 0;
	double seconds = 0.0;
	int64_t peak_working_set_delta = 0;
	int64_t working_set_delta = 0;

	float marker_alpha_dx = 0.0f;
	float marker_alpha_dy = 0.0f;
	std::string error;
	std::string steps;
};

struct MinimapPixels {
	bf3::core::ui::MinimapImages images;
	std::vector<bf3::core::ui::L8Image> satellite_mips;
	bf3::core::ui::Rgba8Image atlas_image;
};

class Bf3MinimapHud : public godot::CanvasLayer {
	GDCLASS(Bf3MinimapHud, godot::CanvasLayer)

public:
	Bf3MinimapHud();
	~Bf3MinimapHud() override;

	void _ready() override;
	void _process(double p_delta) override;
	void _unhandled_input(const godot::Ref<godot::InputEvent> &p_event) override;

	void set_session_path(const godot::NodePath &p_path);
	godot::NodePath get_session_path() const;

	bool ready_to_draw() const { return drawn_; }
	std::shared_ptr<const MinimapAssets> assets() const;
	const bf3::core::ui::MinimapView &view() const { return view_; }
	Bf3MinimapControl *control() const { return control_; }
	Bf3WorldIcons *world_icons() const { return icons_; }
	Bf3LevelSession *session() const { return session_; }
	void set_north_up(bool p_on);

	void set_center_override(float p_x, float p_z);
	void clear_center_override();

protected:
	static void _bind_methods();
	void _notification(int p_what);

private:
	void parse_args();
	void run_loader();
	void build_godot_side();
	void update_view();
	void layout_controls();
	bool inert_args() const;

	godot::NodePath session_path_ = godot::NodePath("../Session");
	Bf3LevelSession *session_ = nullptr;
	std::string mode_override_;
	int size_px_ = 0;
	bool enabled_ = true;
	bool north_up_ = false;
	std::string shots_dir_;
	bool check_ = false;

	bf3::core::InstallReader reader_;
	std::string level_id_;
	std::string mode_id_;
	bool loader_started_ = false;
	std::thread loader_;
	std::atomic<bool> stop_{ false };
	std::atomic<bool> published_{ false };
	mutable std::mutex mutex_;
	std::shared_ptr<const MinimapAssets> assets_;
	std::unique_ptr<MinimapPixels> pixels_;
	bool center_override_ = false;
	float center_override_x_ = 0.0f;
	float center_override_z_ = 0.0f;

	bool build_attempted_ = false;
	bool drawn_ = false;
	godot::Ref<godot::ImageTexture> satellite_;
	godot::Ref<godot::ImageTexture> combat_area_;
	godot::Ref<godot::ImageTexture> detail_;
	godot::Ref<godot::ImageTexture> atlas_;
	Bf3MinimapControl *control_ = nullptr;
	Bf3WorldIcons *icons_ = nullptr;
	bf3::core::ui::MinimapView view_;
	std::unique_ptr<MinimapCheck> check_runner_;
	std::unique_ptr<Pt1Iff> pt1_iff_;
};

}
