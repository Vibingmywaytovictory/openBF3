#pragma once
// The S3 staged crowd: 64 soldiers on a grid, driven by a deterministic schedule, plus the gate thresholds
// (docs/design/s3.md §1, §5.7). Godot-free, so the schedule is unit-tested and the Godot check and the
// install-backed ctest drive the same soldiers.
//
// Why a stage and not the server: M1 clamps --walkers to 16 (level_session.cpp parse_args), and its walkers
// follow courses that leave the camera. The gate needs 64 soldiers in view at known distances. The stage
// therefore feeds CrowdSoldierInput directly (the same struct present_others output is turned into), at the
// spawn of the session's own soldier on Metro.
//
// Schedule: soldier i (row r = i / cols, column c = i % cols; team = c < cols / 2 ? 0 : 1) at
// origin + r * spacing forward + (c - (cols - 1) / 2) * spacing right, facing the camera's opposite heading
// plus i * 37 deg. It cycles through the phases below with a per-soldier offset (i * 0.731 s), so at any time
// every phase is on screen. Velocities are the M1 motor's: forward 4.0 m/s, back and strafe 0.75 x 4.0.
// The phases do not move the soldier: the grid position stays fixed and only the animation sees the velocity
// (treadmill), so the soldiers never leave the view or collide.

#include "bf3/anim/crowd_eval.h"
#include "bf3/anim/pose_math.h"

#include <cstdint>

namespace bf3::anim {

enum class StagePhase : uint8_t { Idle, RunFwd, RunBwd, RunLeft, RunRight, Reload, AimUp, AimDown, Count };
constexpr size_t kStagePhases = size_t(StagePhase::Count);

struct CrowdStageSpec {
	uint32_t rows = 8;
	uint32_t cols = 8;
	float spacing = 2.0f; // metres between grid cells
	float phase_seconds = 4.0f; // each phase lasts this long (the AK-74M reload, 3.6 s at rate 1, fits)
	float run_speed = 4.0f; // M1 motor forward speed (m/s)
	float side_factor = 0.75f; // back and strafe speed factor (M1 motor)
	float aim_pitch_deg = 45.0f; // AimUp / AimDown phases
	Vec3 origin{}; // grid front-centre, feet height (from the session's own spawn)
	float heading = 0.0f; // yaw the grid extends along (radians, sim convention)

	constexpr uint32_t soldiers() const { return rows * cols; }
};

// Phase of soldier p_i at stage time p_t (seconds since the stage started).
StagePhase stage_phase(const CrowdStageSpec &p_spec, uint32_t p_i, double p_t);
// Input of soldier p_i at stage time p_t. kReloadStart is set on the first frame of a Reload phase:
// p_prev_t < phase start <= p_t.
CrowdSoldierInput stage_input(const CrowdStageSpec &p_spec, uint32_t p_i, double p_t, double p_prev_t, const Vec3 &p_camera);

// Gate thresholds (s3.md §1). One place, so the Godot check, the README and the ctest wrapper cannot disagree.
namespace s3_gate {
constexpr uint32_t kSoldiers = 64;
constexpr uint32_t kWidth = 2560; // window client size the gate runs at
constexpr uint32_t kHeight = 1440;
constexpr double kMinSeconds = 30.0; // per measured run, after warm-up
constexpr double kWarmupSeconds = 3.0;
constexpr double kMainThreadMs = 2.0; // G1: p50 and p99 of the per-frame main-thread animation + skeleton timer
constexpr double kFrameMs = 16.7; // G2: p50 and p99 of the frame time, vsync off and on
} // namespace s3_gate

} // namespace bf3::anim
