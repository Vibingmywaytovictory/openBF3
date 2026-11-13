#include "bf3/present/vehicle_turrets.h"

#include "bf3/sim/command.h"

#include <cmath>

namespace bf3::present {

namespace {

namespace vec = core::vec;

core::Transform turn_about(const core::Vec3 &p_axis, const core::Vec3 &p_pivot, float p_angle) {
	const core::Vec3 a = vec::normalize_or(p_axis, core::Vec3{ 0.0f, 1.0f, 0.0f });
	const float s = std::sin(0.5f * p_angle);
	core::Transform t =
			core::Transform::from_rotation_translation({ a[0] * s, a[1] * s, a[2] * s, std::cos(0.5f * p_angle) }, {});
	t.trans = vec::sub(p_pivot, t.transform_vector(p_pivot));
	return t;
}

}

std::vector<core::Transform> turret_offsets(
		const sim::vehicles::VehicleArmament &p_armament, std::span<const float> p_angles) {
	const sim::vehicles::ArmamentState rest{};
	std::vector<core::Transform> out(p_armament.turrets.size());
	for (size_t i = 0; i < p_armament.turrets.size() && i < rest.bodies.size(); ++i) {
		const sim::vehicles::ArmamentTurret &t = p_armament.turrets[i];
		const float angle = i < p_angles.size() && std::isfinite(p_angles[i]) ? p_angles[i] : 0.0f;
		const core::Transform frame = sim::vehicles::turret_world(p_armament, rest, core::Transform::identity(), i);
		const float handed = frame.determinant() < 0.0f ? -1.0f : 1.0f;
		const core::Transform parent =
				t.frame_turret >= 0 && size_t(t.frame_turret) < i ? out[size_t(t.frame_turret)] : core::Transform{};
		out[i] = parent.then_inner(turn_about(frame.axis(t.rule_axis), frame.trans, handed * angle));
	}
	return out;
}

VehicleLookLimits camera_look_limits(const core::gameplay::VehicleCameraDef &p_camera) {
	VehicleLookLimits out;
	out.left = out.right = sim::kPi;
	out.up = out.down = 0.5f * sim::kPi;
	if (p_camera.left_yaw > 0.0f || p_camera.right_yaw > 0.0f) {
		out.left = p_camera.left_yaw * core::kDegToRadF;
		out.right = p_camera.right_yaw * core::kDegToRadF;
		if (p_camera.turret >= 0) {
			out.centre = std::atan2(p_camera.transform.forward[0], p_camera.transform.forward[2]);
		}
	}
	if (p_camera.up_pitch > 0.0f || p_camera.down_pitch > 0.0f) {
		out.up = p_camera.up_pitch * core::kDegToRadF;
		out.down = p_camera.down_pitch * core::kDegToRadF;
	}
	return out;
}

int part_turret(std::span<const core::gameplay::VehiclePartTurretDef> p_parts, std::span<const int> p_component_meshes,
		uint8_t p_mesh, uint16_t p_part) {
	if (p_mesh > 0) {
		return size_t(p_mesh - 1) < p_component_meshes.size() ? p_component_meshes[size_t(p_mesh - 1)] : -1;
	}
	for (const core::gameplay::VehiclePartTurretDef &p : p_parts) {
		if (p.part == p_part) {
			return p.turret;
		}
	}
	return -1;
}

}
