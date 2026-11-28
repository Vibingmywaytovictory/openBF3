// SOURCE: OpenBF2 Code/BF2/AI/AIDLL/AI/AIBotBehaviour/BBFire.cpp @c47317b
#include "bf3/ai/vehicle/seat_fire.h"

#include "bf3/ai/behaviour/curves.h"
#include "bf3/ai/main/ai_settings.h"
#include "bf3/core/math.h"

#include <algorithm>
#include <cmath>

namespace bf3::ai {

namespace {

namespace vec = core::vec;

float attack_factor(const SeatFireTarget &p_target) {
	if (!p_target.attacking) {
		return 1.0f;
	}
	const float ramp = std::clamp(float(p_target.attack_age_s) / kSeatFireAttackFade, 0.0f, 1.0f);
	const float factor = kSeatFireAttackPeak - ramp * (kSeatFireAttackPeak - 1.0f);
	return p_target.human && p_target.soldier ? factor * kSeatFireHumanAttack : factor;
}

float closing_approach(const SeatFireBot &p_bot, const SeatFireTarget &p_target, const Vec3 &p_direction) {
	if (!(p_target.max_speed - p_bot.max_speed > kSeatFireFasterTarget)) {
		return kSeatFireOutOfRangeApproach;
	}
	const Vec3 up{ 0.0f, 1.0f, 0.0f };
	const Vec3 across = vec::cross(up, p_direction);
	const float towards =
			std::max(vec::dot(p_direction, p_target.velocity) - vec::dot(p_direction, p_bot.velocity), 0.0f);
	const float sideways = std::max(vec::dot(across, p_target.velocity) - vec::dot(across, p_bot.velocity), 0.0f);
	return kSeatFireOutOfRangeApproach /
			(towards + towards + up.y + (sideways + sideways) + vec::length(p_target.velocity) * 0.5f);
}

}

// STAND-IN: no fire feedback, unseen or air targets, material test, orders, soft-step draw or chooseWeapon; harmless at
// 0. Settle: port them with BotSenses, the orders and the bot skill draw.
SeatFireChoice choose_seat_fire(const SeatFireBot &p_bot, std::span<const SeatFireWeapon> p_weapons,
		std::span<const SeatFireTarget> p_targets) {
	SeatFireChoice out;
	const float area = p_bot.has_area && !p_bot.inside_area ? kSeatFireOutsideAreaFactor : kSeatFireAreaFactor;
	float current_score = 0.0f;
	bool keep = false;
	for (size_t t = 0; t < p_targets.size(); ++t) {
		const SeatFireTarget &target = p_targets[t];
		if (!(target.health > 0.0f) || target.part_count == 0) {
			continue;
		}
		if (!p_bot.mobile && !target.in_view) {
			continue;
		}
		Vec3 direction = vec::sub(target.position, p_bot.position);
		float distance = vec::length(direction);
		if (distance > 0.0001f) {
			direction = vec::scale(direction, 1.0f / distance);
		}
		distance = std::max(distance, 1.0f);

		float best_weapon_score = 0.0f;
		int best_weapon = -1;
		float max_range = 0.0f;
		for (size_t w = 0; w < p_weapons.size(); ++w) {
			const SeatFireWeapon &weapon = p_weapons[w];
			float score =
					weapon.ammo == -1 || weapon.ammo > 0 ? float(weapon.strengths[size_t(target.strength_type)]) : 0.0f;
			if (weapon.active) {
				score *= kSeatFireActiveWeapon;
			}
			if (score > best_weapon_score) {
				best_weapon = int(w);
				best_weapon_score = score;
			}
			max_range = std::max(max_range, weapon.max_range);
		}
		if (best_weapon_score == 0.0f) {
			continue;
		}

		float target_area = area;
		if (p_bot.mobile && p_bot.has_area && !target.in_area) {
			target_area *= kSeatFireOutsideAreaFactor;
		}
		float approach = 1.0f;
		if (!target.mobile || !(distance > max_range)) {
			target_area = kSeatFireAreaFactor;
		} else {
			approach = closing_approach(p_bot, target, direction);
		}
		if (distance > max_range &&
				(!target.mobile || !p_bot.mobile ||
						distance - max_range > p_bot.max_speed * kSeatFireApproachSeconds)) {
			continue;
		}

		float strength_sum = 0.0f;
		float score = 0.0f;
		const float armour = AiSettings::kArmourClassValues[size_t(target.strength_type)];
		for (uint8_t k = 0; k < target.part_count; ++k) {
			const SeatFirePart &part = target.parts[k];
			strength_sum += part.battle[size_t(p_bot.strength_type)];
			float strength = part.battle[size_t(p_bot.strength_type)];
			if (p_bot.exposed_soldier) {
				strength = std::max(strength, part.battle[size_t(p_bot.soldier_strength_type)]);
			}
			if (!target.soldier) {
				strength *= kSeatFireEnemyStrengthTweak;
			}
			score += (strength + part.strength + p_bot.battle[size_t(target.strength_type)]) * armour * target_area;
		}

		const float range_factor = std::max(1.0f - distance / (max_range * kSeatFireRangeSlack), 0.0f);
		score = range_factor * best_weapon_score * approach * attack_factor(target) * target_area * score;
		const SeatFireWeapon &chosen = p_weapons[size_t(best_weapon)];
		score *= ((chosen.max_range - chosen.min_range) * kSeatFireMinRangeShare + chosen.min_range) / distance;
		if (strength_sum <= 0.0f) {
			score *= kSeatFireHarmless;
		}
		if (target.id == p_bot.current_target) {
			current_score = score;
			keep = true;
		}
		if (score > out.score) {
			out.score = score;
			out.target = int(t);
			out.weapon = best_weapon;
			out.distance = distance;
		}
	}
	if (!(out.score > 0.0f)) {
		return SeatFireChoice{};
	}
	keep = keep && current_score * kSeatFireKeepTarget > out.score;
	if (keep) {
		for (size_t t = 0; t < p_targets.size(); ++t) {
			if (p_targets[t].id == p_bot.current_target) {
				out.target = int(t);
				// STAND-IN: the kept target's distance is held at 1 m or more, as in the loop; the pin leaves it
				// unclamped. Settle: the pin's kept-target score under 1 m.
				out.distance = std::max(vec::length(vec::sub(p_targets[t].position, p_bot.position)), 1.0f);
			}
		}
	}
	out.kept = keep;
	out.urgency = DecliningSlopeCurve::calculate(out.score + out.score);
	return out;
}

// SOURCE: OpenBF2 Code/BF2/AI/AIDLL/AI/AIBotActionPlan/BAPCConPrecision.cpp @c47317b, evaluate's variables path with
// waitForBest and limitPositiveY off.
bool fire_precise(const Vec3 &p_barrel, const Vec3 &p_aim, float p_distance, float p_precision, bool p_close_vehicle) {
	const float precision = std::max(p_precision, kMinFirePrecision);
	const float precision_sqr = std::max(p_precision * p_precision, kMinFirePrecision * kMinFirePrecision);
	if (!p_close_vehicle &&
			std::atan(precision / kPrecisionAngleMetres) <=
					std::acos(std::clamp(vec::dot(p_barrel, p_aim), -1.0f, 1.0f))) {
		return false;
	}
	const Vec3 off = vec::scale(vec::sub(p_barrel, p_aim), p_distance);
	if (precision_sqr < off.x * off.x + off.z * off.z) {
		return false;
	}
	if (p_close_vehicle) {
		return true;
	}
	const float vertical = off.y * off.y * (off.y > 0.0f ? 1.0f : off.y < 0.0f ? -1.0f : 0.0f);
	const float upper = precision_sqr * kPrecisionVerticalShare;
	return vertical <= upper && vertical >= -upper * kPrecisionNegativeYLimit;
}

}
