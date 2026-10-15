#pragma once
// The S3 staged crowd: 64 soldiers on a fixed grid on Metro, driven by a deterministic schedule, its camera, and
// the gate thresholds (docs/design/s3.md §1, §5.8). Godot-free, so the schedule and the framing are unit-tested and
// the Godot check and the install-backed ctest drive the same soldiers.
//
// Why a stage and not the server: M1 clamps --walkers to 16 (level_session.cpp parse_args), and its walkers
// follow courses that leave the camera. The gate needs 64 soldiers in view at known distances. The stage
// therefore feeds CrowdSoldierInput directly (the same struct present_others output is turned into later).
//
// Place [verified 2026-10-15 with a scratch probe over the M1 static world of MP_Subway TeamDeathMatch0 through
// bf3_sim::CollisionWorld; s3.md §4.5, §5.8]: a fixed origin (kMetroStageOrigin) on a flat floor at y 64.40 in the
// TDM area, grid heading +X (sim yaw 90 deg). There, all 64 cells have floor within 0.3 m, no 0.45 m x 1.85 m
// capsule overlaps the Collision layer, the camera point is clear, and 6 rays from the camera to each soldier
// (0.2 / 1.0 / 1.8 m high, +-0.4 m to the sides) are unoccluded on Collision + Raycast. Every origin within 1 m
// passes the same test at this heading. A spawn-relative stage does not work: 0 of 408 spawn x heading candidates
// pass at the spawn point, and 2 of 19,992 within +-12 m of a spawn (about 14 m off spawns 31 and 35).
//
// Framing: the camera sits camera_back (9.5 m) behind the front row and camera_up (3 m) above the floor on the
// grid's centre line, aimed at the grid centre at target_height, vertical FOV camera_fov_deg (60, Camera3D
// KEEP_HEIGHT). At 16:9 every corner of every soldier's box (+-0.7 m, 0..1.9 m) lies inside the frustum with
// 15.4 % of the half extent to spare (stage_frustum_margin; s3.contract requires >= 10 %). The facing turns a full
// 360 deg in RunCircle and ends where it started. The Godot check re-tests the same in game
// (Camera3D::is_position_in_frustum on the box corners; rays against its own CollisionWorld) and refuses to
// measure when fewer than 64 soldiers are in view (gate G0).
//
// Schedule (s3.md §5.8): soldier i (row r = i / cols, column c = i % cols; team = c < cols / 2 ? 0 : 1) faces
// the grid heading + 180 deg + i * 37 deg and cycles through the phases below, phase_seconds each, offset by
// i * 0.731 s, so every phase is on screen at once. Each phase has a target LOCAL velocity (in the soldier's
// frame: +z forward, +x to its left, Y up, right-handed); at each phase start (and at RunDiagonal's half) the
// local velocity ramps linearly over ramp_seconds, so the run blend crosses direction brackets and the idle fade
// (0.2..1 m/s) in game, not only in unit tests:
//   Idle        0
//   RunFwd      run_speed forward
//   RunBwd      side_factor * run_speed back
//   RunLeft     side_factor * run_speed left
//   RunRight    side_factor * run_speed right
//   RunDiagonal circle_speed at 45 deg (forward-left) for the first half, 135 deg (back-right) for the second
//   RunCircle   circle_speed with the soldier's facing turning circle_turn_deg_s while the world velocity holds:
//               the local direction sweeps the full circle (every bracket, two cycles nearly everywhere)
//   Reload      the team's reload over a forward-right diagonal at circle_speed (legs blend under the mask)
//   AimUp       standing, pitch +aim_pitch_deg
//   AimDown     backward-left diagonal at circle_speed, pitch -aim_pitch_deg
// The grid positions stay fixed: only the animation sees the velocity (treadmill), so nobody leaves the view.
// The facing (root yaw) changes only in RunCircle, so the root transform rotates on screen there.

#include "bf3/anim/crowd_eval.h"
#include "bf3/anim/pose_math.h"

#include <array>
#include <cmath>
#include <cstdint>

namespace bf3::anim {

enum class StagePhase : uint8_t { Idle, RunFwd, RunBwd, RunLeft, RunRight, RunDiagonal, RunCircle, Reload, AimUp, AimDown, Count };
constexpr size_t kStagePhases = size_t(StagePhase::Count);

// The fixed stage on Metro (MP_Subway TeamDeathMatch0), see the header comment.
inline constexpr Vec3 kMetroStageOrigin{ -264.0f, 64.40f, -463.0f }; // grid front-centre, feet height
inline constexpr float kMetroStageHeading = 1.57079633f; // sim yaw: rows extend along +X

// Synthetic camera distances for the temporal-LOD run R3 (s3.md §5.7): row r reports kLodRowDistance[r] instead
// of its real distance (10..24 m, all tier First), so every GameAnimationSettings tier and the far policy run in
// game: below First, First (2), Second (30), ... Sixth (150), beyond far (208).
inline constexpr std::array<float, 8> kLodRowDistance{ 1.0f, 15.0f, 45.0f, 75.0f, 105.0f, 135.0f, 175.0f, 215.0f };

struct CrowdStageSpec {
	uint32_t rows = 8;
	uint32_t cols = 8;
	float spacing = 2.0f; // metres between grid cells
	float phase_seconds = 4.0f; // each phase lasts this long (the AK-74M reload, 3.6 s at rate 1, fits)
	float ramp_seconds = 1.0f; // local velocity ramp at each phase start
	float run_speed = 4.0f; // M1 motor forward speed (m/s)
	float side_factor = 0.75f; // back and strafe speed factor (M1 motor)
	float circle_speed = 3.5f; // RunDiagonal, RunCircle, Reload and AimDown speed (m/s)
	float circle_turn_deg_s = 90.0f; // RunCircle facing turn rate (one full sweep per phase)
	float aim_pitch_deg = 45.0f; // AimUp / AimDown phases
	Vec3 origin = kMetroStageOrigin;
	float heading = kMetroStageHeading; // yaw the rows extend along (radians, sim convention: yaw 0 looks down +Z)
	// Camera (s3.md §5.8).
	float camera_back = 9.5f; // metres behind the front row
	float camera_up = 3.0f; // metres above the floor
	float camera_fov_deg = 60.0f; // vertical (Camera3D::set_fov, KEEP_HEIGHT)
	float target_height = 0.9f; // aim point above the grid centre
	// A soldier's in-view box around its feet: +-box_half horizontally, 0..box_height (posed limbs and weapon).
	float box_half = 0.7f;
	float box_height = 1.9f;

