#include "bf3/sim/vehicles/vehicle_armament.h"

#include "bf3/core/gameplay/vehicle_def.h"
#include "bf3/core/util/finite.h"

#include <algorithm>
#include <cmath>
#include <string_view>

namespace bf3::sim::vehicles {

namespace {

using core::finite;
namespace vec = core::vec;

constexpr float kDegreesPerRadian = 57.2957795f;

core::Vec3 axis_unit(int p_axis) {
	return p_axis == 0	  ? core::Vec3{ 1.0f, 0.0f, 0.0f }
			: p_axis == 2 ? core::Vec3{ 0.0f, 0.0f, 1.0f }
						  : core::Vec3{ 0.0f, 1.0f, 0.0f };
}

core::Vec3 axis_reference(int p_axis) {
	return p_axis == 2 ? core::Vec3{ 1.0f, 0.0f, 0.0f } : core::Vec3{ 0.0f, 0.0f, 1.0f };
}

core::Transform axis_rotation(int p_axis, float p_angle) {
	const float s = std::sin(p_angle / 2.0f);
	const core::Vec3 a = axis_unit(p_axis);
	return core::Transform::from_rotation_translation({ a[0] * s, a[1] * s, a[2] * s, std::cos(p_angle / 2.0f) }, {});
}

core::Transform parent_world(
		const VehicleArmament &p_armament, const ArmamentState &p_state, const core::Transform &p_hull, int p_turret) {
	return p_turret >= 0 ? turret_world(p_armament, p_state, p_hull, size_t(p_turret)) : p_hull;
}

bool frame_ok(const core::Transform &p_frame) {
	for (size_t i = 0; i < 3; ++i) {
		if (!finite(p_frame.axis(i))) {
			return false;
		}
	}
	return finite(p_frame.trans) && std::fabs(p_frame.trans[0]) < 100.0f && std::fabs(p_frame.trans[1]) < 100.0f &&
			std::fabs(p_frame.trans[2]) < 100.0f;
}

TurretAimAxis aim_of(std::string_view p_input, int p_axis) {
	const bool player = p_input == "EIAYaw" || p_input == "EIARoll" || p_input == "EIAPitch" ||
			p_input == "EIACameraYaw" || p_input == "EIACameraPitch";
	if (!player) {
		return TurretAimAxis::None;
	}
	return p_axis == 1 ? TurretAimAxis::Yaw : p_axis == 0 ? TurretAimAxis::Pitch : TurretAimAxis::None;
}

}

core::Result<VehicleArmament> build_vehicle_armament(const core::gameplay::VehicleDef &p_def) {
	const std::string where = "vehicle armament " + p_def.blueprint;
	if (p_def.turrets.size() > kMaxArmamentTurrets || p_def.weapons.size() > kMaxArmamentWeapons ||
			p_def.seats.size() > kMaxArmamentSeats) {
		return core::make_error(where + ": more than " + std::to_string(kMaxArmamentTurrets) + " turrets, " +
				std::to_string(kMaxArmamentWeapons) + " weapons or " + std::to_string(kMaxArmamentSeats) + " seats");
	}
	VehicleArmament out;
	for (size_t i = 0; i < p_def.turrets.size(); ++i) {
		const core::gameplay::VehicleTurretDef &t = p_def.turrets[i];
		if (t.parent >= int(i) || t.frame_turret >= int(i) || t.seat >= int(p_def.seats.size()) || !frame_ok(t.frame)) {
			return core::make_error(where + ": turret " + std::to_string(i) +
					" has a later parent, a seat out of range or a bad frame");
		}
		const float numbers[] = { t.mass, t.inertia, t.angular_momentum_multiplier, t.force_modifier,
			t.reset_force_modifier, t.reset_force_input_threshold, t.angular_momentum_threshold, t.damping_acc,
			t.damping_deacc, t.min_angle, t.max_angle, t.world_space_lock_efficiency, t.extended_heading,
			t.extended_width, t.extended_falloff, t.extended_min_angle, t.extended_max_angle };
		for (const float n : numbers) {
			if (!finite(n) || std::fabs(n) > 1e7f) {
				return core::make_error(where + ": turret " + std::to_string(i) + " has a non-finite or huge value");
			}
		}
		if (!(t.mass > 0.0f) || !(t.inertia > 0.0f) || t.world_space_lock_efficiency < 0.0f ||
				t.world_space_lock_efficiency > 1.0f || t.rotation_axis < 0 || t.rotation_axis > 2) {
			return core::make_error(
					where + ": turret " + std::to_string(i) + " mass, inertia, axis or lock efficiency out of range");
		}
		ArmamentTurret a;
		RotationBodyRule &r = a.rule;
		r.mass = t.mass;
		r.inertia = t.inertia;
		r.momentum_multiplier = t.angular_momentum_multiplier;
		r.force_modifier = t.force_modifier;
		r.reset_force_modifier = t.reset_force_modifier;
		r.reset_input_threshold = t.reset_force_input_threshold;
		r.momentum_threshold = t.angular_momentum_threshold;
		r.damping_acc = t.damping_acc;
		r.damping_deacc = t.damping_deacc;
		r.use_linear_input = t.use_linear_input;
		r.use_threshold = t.use_threshold;
		r.use_damping = t.use_damping;
		r.post_satisfy = t.post_satisfy;
		r.constrained = t.constrained;
		r.min_degrees = t.min_angle;
		r.max_degrees = t.max_angle;
		r.extended = { t.extended, t.extended_heading, t.extended_width, t.extended_falloff, t.extended_min_angle,
			t.extended_max_angle };
		r.world_lock_efficiency = t.world_space_lock_efficiency;
		a.parent = int8_t(t.parent);
		a.frame_turret = int8_t(t.frame_turret);
		a.seat = int8_t(t.seat);
		a.aim = aim_of(t.rotation_input, t.rotation_axis);
		a.rule_axis = uint8_t(t.rotation_axis);
		a.frame = t.frame;
		out.turrets.push_back(a);
	}
	for (size_t i = 0; i < p_def.weapons.size(); ++i) {
		const core::gameplay::VehicleWeaponDef &w = p_def.weapons[i];
		if (w.firing.empty() || w.seat < 0) {
			continue;
		}
		if (w.seat >= int(p_def.seats.size()) || w.turret >= int(p_def.turrets.size()) ||
				w.frame_turret >= int(p_def.turrets.size()) || !frame_ok(w.frame) ||
				!finite(w.projectile_spawn_offset) || !finite(w.impulse_strength) ||
				!(w.reload_time_multiplier >= 0.0f && w.reload_time_multiplier < 100.0f) ||
				!(w.overheat_drop_multiplier >= 0.0f && w.overheat_drop_multiplier < 100.0f)) {
			return core::make_error(
					where + ": weapon " + std::to_string(i) + " has a seat, turret or value out of range");
		}
		ArmamentWeapon a;
		a.firing = w.firing;
		a.seat = int8_t(w.seat);
		a.turret = int8_t(w.turret);
		a.frame_turret = int8_t(w.frame_turret);
		a.frame = w.frame;
		a.spawn_offset = w.projectile_spawn_offset;
		a.impulse_strength = w.impulse_strength;
		a.reload_time_multiplier = w.reload_time_multiplier;
		a.overheat_drop_multiplier = w.overheat_drop_multiplier;
		if (!w.stances.empty()) {
			a.stance_mask = 0;
			for (const int s : w.stances) {
				if (s >= 0 && s < 8) {
					a.stance_mask = uint8_t(a.stance_mask | (1u << s));
				}
			}
		}
		out.weapons.push_back(std::move(a));
	}
	for (const core::gameplay::VehicleSeatDef &s : p_def.seats) {
		out.seat_stances.push_back(uint8_t(std::clamp(s.stance_count, 1, 8)));
	}
	return out;
}

core::Transform hull_transform(const std::array<float, 4> &p_rotation, const core::Vec3 &p_origin) {
	return core::Transform::from_rotation_translation(p_rotation, p_origin);
}

float hull_up_y(const std::array<float, 4> &p_rotation) {
	return 1.0f - 2.0f * (p_rotation[0] * p_rotation[0] + p_rotation[2] * p_rotation[2]);
}

core::Transform turret_world(const VehicleArmament &p_armament, const ArmamentState &p_state,
		const core::Transform &p_hull, size_t p_turret) {
	const ArmamentTurret &t = p_armament.turrets[p_turret];
	const core::Transform rest = parent_world(p_armament, p_state, p_hull, t.frame_turret).then_inner(t.frame);
	return rest.then_inner(axis_rotation(t.rule_axis, p_state.bodies[p_turret].angle));
}

core::Transform weapon_world(const VehicleArmament &p_armament, const ArmamentState &p_state,
		const core::Transform &p_hull, size_t p_weapon) {
	const ArmamentWeapon &w = p_armament.weapons[p_weapon];
	return parent_world(p_armament, p_state, p_hull, w.frame_turret).then_inner(w.frame);
}

core::Vec3 weapon_muzzle(const VehicleArmament &p_armament, const ArmamentState &p_state, const core::Transform &p_hull,
		size_t p_weapon) {
	return weapon_world(p_armament, p_state, p_hull, p_weapon)
			.transform_point(p_armament.weapons[p_weapon].spawn_offset);
}

float aim_error(
		const ArmamentTurret &p_turret, float p_angle, const core::Transform &p_rest, const core::Vec3 &p_direction) {
	const float x = vec::dot(p_direction, p_rest.right);
	const float y = vec::dot(p_direction, p_rest.up);
	const float z = vec::dot(p_direction, p_rest.forward);
	float desired = p_angle;
	if (p_turret.aim == TurretAimAxis::Yaw) {
		desired = std::atan2(x, z);
	} else if (p_turret.aim == TurretAimAxis::Pitch) {
		desired = std::atan2(-y, std::sqrt(x * x + z * z));
	}
	return wrap_angle(desired - p_angle);
}

float aim_level(const ArmamentTurret &p_turret, float p_error) {
	const float step = rotation_steady_rate(p_turret.rule, 1.0f, kTurretStepSeconds) * kTurretStepSeconds;
	return step > 0.0f ? std::clamp(-p_error / step, -1.0f, 1.0f) : 0.0f;
}

void step_armament(const VehicleArmament &p_armament, ArmamentState &r_state, const core::Transform &p_hull,
		std::span<const SeatAim> p_seats) {
	for (size_t i = 0; i < p_armament.turrets.size() && i < kMaxArmamentTurrets; ++i) {
		const ArmamentTurret &t = p_armament.turrets[i];
		RotationBodyState &body = r_state.bodies[i];
		const bool has_parent = t.parent >= 0;
		const float parent_degrees = has_parent ? r_state.bodies[size_t(t.parent)].angle * kDegreesPerRadian : 0.0f;
		const RotationLimits limits = rotation_limits(t.rule, parent_degrees, has_parent);
		const core::Transform rest = parent_world(p_armament, r_state, p_hull, t.frame_turret).then_inner(t.frame);
		const core::Vec3 axis = vec::normalize(rest.transform_vector(axis_unit(t.rule_axis)));
		const core::Vec3 reference = rest.transform_vector(axis_reference(t.rule_axis));
		if (r_state.parent_known[i] != 0) {
			const core::Vec3 before = r_state.parent_reference[i];
			const float twist = std::atan2(vec::dot(axis, vec::cross(before, reference)),
					vec::dot(vec::sub(before, vec::scale(axis, vec::dot(before, axis))), reference));
			body.angle = rotation_direct(
					t.rule, body.angle, world_lock_rotation(t.rule.world_lock_efficiency, twist), limits);
		}
		r_state.parent_reference[i] = reference;
		r_state.parent_known[i] = 1;
		float level = 0.0f;
		if (t.seat >= 0 && size_t(t.seat) < p_seats.size() && p_seats[size_t(t.seat)].active &&
				t.aim != TurretAimAxis::None) {
			level = aim_level(t, aim_error(t, body.angle, rest, p_seats[size_t(t.seat)].direction));
		}
		rotation_body_step(t.rule, body, rotation_input_torque(t.rule, level, body.angle), limits, kTurretStepSeconds);
	}
}

bool weapon_in_stance(const ArmamentWeapon &p_weapon, uint8_t p_stance) {
	return p_stance < 8 && (p_weapon.stance_mask & (1u << p_stance)) != 0;
}

}
