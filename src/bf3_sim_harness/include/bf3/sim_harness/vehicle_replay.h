#pragma once

#include "bf3/core/result.h"
#include "bf3/sim/sim_types.h"
#include "bf3/sim/static_world.h"
#include "bf3/sim/vehicle_ghost.h"
#include "bf3/sim/vehicle_model.h"
#include "bf3/sim/vehicle_types.h"
#include "bf3/sim_harness/vehicle_course.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace bf3::sim {

inline constexpr uint32_t kSettleTicks = 120;

inline constexpr VehicleCommand kSettleCommand{ 0, 0, 0, 255, 0, 0, 0 };

inline constexpr uint32_t kG1Ticks = 3600;
inline constexpr float kG2MaxError = 0.02f;

inline constexpr float kG2xMaxError = 0.02f;
inline constexpr uint32_t kG2SnapshotInterval = 2;
inline constexpr std::array<uint32_t, 2> kG2ReplayTicks{ 6, 9 };

enum class VehicleRestoreMode : uint8_t { OwnHistory = 0, Invalidate = 1, GhostLambdas = 2, ExactPod = 3, PodOwnLambdas = 4, Full = 5 };
inline constexpr size_t kVehicleRestoreModeCount = 6;

const char *vehicle_restore_mode_name(VehicleRestoreMode p_mode);

struct VehicleReplayOptions {
	uint32_t ticks = kG1Ticks;

	std::vector<uint32_t> restore_ticks{ 1, 7, 600, 1234, 1800, 2999, 3500 };
	uint32_t random_restores = 8;
	uint32_t replay_length = 600;
	uint64_t seed = 0x5332u;

	uint32_t snapshot_interval = kG2SnapshotInterval;
	std::vector<uint32_t> replay_ticks{ kG2ReplayTicks.begin(), kG2ReplayTicks.end() };
	uint32_t modes = 0x3Fu;
	VehicleGhostQuantisation quantisation = kDefaultVehicleGhost;
	bool g1 = true, g2 = true;
	bool keep_hashes = true;
};

struct VehicleDivergence {
	Tick tick = 0;

	std::string field;
};

struct VehicleErrorStats {
	size_t samples = 0;
	float max = 0;
	float p99 = 0;
	float mean = 0;
	Tick worst_tick = 0;
};

struct VehicleCorrectionReport {
	VehicleRestoreMode mode = VehicleRestoreMode::OwnHistory;
	uint32_t replay_ticks = 0;
	VehicleErrorStats all, wheel_contact, static_collision;
	float max_orientation_degrees = 0;
	float max_velocity_error = 0;

	uint32_t diverged_bases = 0;
};

struct VehicleCourseReport {
	VehicleCourse course;
	uint32_t ticks = 0;

	uint32_t g1_ticks = 0;
	uint32_t g1_mismatches = 0;
	VehicleDivergence g1_first;
	uint32_t replay_runs = 0;
	uint32_t replay_ticks = 0;
	uint32_t replay_mismatches = 0;
	VehicleDivergence replay_first;

	std::vector<VehicleCorrectionReport> corrections;

	float distance = 0;
	float max_speed_kmh = 0;
	uint32_t ticks_all_wheels_off = 0;
	uint32_t ticks_any_wheel_off = 0;
	uint32_t ticks_static_contact = 0;
	uint32_t static_impacts = 0;
	float max_impact_speed_kmh = 0;
	uint32_t ticks_asleep = 0;
	uint32_t yaw_rate_sign_changes = 0;
	float yaw_turns = 0;
	float creep = 0;
	bool drove = false;
	std::string drove_why;
	size_t snapshot_bytes = 0;
	uint64_t steps = 0;
	double seconds = 0;

	std::vector<uint64_t> server_hashes;

	VehicleCommandLog log;
};

core::Result<VehicleCourseReport> run_vehicle_course(std::shared_ptr<const StaticWorldShapes> p_shapes, std::shared_ptr<const VehicleModel> p_model,
		const VehicleCourse &p_course, const VehicleReplayOptions &p_options = {});

