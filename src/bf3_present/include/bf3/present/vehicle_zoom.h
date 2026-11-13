#pragma once

#include <cstdint>
#include <optional>

namespace bf3::present {

struct VehicleZoomDef {
	float fov = 0.0f;
	float fov_transition = 0.0f;
	bool fade_to_black = false;
	float fade_out = 0.0f;
	float black = 0.0f;
	float fade_in = 0.0f;
};

struct VehicleZoomView {
	float fov = 0.0f;
	float black = 0.0f;
	bool zoomed = false;
};

class VehicleZoom {
public:
	VehicleZoomView step(const std::optional<VehicleZoomDef> &p_zoom, bool p_held, float p_regular_fov, double p_dt);
	void reset();

private:
	enum class Phase : uint8_t { Idle, FadeOut, Black, FadeIn };

	Phase phase_ = Phase::Idle;
	bool zoomed_ = false;
	float t_ = 0.0f;
	std::optional<float> fov_;
};

}
