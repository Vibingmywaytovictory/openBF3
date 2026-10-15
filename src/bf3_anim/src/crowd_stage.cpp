// The S3 staged crowd: schedule, inputs and blend coverage (docs/design/s3.md §5.8). See crowd_stage.h.
#include "bf3/anim/crowd_stage.h"

#include <algorithm>
#include <cmath>

namespace bf3::anim {

namespace {

constexpr double kOffsetSeconds = 0.731; // soldier i starts its schedule i * 0.731 s in
constexpr float kFacingStepDeg = 37.0f; // soldier i faces heading + 180 deg + i * 37 deg
constexpr float kDeg = 0.0174532925f;
constexpr float kPi = 3.14159265f;

struct Target {
	Vec3 local{}; // +z forward, +x the soldier's left (Y up, right-handed; see the header)
	float pitch = 0.0f; // radians
	float facing_turn = 0.0f; // radians the facing has turned since the phase start (RunCircle)
};

Vec3 dir_local(float p_deg, float p_speed) {
	// p_deg measured from forward (+z) towards the left (+x).
	return Vec3{ std::sin(p_deg * kDeg) * p_speed, 0.0f, std::cos(p_deg * kDeg) * p_speed };
}

// The unramped target of phase p_phase at p_u seconds into it.
Target target_of(const CrowdStageSpec &p_spec, StagePhase p_phase, float p_u) {
	Target t;
	const float side = p_spec.side_factor * p_spec.run_speed;
	switch (p_phase) {
		case StagePhase::Idle:
			break;
		case StagePhase::RunFwd:
			t.local = dir_local(0.0f, p_spec.run_speed);
			break;
		case StagePhase::RunBwd:
			t.local = dir_local(180.0f, side);
			break;
		case StagePhase::RunLeft:
			t.local = dir_local(90.0f, side);
			break;
		case StagePhase::RunRight:
			t.local = dir_local(-90.0f, side);
			break;
		case StagePhase::RunDiagonal:
			t.local = p_u < p_spec.phase_seconds * 0.5f ? dir_local(45.0f, p_spec.circle_speed) : dir_local(-135.0f, p_spec.circle_speed);
			break;
		case StagePhase::RunCircle: {
			// The world velocity holds while the facing turns: in the soldier's frame the direction turns the other way.
			t.facing_turn = p_spec.circle_turn_deg_s * kDeg * p_u;
			t.local = dir_local(-p_spec.circle_turn_deg_s * p_u, p_spec.circle_speed);
			break;
		}
		case StagePhase::Reload:
			t.local = dir_local(-45.0f, p_spec.circle_speed);
			break;
		case StagePhase::AimUp:
			t.pitch = p_spec.aim_pitch_deg * kDeg;
			break;
		case StagePhase::AimDown:
			t.local = dir_local(135.0f, p_spec.circle_speed);
			t.pitch = -p_spec.aim_pitch_deg * kDeg;
			break;
		case StagePhase::Count:
			break;
	}
	return t;
}

Vec3 lerp3(const Vec3 &p_a, const Vec3 &p_b, float p_t) {
	return Vec3{ p_a.x + (p_b.x - p_a.x) * p_t, p_a.y + (p_b.y - p_a.y) * p_t, p_a.z + (p_b.z - p_a.z) * p_t };
}

struct Schedule {
	StagePhase phase = StagePhase::Idle;
	float u = 0.0f; // seconds into the phase
	double soldier_t = 0.0; // the soldier's own schedule time
};

Schedule schedule_of(const CrowdStageSpec &p_spec, uint32_t p_i, double p_t) {
	Schedule s;
	const double phase_s = p_spec.phase_seconds > 0.0f ? double(p_spec.phase_seconds) : 4.0;
	const double period = phase_s * double(kStagePhases);
	double t = (std::isfinite(p_t) ? p_t : 0.0) + double(p_i) * kOffsetSeconds;
	s.soldier_t = t;
	t = std::fmod(t, period);
	if (t < 0.0) {
		t += period;
	}
	const size_t k = std::min(size_t(t / phase_s), kStagePhases - 1);
	s.phase = StagePhase(k);
	s.u = float(t - double(k) * phase_s);
	return s;
}

// The ramped local velocity, pitch and facing turn of a schedule point.
Target ramped(const CrowdStageSpec &p_spec, const Schedule &p_s) {
	const float ramp = p_spec.ramp_seconds > 0.0f ? p_spec.ramp_seconds : 0.0f;
	Target t = target_of(p_spec, p_s.phase, p_s.u);
	const StagePhase prev = StagePhase((size_t(p_s.phase) + kStagePhases - 1) % kStagePhases);
	if (ramp > 0.0f && p_s.u < ramp) {
		const Target from = target_of(p_spec, prev, p_spec.phase_seconds);
		const float a = p_s.u / ramp;
		t.local = lerp3(from.local, t.local, a);
		t.pitch = from.pitch + (t.pitch - from.pitch) * a;
	}
	const float half = p_spec.phase_seconds * 0.5f;
	if (p_s.phase == StagePhase::RunDiagonal && ramp > 0.0f && p_s.u >= half && p_s.u < half + ramp) {
		const Target from = target_of(p_spec, StagePhase::RunDiagonal, half - 1.0e-3f);
		t.local = lerp3(from.local, t.local, (p_s.u - half) / ramp);
	}
	return t;
}

} // namespace

StagePhase stage_phase(const CrowdStageSpec &p_spec, uint32_t p_i, double p_t) {
	return schedule_of(p_spec, p_i, p_t).phase;
}

CrowdSoldierInput stage_input(const CrowdStageSpec &p_spec, uint32_t p_i, double p_t, double p_prev_t, const Vec3 &p_camera, bool p_lod_spread) {
	CrowdSoldierInput in;
	const uint32_t cols = p_spec.cols == 0 ? 1 : p_spec.cols;
	const uint32_t row = p_i / cols, col = p_i % cols;
	in.team = col < cols / 2 ? 0 : 1;
	in.position = stage_cell(p_spec, p_i);
	const Schedule s = schedule_of(p_spec, p_i, p_t);
	const Target t = ramped(p_spec, s);
	const float base_yaw = p_spec.heading + kPi + float(p_i) * kFacingStepDeg * kDeg;
	in.yaw = std::remainder(base_yaw + t.facing_turn, 2.0f * kPi);
	// World velocity = rotate_y(yaw) * local (sim yaw: forward = (sin yaw, 0, cos yaw)).
	const float cy = std::cos(in.yaw), sy = std::sin(in.yaw);
	in.velocity = Vec3{ t.local.x * cy + t.local.z * sy, 0.0f, -t.local.x * sy + t.local.z * cy };
	in.pitch = t.pitch;
	if (p_lod_spread) {
		in.camera_distance = kLodRowDistance[std::min<size_t>(row, kLodRowDistance.size() - 1)];
	} else {
		const float dx = in.position.x - p_camera.x, dy = in.position.y + 1.0f - p_camera.y, dz = in.position.z - p_camera.z;
		in.camera_distance = std::sqrt(dx * dx + dy * dy + dz * dz);
	}
	// kReloadStart on the first frame at or after a Reload phase start: prev < start <= t (soldier time).
	if (std::isfinite(p_prev_t) && std::isfinite(p_t) && p_prev_t < p_t) {
		const double phase_s = p_spec.phase_seconds > 0.0f ? double(p_spec.phase_seconds) : 4.0;
		const double period = phase_s * double(kStagePhases);
		const double offset = double(size_t(StagePhase::Reload)) * phase_s;
		const double now = s.soldier_t;
		const double before = p_prev_t + double(p_i) * kOffsetSeconds;
		const double k = std::floor((now - offset) / period);
		const double start = offset + k * period;
		if (start > before && start <= now) {
			in.flags |= crowd_flags::kReloadStart;
		}
	}
	return in;
}

StageCoverage stage_coverage(const CrowdStageSpec &p_spec, const std::array<LocoClipInfo, kLocoClips> &p_clips, const LocoParams &p_params, double p_step) {
	StageCoverage c;
	const double period = double(p_spec.phase_seconds) * double(kStagePhases);
	if (!(p_step > 0.0) || !(period > 0.0) || period / p_step > 1.0e7) {
		return c;
	}
	uint64_t samples = 0, two = 0, idle_mix = 0, clips = 0;
	const uint32_t n = p_spec.soldiers();
	for (double t = 0.0; t < period; t += p_step) {
		for (uint32_t i = 0; i < n; ++i) {
			const CrowdSoldierInput in = stage_input(p_spec, i, t, t, Vec3{}, false);
			const float cy = std::cos(in.yaw), sy = std::sin(in.yaw);
			const Vec3 local{ in.velocity.x * cy - in.velocity.z * sy, 0.0f, in.velocity.x * sy + in.velocity.z * cy };
			const LocoWeights w = locomotion_weights(p_clips, local, p_params);
			uint32_t cycles = 0, weighted = 0;
			for (size_t k = 0; k < kLocoClips; ++k) {
				if (w.weight[k] > 0.0f) {
					++weighted;
					cycles += k != size_t(LocoClip::Idle) ? 1 : 0;
				}
			}
			++samples;
			two += cycles >= 2 ? 1 : 0;
			idle_mix += (w.weight[size_t(LocoClip::Idle)] > 0.0f && cycles >= 1) ? 1 : 0;
			clips += weighted;
		}
	}
	if (samples > 0) {
		c.two_cycle_fraction = double(two) / double(samples);
		c.idle_mix_fraction = double(idle_mix) / double(samples);
		c.mean_loco_clips = double(clips) / double(samples);
	}
	return c;
}

} // namespace bf3::anim
