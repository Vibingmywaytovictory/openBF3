#pragma once

#include "bf3/anim/crowd_eval.h"
#include "bf3/anim/pose_math.h"

#include <array>
#include <cmath>
#include <cstdint>

namespace bf3::anim {

enum class StagePhase : uint8_t { Idle, RunFwd, RunBwd, RunLeft, RunRight, RunDiagonal, RunCircle, Reload, AimUp, AimDown, Count };
constexpr size_t kStagePhases = size_t(StagePhase::Count);

inline constexpr Vec3 kMetroStageOrigin{ -264.0f, 64.40f, -463.0f };
inline constexpr float kMetroStageHeading = 1.57079633f;

inline constexpr std::array<float, 8> kLodRowDistance{ 1.0f, 15.0f, 45.0f, 75.0f, 105.0f, 135.0f, 175.0f, 215.0f };

struct CrowdStageSpec {
	uint32_t rows = 8;
	uint32_t cols = 8;
	float spacing = 2.0f;
	float phase_seconds = 4.0f;
	float ramp_seconds = 1.0f;
	float run_speed = 4.0f;
	float side_factor = 0.75f;
	float circle_speed = 3.5f;
	float circle_turn_deg_s = 90.0f;
	float aim_pitch_deg = 45.0f;
	Vec3 origin = kMetroStageOrigin;
	float heading = kMetroStageHeading;

	float camera_back = 9.5f;
	float camera_up = 3.0f;
	float camera_fov_deg = 60.0f;
	float target_height = 0.9f;

	float box_half = 0.7f;
	float box_height = 1.9f;

	constexpr uint32_t soldiers() const { return rows * cols; }
};

struct StageCamera {
	Vec3 position{};
	Vec3 target{};
	float fov_y_deg = 60.0f;
};

inline Vec3 stage_cell(const CrowdStageSpec &p_spec, uint32_t p_i) {
	const float fx = std::sin(p_spec.heading), fz = std::cos(p_spec.heading);
	const float rx = fz, rz = -fx;
	const uint32_t cols = p_spec.cols == 0 ? 1 : p_spec.cols;
	const float x = (float(p_i % cols) - float(cols - 1) * 0.5f) * p_spec.spacing;
	const float z = float(p_i / cols) * p_spec.spacing;
	return Vec3{ p_spec.origin.x + rx * x + fx * z, p_spec.origin.y, p_spec.origin.z + rz * x + fz * z };
}

inline StageCamera stage_camera(const CrowdStageSpec &p_spec) {
	const float fx = std::sin(p_spec.heading), fz = std::cos(p_spec.heading);
	const float depth = float(p_spec.rows > 0 ? p_spec.rows - 1 : 0) * p_spec.spacing;
	StageCamera c;
	c.position = Vec3{ p_spec.origin.x - fx * p_spec.camera_back, p_spec.origin.y + p_spec.camera_up, p_spec.origin.z - fz * p_spec.camera_back };
	c.target = Vec3{ p_spec.origin.x + fx * depth * 0.5f, p_spec.origin.y + p_spec.target_height, p_spec.origin.z + fz * depth * 0.5f };
	c.fov_y_deg = p_spec.camera_fov_deg;
	return c;
}

inline float stage_frustum_margin(const CrowdStageSpec &p_spec, const StageCamera &p_camera, float p_aspect) {
	const float tv = std::tan(p_camera.fov_y_deg * 0.5f * 0.0174532925f);
	const float th = tv * p_aspect;
	Vec3 f{ p_camera.target.x - p_camera.position.x, p_camera.target.y - p_camera.position.y, p_camera.target.z - p_camera.position.z };
	const float fl = std::sqrt(f.x * f.x + f.y * f.y + f.z * f.z);
	if (!(fl > 1.0e-4f) || !(tv > 0.0f) || !(th > 0.0f)) {
		return -1.0f;
	}
	f = Vec3{ f.x / fl, f.y / fl, f.z / fl };

	Vec3 r{ f.z, 0.0f, -f.x };
	const float rl = std::sqrt(r.x * r.x + r.z * r.z);
	if (!(rl > 1.0e-4f)) {
		return -1.0f;
	}
	r = Vec3{ r.x / rl, 0.0f, r.z / rl };
	const Vec3 u{ f.y * r.z - f.z * r.y, f.z * r.x - f.x * r.z, f.x * r.y - f.y * r.x };
	float worst = 1.0f;
	const uint32_t n = p_spec.soldiers();
	for (uint32_t i = 0; i < n; ++i) {
		const Vec3 c = stage_cell(p_spec, i);
		for (int k = 0; k < 8; ++k) {
			const Vec3 p{ c.x + ((k & 1) ? p_spec.box_half : -p_spec.box_half) - p_camera.position.x,
				c.y + ((k & 2) ? p_spec.box_height : 0.0f) - p_camera.position.y, c.z + ((k & 4) ? p_spec.box_half : -p_spec.box_half) - p_camera.position.z };
			const float zf = p.x * f.x + p.y * f.y + p.z * f.z;
			if (!(zf > 0.05f)) {
				return -1.0f;
			}
			const float xr = std::fabs(p.x * r.x + p.y * r.y + p.z * r.z);
			const float yu = std::fabs(p.x * u.x + p.y * u.y + p.z * u.z);
			const float mh = 1.0f - xr / (zf * th);
			const float mv = 1.0f - yu / (zf * tv);
			worst = mh < worst ? mh : worst;
			worst = mv < worst ? mv : worst;
		}
	}
	return worst;
}

StagePhase stage_phase(const CrowdStageSpec &p_spec, uint32_t p_i, double p_t);

CrowdSoldierInput stage_input(const CrowdStageSpec &p_spec, uint32_t p_i, double p_t, double p_prev_t, const Vec3 &p_camera, bool p_lod_spread);

struct StageCoverage {
	double two_cycle_fraction = 0.0;
	double idle_mix_fraction = 0.0;
	double mean_loco_clips = 0.0;
};
StageCoverage stage_coverage(const CrowdStageSpec &p_spec, const std::array<LocoClipInfo, kLocoClips> &p_clips, const LocoParams &p_params, double p_step);

namespace crowd_gate {
constexpr uint32_t kSoldiers = 64;
constexpr uint32_t kInView = 64;
constexpr uint32_t kWidth = 2560;
constexpr uint32_t kHeight = 1440;
constexpr double kMinSeconds = 30.0;
constexpr double kWarmupSeconds = 3.0;
constexpr double kWindowSeconds = 5.0;
constexpr uint32_t kR2Windows = 14;
constexpr double kWindowSettleSeconds = 0.5;
constexpr uint32_t kBenchWindows = 4;
constexpr double kMainThreadMs = 2.0;
constexpr double kFrameMs = 16.7;

constexpr double kMinTwoCycleFraction = 0.25;
constexpr double kMinIdleMixFraction = 0.01;
constexpr double kMinMeanLocoClips = 1.25;
constexpr float kMinFrustumMargin = 0.10f;
}

}
