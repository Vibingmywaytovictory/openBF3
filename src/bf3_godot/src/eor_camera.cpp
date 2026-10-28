#include "eor_camera.h"

#include "level_upload.h"

#include <godot_cpp/classes/camera3d.hpp>

namespace bf3::godot_side {

using namespace godot;

void EorCameraView::frame(Camera3D *p_camera, const std::optional<core::level::EorCamera> &p_camera_data,
		const core::ui::HudEndOfRound &p_end_of_round) {
	if (p_camera == nullptr) {
		return;
	}
	const bool want = p_camera_data.has_value() && p_end_of_round.stage != core::ui::HudEorStage::None;
	if (want && !active_) {
		player_view_ = p_camera->get_global_transform();
		player_fov_deg_ = float(p_camera->get_fov());
		active_ = true;
		++stats_.taken;
	} else if (!want && active_) {
		p_camera->set_global_transform(player_view_);
		p_camera->set_fov(player_fov_deg_);
		active_ = false;
		++stats_.released;
		return;
	}
	if (!active_) {
		return;
	}
	++stats_.frames;
	p_camera->set_global_transform(instance_transform(p_camera_data->transform));
	p_camera->set_fov(p_camera_data->fov_deg);
}

}
