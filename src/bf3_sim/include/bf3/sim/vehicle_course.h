#pragma once

#include "bf3/core/result.h"
#include "bf3/sim/sim_types.h"
#include "bf3/sim/vehicle_types.h"
#include "bf3/sim/vehicle_world.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::sim {

class CollisionWorld;

enum class VehicleCourseKind : uint8_t { Straight = 0, Brake = 1, Slalom = 2, Circle = 3, Bumps = 4, Collision = 5, SlopeIdle = 6 };
inline constexpr size_t kVehicleCourseKindCount = 7;
const char *vehicle_course_name(VehicleCourseKind p_kind);

struct VehicleCourse {
	VehicleCourseKind kind = VehicleCourseKind::Straight;
	bool found = false;
	VehiclePlacement start;
	Vec3 a{}, b{};
	float slope_degrees = 0;
	float feature = 0;
	uint32_t seed = 0;
	size_t queries = 0;
	bool budget_exhausted = false;
};

struct VehicleCourseSearch {

	float radius = 600.0f;
	float grid = 4.0f;
	float max_flat_degrees = 4.0f;

	size_t max_queries_per_kind = 400000;

	float turnaround_radius = 20.0f;
	float max_turnaround_degrees = 10.0f;
};

core::Result<std::array<VehicleCourse, kVehicleCourseKindCount>> find_vehicle_courses(const CollisionWorld &p_world,
		std::span<const Vec3> p_seeds, const VehicleModel &p_model, const VehicleCourseSearch &p_search = {});

std::array<VehicleCourse, kVehicleCourseKindCount> synthetic_vehicle_courses(const SyntheticVehicleGround &p_ground = {});

std::array<VehicleCourse, 3> synthetic_vehicle_group(const SyntheticVehicleGround &p_ground = {});

struct VehicleScriptMemory {
	uint8_t phase = 0;
	uint8_t leg = 0;
	uint32_t phase_ticks = 0;
	float measured[4] = {};
};

VehicleCommand drive_vehicle_course(const VehicleCourse &p_course, const VehicleModel &p_model, const VehicleState &p_state, Tick p_tick,
		VehicleScriptMemory &r_memory);

struct VehicleCommandLog {
	std::vector<VehicleCommand> commands;
};

inline constexpr size_t kMaxVehicleLogCommands = 1u << 20;

std::string format_vehicle_command_log(const VehicleCommandLog &p_log);

core::Result<VehicleCommandLog> parse_vehicle_command_log(std::string_view p_text, size_t p_max_commands = kMaxVehicleLogCommands);

}
