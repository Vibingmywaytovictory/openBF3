#include "bf3/present/vehicle_zoom.h"

#include <algorithm>
#include <cmath>

namespace bf3::present {

void VehicleZoom::reset() {
	phase_ = Phase::Idle;
	zoomed_ = false;
	t_ = 0.0f;
	fov_.reset();
}

VehicleZoomView VehicleZoom::step(
		const std::optional<VehicleZoomDef> &p_zoom, bool p_held, float p_regular_fov, double p_dt) {
	if (!p_zoom) {
		reset();
		return { p_regular_fov, 0.0f, false };
	}
	const VehicleZoomDef &z = *p_zoom;
	const float dt = float(std::max(0.0, p_dt));
	if (!z.fade_to_black) {
		zoomed_ = p_held;
		const float target = zoomed_ ? z.fov : p_regular_fov;
		const float from = fov_.value_or(target);
		const float span = std::fabs(z.fov - p_regular_fov);
		const float rate = z.fov_transition > 0.0f ? span / z.fov_transition : 0.0f;
		const float moved = rate > 0.0f ? rate * dt : std::fabs(target - from);
		fov_ = std::fabs(target - from) <= moved ? target : from + std::copysign(moved, target - from);
		return { *fov_, 0.0f, zoomed_ };
	}
	fov_.reset();
	t_ += dt;
	float black = 0.0f;
	for (int guard = 0; guard < 4; ++guard) {
		if (phase_ == Phase::Idle) {
			if (p_held == zoomed_) {
				t_ = 0.0f;
				break;
			}
			phase_ = Phase::FadeOut;
			t_ = dt;
		}
		if (phase_ == Phase::FadeOut) {
			if (t_ < z.fade_out) {
				black = t_ / z.fade_out;
				break;
			}
			t_ -= std::max(0.0f, z.fade_out);
			zoomed_ = p_held;
			phase_ = Phase::Black;
		}
		if (phase_ == Phase::Black) {
			if (t_ < z.black) {
				black = 1.0f;
				break;
			}
			t_ -= std::max(0.0f, z.black);
			phase_ = Phase::FadeIn;
		}
		if (phase_ == Phase::FadeIn) {
			if (t_ < z.fade_in) {
				black = 1.0f - t_ / z.fade_in;
				break;
			}
			t_ = 0.0f;
			phase_ = Phase::Idle;
			break;
		}
	}
	return { zoomed_ ? z.fov : p_regular_fov, std::clamp(black, 0.0f, 1.0f), zoomed_ };
}

}
