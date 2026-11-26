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
inline constexpr double kNeverPossessedSeconds = -1000.0;
inline constexpr float kUpsideDownUp = 0.01f;
inline constexpr float kChangeMaxAltitude = 8.0f;
inline constexpr float kGetOutUrgency = 10000.0f;
inline constexpr size_t kChangeStrengthCount = 6;
inline constexpr int kChangeInfantryStrength = 0;
inline constexpr int kChangeLightArmourStrength = 1;

using ChangeStrengths = std::array<float, kChangeStrengthCount>;

enum class ChangeUnitKind : uint8_t { Soldier, Ground, Air, Naval };

struct ChangeSeat {
	int occupant = -1;
	bool occupant_bot = false;
	bool same_order = true;
	int booked_by = -1;
	double possessed_s = kNeverPossessedSeconds;
	ChangeStrengths strengths{};
	bool armed = false;
	bool exposed = false;
	float min_range = 0.0f;
	float max_range = 0.0f;
	float aim_yaw_deg = 0.0f;
	float yaw_min_deg = -180.0f;
	float yaw_max_deg = 180.0f;
	float pitch_min_deg = -90.0f;
	float pitch_max_deg = 90.0f;

	bool operator==(const ChangeSeat &) const = default;
};

struct ChangeSensed {
	Vec3 position{};
	int strength_type = kChangeInfantryStrength;
	ChangeStrengths strengths{};
	bool sided = false;

	bool operator==(const ChangeSensed &) const = default;
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
	int strength_type = kChangeLightArmourStrength;
	Vec3 right{ 1.0f, 0.0f, 0.0f };
	Vec3 hull_up{ 0.0f, 1.0f, 0.0f };
	Vec3 forward{ 0.0f, 0.0f, 1.0f };
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
	float bail_value = 0.3f;
	bool stuck = false;
	int vehicle = -1;
	uint8_t seat = 0;
	ChangeStrengths strengths{};
	ChangeStrengths enemy_strengths{};
	ChangeStrengths enemy_types{};
	std::span<const ChangeSensed> sensed;
	std::span<const ChangeSensed> known;
	std::span<const Vec3> neighbour_offsets;
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

bool change_camera_valid(const ChangeVehicle &p_vehicle, uint8_t p_seat, const Vec3 &p_direction);

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