	constexpr uint32_t soldiers() const { return rows * cols; }
};

struct StageCamera {
	Vec3 position{};
	Vec3 target{};
	float fov_y_deg = 60.0f;
};

// World feet position of grid cell p_i (no floor snap; the check snaps to the floor with a ray).
inline Vec3 stage_cell(const CrowdStageSpec &p_spec, uint32_t p_i) {
	const float fx = std::sin(p_spec.heading), fz = std::cos(p_spec.heading); // forward (rows)
	const float rx = fz, rz = -fx; // a horizontal perpendicular (columns); the grid is symmetric about the centre line
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

// Smallest frustum margin over every corner of every soldier box, as a fraction of the half extent at that depth
// (1 = on the view axis, 0 = on a side plane, < 0 = outside), for a symmetric perspective camera with vertical FOV
// p_camera.fov_y_deg and aspect p_aspect (width / height), looking from position to target with +Y up. Returns
// -1 for a degenerate camera or a box corner behind the near plane (0.05 m).
inline float stage_frustum_margin(const CrowdStageSpec &p_spec, const StageCamera &p_camera, float p_aspect) {
	const float tv = std::tan(p_camera.fov_y_deg * 0.5f * 0.0174532925f);
	const float th = tv * p_aspect;
	Vec3 f{ p_camera.target.x - p_camera.position.x, p_camera.target.y - p_camera.position.y, p_camera.target.z - p_camera.position.z };
	const float fl = std::sqrt(f.x * f.x + f.y * f.y + f.z * f.z);
	if (!(fl > 1.0e-4f) || !(tv > 0.0f) || !(th > 0.0f)) {
		return -1.0f;
	}
	f = Vec3{ f.x / fl, f.y / fl, f.z / fl };
	// right = up x f, then up' = f x right (any handedness: only |projections| are used)
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

// Phase of soldier p_i at stage time p_t (seconds since the stage started).
StagePhase stage_phase(const CrowdStageSpec &p_spec, uint32_t p_i, double p_t);
// Input of soldier p_i at stage time p_t. kReloadStart is set on the first frame of a Reload phase:
// p_prev_t < phase start <= p_t. camera_distance is the real distance to p_camera, or kLodRowDistance[row] when
// p_lod_spread (run R3).
CrowdSoldierInput stage_input(const CrowdStageSpec &p_spec, uint32_t p_i, double p_t, double p_prev_t, const Vec3 &p_camera, bool p_lod_spread);
// The schedule's own blend coverage over one full period, sampled every p_step seconds for every soldier
// (anim.s3.unit asserts it is above the s3_gate blend floors, so the in-game G0 blend row can only fail when
// the evaluator or the stage is broken).
struct StageCoverage {
	double two_cycle_fraction = 0.0; // soldier-samples with two direction cycles weighted
	double idle_mix_fraction = 0.0; // soldier-samples with idle and a cycle both weighted
	double mean_loco_clips = 0.0; // locomotion clips with weight > 0 per soldier-sample
};
StageCoverage stage_coverage(const CrowdStageSpec &p_spec, const std::array<LocoClipInfo, kLocoClips> &p_clips, const LocoParams &p_params, double p_step);

// Gate thresholds (s3.md §1). One place, so the Godot check, the README and the ctest wrapper cannot disagree.
namespace s3_gate {
constexpr uint32_t kSoldiers = 64;
constexpr uint32_t kInView = 64; // G0: soldiers whose box is inside the frustum and unoccluded, on every sampled frame
constexpr uint32_t kWidth = 2560; // window client size the gate runs at
constexpr uint32_t kHeight = 1440;
constexpr double kMinSeconds = 30.0; // per measured run, after warm-up
constexpr double kWarmupSeconds = 3.0;
constexpr double kWindowSeconds = 5.0; // R2 and the backend bench alternate animated / frozen windows of this length
constexpr uint32_t kR2Windows = 14; // 7 animated + 7 frozen: 31.5 s of each after the settle time
constexpr double kWindowSettleSeconds = 0.5; // discarded at the start of each window (the transition)
constexpr uint32_t kBenchWindows = 4; // per backend: 2 animated + 2 frozen
constexpr double kMainThreadMs = 2.0; // G1a: p50 and p99 of the per-frame step wall time; G1b: p50 step + hidden
constexpr double kFrameMs = 16.7; // G2: p50 and p99 of the frame time, vsync off and on
// G0 blend floors per run (the schedule gives more; stage_coverage).
constexpr double kMinTwoCycleFraction = 0.25;
constexpr double kMinIdleMixFraction = 0.01;
constexpr double kMinMeanLocoClips = 1.25;
constexpr float kMinFrustumMargin = 0.10f; // stage_frustum_margin at 16:9 (s3.contract)
} // namespace s3_gate

} // namespace bf3::anim
