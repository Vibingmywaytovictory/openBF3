#pragma once

#include "bf3/core/level/eor_camera.h"
#include "bf3/core/ui/hud_model.h"

#include <godot_cpp/variant/transform3d.hpp>

#include <cstdint>
#include <optional>

namespace godot {
class Camera3D;
}

namespace bf3::godot_side {

struct EorCameraStats {
	uint64_t taken = 0;
	uint64_t released = 0;
	uint64_t frames = 0;
};

class EorCameraView {
public:
	void frame(godot::Camera3D *p_camera, const std::optional<core::level::EorCamera> &p_camera_data,
			const core::ui::HudEndOfRound &p_end_of_round);
	bool active() const { return active_; }
	const EorCameraStats &stats() const { return stats_; }

private:
	bool active_ = false;
	godot::Transform3D player_view_;
	float player_fov_deg_ = 0.0f;
	EorCameraStats stats_;
};

}
