#pragma once

#include "bf3/core/ui/minimap.h"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/variant/rect2.hpp>
#include <godot_cpp/variant/vector2i.hpp>

#include <string>
#include <vector>

namespace godot {
class CanvasLayer;
class ColorRect;
}

namespace bf3::godot_side {

class Bf3MinimapHud;

inline constexpr int kMinimapCheckRingSamples = 16;
inline constexpr int kMinimapCheckRings = 3;

class MinimapCheck {
public:

	MinimapCheck(Bf3MinimapHud &p_hud, std::string p_shots_dir, bool p_rows = true);

	bool step(double p_delta);
	int exit_code() const;

private:
	struct Row {
		std::string name;
		bool pass = false;
		std::string detail;
	};

	struct ViewFrames {
		godot::Ref<godot::Image> uv;
		godot::Ref<godot::Image> hidden;
		godot::Ref<godot::Image> marker;
		godot::Ref<godot::Image> full;
		bf3::core::ui::MinimapView view;
	};
	void add(const std::string &p_name, bool p_pass, const std::string &p_detail);
	godot::Ref<godot::Image> grab() const;
	void row_calibration(const godot::Ref<godot::Image> &p_image);
	void row_map_uv(const char *p_name, const ViewFrames &p_frames);
	void row_marker_pixels(const ViewFrames &p_north, const ViewFrames &p_rotating);
	void row_icons_minimap(const ViewFrames &p_north, const ViewFrames &p_rotating);
	void evaluate_icons(const ViewFrames &p_frames, std::string &r_detail, bool &r_pass, size_t &r_checked);
	void row_icons_world(const godot::Ref<godot::Image> &p_hidden, const godot::Ref<godot::Image> &p_visible);
	void finish_rows();

	Bf3MinimapHud &hud_;
	std::string shots_dir_;
	bool rows_enabled_ = true;
	std::vector<Row> rows_;
	int phase_ = 0;
	int frames_in_phase_ = 0;
	int wait_ = 0;
	float yaw0_ = 0.0f;
	float pitch0_ = 0.0f;
	float world_yaw_ = 0.0f;
	godot::CanvasLayer *calibration_layer_ = nullptr;
	godot::ColorRect *calibration_ = nullptr;
	godot::Vector2i calibration_px_;
	ViewFrames north_;
	ViewFrames rotating_;
	godot::Ref<godot::Image> world_hidden_;

	std::vector<bf3::core::ui::ScreenPoint> north_icons_;
	std::vector<godot::Rect2> north_rects_;
	std::vector<bool> north_clamped_;
	std::vector<bf3::core::ui::ScreenPoint> rotating_icons_;
	std::vector<godot::Rect2> rotating_rects_;
	std::vector<bool> rotating_clamped_;
	size_t shot_index_ = 0;
	size_t shots_saved_ = 0;
	bool shots_failed_ = false;
	bool done_ = false;
	int exit_code_ = 1;
};

}