struct VehicleActivityThresholds {
	float min_distance = 0;
	float min_max_speed_kmh = 0;
	uint32_t min_yaw_rate_sign_changes = 0;
	float min_yaw_turns = 0;
	uint32_t min_ticks_any_wheel_off = 0;
	uint32_t min_static_impacts = 0;
	uint32_t min_ticks_static_contact = 0;
	float max_creep = 0;
	bool must_stay_awake = true;
};
inline constexpr std::array<VehicleActivityThresholds, kVehicleCourseKindCount> kVehicleActivity{ {
		{ 300.0f, 60.0f, 0, 0.0f, 0, 0, 0, 0.0f, true },
		{ 300.0f, 55.0f, 0, 0.0f, 0, 0, 0, 0.0f, true },
		{ 300.0f, 30.0f, 20, 0.0f, 0, 0, 0, 0.0f, true },
		{ 0.0f, 0.0f, 0, 3.0f, 0, 0, 0, 0.0f, true },
		{ 300.0f, 45.0f, 0, 0.0f, 30, 0, 0, 0.0f, true },
		{ 0.0f, 0.0f, 0, 0.0f, 0, 3, 30, 0.0f, true },
		{ 0.0f, 0.0f, 0, 0.0f, 0, 0, 0, 0.05f, false },
} };
bool vehicle_course_drove(const VehicleCourseReport &p_report, std::string *r_why = nullptr);

bool vehicle_gate_courses(const std::vector<VehicleCourseReport> &p_reports, std::string *r_why = nullptr);
bool vehicle_gate_drove(const std::vector<VehicleCourseReport> &p_reports, std::string *r_why = nullptr);
bool vehicle_gate_g1(const std::vector<VehicleCourseReport> &p_reports, std::string *r_why = nullptr);
bool vehicle_gate_g2(const std::vector<VehicleCourseReport> &p_reports, std::string *r_why = nullptr);
bool vehicle_gate_g2x(const std::vector<VehicleCourseReport> &p_reports, std::string *r_why = nullptr);

struct VehicleGroupReport {
	uint32_t vehicles = 0;
	uint32_t ticks = 0;
	uint32_t g1_ticks = 0;
	uint32_t g1_mismatches = 0;
	VehicleDivergence g1_first;
	uint32_t replay_runs = 0;
	uint32_t replay_ticks = 0;
	uint32_t replay_mismatches = 0;
	VehicleDivergence replay_first;
	uint32_t ticks_vehicle_contact = 0;
	uint32_t slot0_ticks_asleep = 0;
	std::vector<float> slot_distance;
	uint64_t steps = 0;
	double seconds = 0;
};

core::Result<VehicleGroupReport> run_vehicle_group(std::shared_ptr<const StaticWorldShapes> p_shapes, std::shared_ptr<const VehicleModel> p_model,
		std::span<const VehicleCourse> p_courses, const VehicleReplayOptions &p_options = {});
bool vehicle_gate_g1m(const VehicleGroupReport &p_report, std::string *r_why = nullptr);

std::string format_vehicle_course_report(const VehicleCourseReport &p_report);
std::string format_vehicle_group_report(const VehicleGroupReport &p_report);

struct VehicleHandling {
	float time_0_60_kmh = 0;
	float speed_at_6s_kmh = 0;
	float brake_distance_60_m = 0;
	float brake_deceleration = 0;
	float turning_circle_m = 0;
	float turning_circle_com_m = 0;
	float top_speed_kmh = 0;
};

core::Result<VehicleHandling> measure_vehicle_handling(std::shared_ptr<const VehicleModel> p_model);

inline constexpr float kSettleDriftLimit = 0.01f;
inline constexpr uint32_t kSettleDriftTicks = 60;

struct VehicleSettleReport {
	Vec3 spawn{};
	float drop = 0;
	float drift_y = 0;
	float drift = 0;
	uint8_t wheels_in_contact = 0;
	bool settled = false;
};

core::Result<std::vector<VehicleSettleReport>> measure_vehicle_settle(std::shared_ptr<const StaticWorldShapes> p_shapes,
		std::shared_ptr<const VehicleModel> p_model, std::span<const VehiclePlacement> p_placements);

}
