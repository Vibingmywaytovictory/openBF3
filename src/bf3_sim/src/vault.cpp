#include "bf3/sim/vault.h"

#include "bf3/sim/collision_world.h"

#include <algorithm>
#include <cmath>

namespace bf3::sim {

namespace {

float clamp_between(float p_value, float p_low, float p_high) {
	return std::fmin(std::fmax(p_value, p_low), p_high);
}

float ratio_or(float p_measured, float p_optimum, float p_low, float p_high, float p_fallback) {
	return p_optimum > vault_rules::kOptimumFloor ? clamp_between(p_measured / p_optimum, p_low, p_high) : p_fallback;
}

Vec3 at(const Vec3 &p_feet, const Vec3 &p_forward, float p_ahead, float p_up) {
	return { p_feet[0] + p_forward[0] * p_ahead, p_feet[1] + p_up, p_feet[2] + p_forward[2] * p_ahead };
}

float height_at(
		const CollisionWorld &p_world, const Vec3 &p_feet, const Vec3 &p_forward, float p_ahead, uint32_t p_mask) {
	StaticHit hit;
	const Vec3 top = at(p_feet, p_forward, p_ahead, vault_rules::kDownProbe);
	const float fraction =
			p_world.ray(top, { 0.0f, -vault_rules::kDownProbe, 0.0f }, p_mask, &hit) ? hit.fraction : 1.0f;
	return vault_probe_height(fraction);
}

std::optional<float> face_distance(
		const CollisionWorld &p_world, const Vec3 &p_origin, const Vec3 &p_forward, uint32_t p_mask) {
	StaticHit hit;
	const Vec3 reach = { p_forward[0] * vault_rules::kProbeReach, 0.0f, p_forward[2] * vault_rules::kProbeReach };
	if (!p_world.ray(p_origin, reach, p_mask, &hit)) {
		return std::nullopt;
	}
	return hit.fraction * vault_rules::kProbeReach;
}

}

bool vault_input_allows(float p_forward) {
	return !(vault_rules::kMinForwardInput > p_forward);
}

float vault_probe_height(float p_fraction) {
	return std::fmax((1.0f - p_fraction) * vault_rules::kDownProbe, vault_rules::kMinHeight);
}

VaultKind vault_up_or_over(VaultKind p_kind, float p_front_height, float p_further_height) {
	if (p_kind == VaultKind::OverHigh && p_further_height >= p_front_height - vault_rules::kUpTolerance) {
		return VaultKind::UpHigh;
	}
	return p_kind;
}

uint8_t choose_vault_variant(const VaultParams &p_params, VaultKind p_kind, const VaultMeasure &p_measure) {
	const size_t k = size_t(p_kind);
	const size_t n = std::min<size_t>(p_params.variant_count[k], kMaxVaultVariants);
	for (size_t i = 0; i < n; ++i) {
		const VaultVariant &v = p_params.variants[k][i];
		if (std::fabs(p_measure.distance - v.distance) < v.distance_tolerance &&
				std::fabs(p_measure.height - v.height) < v.height_tolerance &&
				std::fabs(p_measure.length - v.length) < v.length_tolerance) {
			return uint8_t(i);
		}
	}
	return kPlainVaultMove;
}

const SpecialMoveParams &vault_move(const VaultParams &p_params, VaultKind p_kind, uint8_t p_variant) {
	const size_t k = std::min<size_t>(size_t(p_kind), kVaultKindCount - 1);
	if (p_variant < std::min<size_t>(p_params.variant_count[k], kMaxVaultVariants)) {
		return p_params.variants[k][p_variant].move;
	}
	return p_params.plain[k];
}

VaultScales vault_scales(const SpecialMoveParams &p_move, const VaultMeasure &p_measure, bool p_disabled) {
	VaultScales s;
	if (!p_move.scale_with_distance || p_disabled) {
		return s;
	}
	s.on = true;
	s.distance = ratio_or(
			p_measure.distance, p_move.optimum_distance, p_move.min_distance_scale, p_move.max_distance_scale, 0.0f);
	s.length =
			ratio_or(p_measure.length, p_move.optimum_length, p_move.min_length_scale, p_move.max_length_scale, 1.0f);
	s.height =
			ratio_or(p_measure.height, p_move.optimum_height, p_move.min_height_scale, p_move.max_height_scale, 0.0f);
	return s;
}

float vault_horizontal_scale(const VaultScales &p_scales, const VaultMeasure &p_measure, float p_moved) {
	if (!p_scales.on) {
		return 1.0f;
	}
	if (p_moved < p_measure.distance - vault_rules::kPhaseMargin) {
		return p_scales.distance;
	}
	if (p_moved >= p_measure.distance + p_measure.length + vault_rules::kPhaseMargin) {
		return 1.0f;
	}
	return p_scales.length;
}

std::optional<Vec3> sample_move_keys(std::span<const MoveKey> p_keys, float p_time) {
	if (p_keys.empty()) {
		return std::nullopt;
	}
	if (p_time <= p_keys.front().time) {
		return p_keys.front().value;
	}
	for (size_t i = 1; i < p_keys.size(); ++i) {
		const MoveKey &a = p_keys[i - 1];
		const MoveKey &b = p_keys[i];
		if (p_time <= b.time) {
			const float span = b.time - a.time;
			if (!(span > 0.0f)) {
				return b.value;
			}
			const float w = (p_time - a.time) / span;
			return Vec3{ a.value[0] + (b.value[0] - a.value[0]) * w, a.value[1] + (b.value[1] - a.value[1]) * w,
				a.value[2] + (b.value[2] - a.value[2]) * w };
		}
	}
	return p_keys.back().value;
}

float move_duration(const SpecialMoveParams &p_move) {
	const size_t n = std::min<size_t>(p_move.motion_count, kMaxMoveKeys);
	return n == 0 ? 0.0f : p_move.motion[n - 1].time;
}

bool vault_contact_aborts(const Vec3 &p_contact_offset, const Vec3 &p_normal, const Vec3 &p_motion) {
	const float d2 = p_contact_offset[0] * p_contact_offset[0] + p_contact_offset[1] * p_contact_offset[1] +
			p_contact_offset[2] * p_contact_offset[2];
	if (d2 < vault_rules::kContactSqrDistance) {
		return false;
	}
	const float m2 = p_motion[0] * p_motion[0] + p_motion[1] * p_motion[1] + p_motion[2] * p_motion[2];
	if (m2 < vault_rules::kContactMinSqrSpeed) {
		return false;
	}
	const float along =
			(p_normal[0] * p_motion[0] + p_normal[1] * p_motion[1] + p_normal[2] * p_motion[2]) / std::sqrt(m2);
	return along < vault_rules::kContactCos;
}

std::optional<VaultStart> probe_vault(
		const CollisionWorld &p_world, const Vec3 &p_feet, const Vec3 &p_forward, uint32_t p_mask) {
	using namespace vault_rules;
	const Vec3 &forward = p_forward;
	const Vec3 right{ -forward[2], 0.0f, forward[0] };
	if (face_distance(p_world, at(p_feet, forward, 0.0f, kMiddleHeight + kHighOffset), forward, p_mask)) {
		return std::nullopt;
	}
	VaultStart out;
	float probe_up = kMiddleHeight + kMiddleOffset;
	std::optional<float> distance = face_distance(p_world, at(p_feet, forward, 0.0f, probe_up), forward, p_mask);
	if (!distance) {
		probe_up = kMiddleHeight + kLowOffset;
		distance = face_distance(p_world, at(p_feet, forward, 0.0f, probe_up), forward, p_mask);
		if (!distance) {
			return std::nullopt;
		}
		out.kind = VaultKind::OverLow;
	}
	const float d = *distance;
	const float front = height_at(p_world, p_feet, forward, d + kFrontEpsilon, p_mask);
	const float further = height_at(p_world, p_feet, forward, d + kUpLength, p_mask);
	out.kind = vault_up_or_over(out.kind, front, further);
	float nearest = d;
	for (const float side : { kSideOffset, -kSideOffset }) {
		const Vec3 origin{ p_feet[0] + right[0] * side, p_feet[1] + probe_up, p_feet[2] + right[2] * side };
		if (const std::optional<float> s = face_distance(p_world, origin, forward, p_mask)) {
			nearest = std::fmin(nearest, *s);
		}
	}
	float length = kUpLength;
	if (out.kind != VaultKind::UpHigh) {
		StaticHit back;
		const Vec3 beyond = at(p_feet, forward, d + kBackSearch, probe_up);
		const Vec3 toward{ -forward[0] * kBackSearch, 0.0f, -forward[2] * kBackSearch };
		length = p_world.ray(beyond, toward, p_mask, &back)
				? std::fmax(kBackSearch * (1.0f - back.fraction), kMinLength)
				: kMinLength;
	}
	const float middle = height_at(p_world, p_feet, forward, d + 0.5f * length, p_mask);
	out.measure.distance = nearest;
	out.measure.height = std::fmax(std::fmax(front, further), middle);
	out.measure.length = length;
	return out;
}

}
