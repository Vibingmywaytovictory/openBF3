#pragma once

#include "bf3/core/math.h"
#include "bf3/core/result.h"
#include "bf3/sim/vehicles/turret_rules.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace bf3::core::gameplay {
struct VehicleDef;
}

namespace bf3::sim::vehicles {

inline constexpr size_t kMaxArmamentTurrets = 16;
inline constexpr size_t kMaxArmamentWeapons = 16;
inline constexpr size_t kMaxArmamentSeats = 8;
inline constexpr float kTurretStepSeconds = 1.0f / 30.0f;
inline constexpr uint32_t kTurretTickDivisor = 2;

enum class TurretAimAxis : uint8_t { None = 0, Yaw = 1, Pitch = 2 };

struct ArmamentTurret {
	RotationBodyRule rule;
	int8_t parent = -1;
	int8_t frame_turret = -1;
	int8_t seat = -1;
	TurretAimAxis aim = TurretAimAxis::None;
	uint8_t rule_axis = 1;
	core::Transform frame;
};

struct ArmamentWeapon {
	std::string firing;
	int8_t seat = -1;
	int8_t turret = -1;
	int8_t frame_turret = -1;
	core::Transform frame;
	core::Vec3 spawn_offset{};
	float impulse_strength = 0;
	float reload_time_multiplier = 1;
	float overheat_drop_multiplier = 1;
	uint8_t stance_mask = 0xFF;
};

struct VehicleArmament {
	std::vector<ArmamentTurret> turrets;
	std::vector<ArmamentWeapon> weapons;
	std::vector<uint8_t> seat_stances;
};

core::Result<VehicleArmament> build_vehicle_armament(const core::gameplay::VehicleDef &p_def);

struct SeatAim {
	bool active = false;
	core::Vec3 direction{ 0.0f, 0.0f, 1.0f };
};

struct ArmamentState {
	std::array<RotationBodyState, kMaxArmamentTurrets> bodies{};
	std::array<core::Vec3, kMaxArmamentTurrets> parent_reference{};
	std::array<uint8_t, kMaxArmamentTurrets> parent_known{};

	bool operator==(const ArmamentState &) const = default;
};

core::Transform hull_transform(const std::array<float, 4> &p_rotation, const core::Vec3 &p_origin);
float hull_up_y(const std::array<float, 4> &p_rotation);

core::Transform turret_world(const VehicleArmament &p_armament, const ArmamentState &p_state,
		const core::Transform &p_hull, size_t p_turret);

core::Transform weapon_world(const VehicleArmament &p_armament, const ArmamentState &p_state,
		const core::Transform &p_hull, size_t p_weapon);

core::Vec3 weapon_muzzle(const VehicleArmament &p_armament, const ArmamentState &p_state, const core::Transform &p_hull,
		size_t p_weapon);

float aim_error(
		const ArmamentTurret &p_turret, float p_angle, const core::Transform &p_rest, const core::Vec3 &p_direction);

float aim_level(const ArmamentTurret &p_turret, float p_error);

void step_armament(const VehicleArmament &p_armament, ArmamentState &r_state, const core::Transform &p_hull,
		std::span<const SeatAim> p_seats);

bool weapon_in_stance(const ArmamentWeapon &p_weapon, uint8_t p_stance);

}
