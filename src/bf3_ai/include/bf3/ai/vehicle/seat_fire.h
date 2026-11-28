// SOURCE: OpenBF2 Code/BF2/AI/AIDLL/AI/AIBotBehaviour/BBFire.h @c47317b
#pragma once

#include "bf3/ai/knowledge/ai_object.h"
#include "bf3/ai/main/ai_script.h"
#include "bf3/ai/vehicle/vehicle_change.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace bf3::ai {

// SOURCE: OpenBF2 Code/BF2/AI/AIDLL/AI/AIBotBehaviour/BBFire.cpp @c47317b, calculateUrgency's constants below.
inline constexpr float kSeatFireEnemyStrengthTweak = 2.0f;
inline constexpr float kSeatFireAreaFactor = 0.5f;
inline constexpr float kSeatFireOutsideAreaFactor = 0.85f;
inline constexpr float kSeatFireActiveWeapon = 1.03f;
inline constexpr float kSeatFireRangeSlack = 1.2f;
inline constexpr float kSeatFireMinRangeShare = 0.1f;
inline constexpr float kSeatFireHarmless = 0.33f;
inline constexpr float kSeatFireKeepTarget = 1.2f;
inline constexpr float kSeatFireApproachSeconds = 5.0f;
inline constexpr float kSeatFireFasterTarget = 5.0f;
inline constexpr float kSeatFireOutOfRangeApproach = 0.25f;
inline constexpr float kSeatFireAttackFade = 30.0f;
inline constexpr float kSeatFireAttackPeak = 1.5f;
inline constexpr float kSeatFireHumanAttack = 3.0f;
inline constexpr size_t kMaxSeatFireWeapons = 8;

struct SeatFireWeapon {
	std::array<int32_t, kCombatStrengthTypeCount> strengths{};
	float min_range = 0.0f;
	float max_range = 0.0f;
	int32_t ammo = -1;
	bool active = false;
};

struct SeatFirePart {
	ChangeStrengths battle{};
	float strength = 0.0f;
};

struct SeatFireTarget {
	int id = -1;
	Vec3 position{};
	Vec3 velocity{};
	float max_speed = 0.0f;
	int strength_type = kChangeInfantryStrength;
	float health = 0.0f;
	bool soldier = true;
	bool mobile = true;
	bool in_view = true;
	bool in_area = true;
	bool attacking = false;
	double attack_age_s = 0.0;
	bool human = false;
	std::array<SeatFirePart, kMaxChangeSeats> parts{};
	uint8_t part_count = 0;
};

struct SeatFireBot {
	Vec3 position{};
	Vec3 velocity{};
	int strength_type = kChangeInfantryStrength;
	ChangeStrengths battle{};
	bool exposed_soldier = false;
	int soldier_strength_type = kChangeInfantryStrength;
	bool mobile = false;
	float max_speed = 0.0f;
	bool has_area = false;
	bool inside_area = true;
	int current_target = -1;
};

struct SeatFireChoice {
	int target = -1;
	int weapon = -1;
	float score = 0.0f;
	float urgency = 0.0f;
	float distance = 0.0f;
	bool kept = false;
};

SeatFireChoice choose_seat_fire(
		const SeatFireBot &p_bot, std::span<const SeatFireWeapon> p_weapons, std::span<const SeatFireTarget> p_targets);

// SOURCE: OpenBF2 Code/BF2/AI/AIDLL/AI/AIBotActionPlan/BAPCConPrecision.cpp @c47317b, the constants below.
inline constexpr float kMinFirePrecision = 0.1f;
inline constexpr float kPrecisionAngleMetres = 50.0f;
inline constexpr float kPrecisionVerticalShare = 0.2f;
inline constexpr float kPrecisionNegativeYLimit = 0.1f;
inline constexpr float kPrecisionCloseHeight = 5.0f;
inline constexpr float kPrecisionCloseMetres = 15.0f;

bool fire_precise(const Vec3 &p_barrel, const Vec3 &p_aim, float p_distance, float p_precision, bool p_close_vehicle);

}
