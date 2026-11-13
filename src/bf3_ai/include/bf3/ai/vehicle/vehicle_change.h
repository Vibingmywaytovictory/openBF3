#pragma once

#include "bf3/ai/knowledge/ai_object.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace bf3::ai {

inline constexpr size_t kMaxChangeSeats = 8;
inline constexpr float kHardwareRadius = 75.0f;
inline constexpr double kPossessionWindowSeconds = 15.0;
inline constexpr float kUpsideDownUp = 0.01f;
inline constexpr float kChangeMaxAltitude = 8.0f;
inline constexpr float kGetOutUrgency = 10000.0f;

enum class ChangeUnitKind : uint8_t { Soldier, Ground, Air, Naval };

struct ChangeSeat {
	int occupant = -1;
	bool occupant_bot = false;
	bool same_order = true;
	int booked_by = -1;

	bool operator==(const ChangeSeat &) const = default;
};

struct ChangeVehicle {
	ChangeUnitKind kind = ChangeUnitKind::Ground;
	int side = 0;
	Vec3 position{};
	float up = 1.0f;
	float radius = 0.0f;
	float max_speed = 0.0f;
	float health = 1.0f;
	float altitude = 0.0f;
	bool on_nav = true;
	float nav_gap = 0.0f;
	double possessed_s = -1.0e9;
	uint8_t seats = 0;
	std::array<ChangeSeat, kMaxChangeSeats> seat{};

	bool operator==(const ChangeVehicle &) const = default;
};

struct ChangeBot {
	int player_id = -1;
	int side = 0;
	Vec3 position{};
	float radius = 0.0f;
	float max_speed = 0.0f;
	float health = 1.0f;
	double now_s = 0.0;
	double spawned_s = 0.0;
	float bail_value = 0.3f;
	bool stuck = false;
	int vehicle = -1;
	uint8_t seat = 0;
};

struct ChangeTarget {
	int vehicle = -1;
	uint8_t seat = 0;

	bool soldier() const { return vehicle < 0; }
	bool operator==(const ChangeTarget &) const = default;
};

struct ChangeChoice {
	ChangeTarget target;
	bool change = false;
	bool same_vehicle = false;
	bool get_out = false;
	float best = 0.0f;
	float current = 0.0f;
	float urgency = 0.0f;
};

float change_unit_urgency(
		const ChangeBot &p_bot, std::span<const ChangeVehicle> p_vehicles, ChangeTarget p_unit, bool p_other);

float change_driver_modifier(const ChangeVehicle &p_vehicle, bool p_current, double p_now_s);

ChangeChoice choose_change(const ChangeBot &p_bot, std::span<const ChangeVehicle> p_vehicles);

enum class ChangeStep : uint8_t { None, Approach, Enter, SelectSeat, GetOut, Hold };

struct ChangeOrder {
	ChangeStep step = ChangeStep::None;
	Vec3 goal{};
	uint8_t seat = 0;
};

ChangeOrder change_plan_step(
		const ChangeBot &p_bot, std::span<const ChangeVehicle> p_vehicles, const ChangeTarget &p_target);

}
