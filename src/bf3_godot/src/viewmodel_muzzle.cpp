// G6 (W3.0 contract, docs/design/m2a-w3.md): the own 1p muzzle from a presented Bf3Viewmodel. See viewmodel_muzzle.h.

#include "viewmodel_muzzle.h"

#include "render_bridge.h"
#include "viewmodel.h"

#include <godot_cpp/classes/skeleton3d.hpp>
#include <godot_cpp/core/math.hpp>

#include <cmath>

namespace bf3::godot_side {

using namespace godot;

ViewmodelMuzzle viewmodel_muzzle_transform(const Bf3Viewmodel &p_viewmodel) {
	return viewmodel_muzzle_transform(p_viewmodel, Transform3D());
}

ViewmodelMuzzle viewmodel_muzzle_transform(const Bf3Viewmodel &p_viewmodel, const Transform3D &p_offset) {
	ViewmodelMuzzle out;
	const Skeleton3D *skeleton = p_viewmodel.skeleton();
	const std::vector<bf3::anim::Xform> &model = p_viewmodel.model();
	if (!p_viewmodel.built() || skeleton == nullptr || model.empty()) {
		return out;
	}
	const Transform3D rig = skeleton->get_global_transform();
	const int32_t root = p_viewmodel.current_item() != nullptr ? int32_t(skeleton->find_bone("Wep_Root")) : -1;
	if (root >= 0 && size_t(root) < model.size()) {
		out.valid = true;
		out.source = ViewmodelMuzzleSource::WeaponRoot;
		out.joint = root;
		out.weapon = rig * to_godot(model[size_t(root)]);
		out.muzzle = out.weapon * p_offset;
		return out;
	}
	// The stand-in: the camera frame is the rig's CameraJoint turned back by the 180 degrees viewmodel.cpp applies
	// (rig = camera x offset x Ry(pi) x inverse(model[CameraJoint])).
	const int32_t cj = p_viewmodel.camera_joint();
	if (cj < 0 || size_t(cj) >= model.size()) {
		return out;
	}
	const Transform3D turn(Basis(Vector3(0, 1, 0), real_t(Math::PI)), Vector3());
	const Transform3D camera = rig * to_godot(model[size_t(cj)]) * turn;
	out.valid = true;
	out.source = ViewmodelMuzzleSource::CameraStandIn;
	out.weapon = camera;
	out.muzzle = Transform3D(camera.basis, camera.xform(kMuzzleCameraStandIn));
	return out;
}

godot::Transform3D to_godot_transform(const fx::Transform &p_t) {
	const Vector3 x(p_t.right.x, p_t.right.y, p_t.right.z);
	const Vector3 y(p_t.up.x, p_t.up.y, p_t.up.z);
	const Vector3 z(p_t.forward.x, p_t.forward.y, p_t.forward.z);
	return Transform3D(Basis(x, y, z), Vector3(p_t.trans.x, p_t.trans.y, p_t.trans.z)); // Basis(x, y, z) takes columns
}

fx::Transform to_fx_transform(const godot::Transform3D &p_t) {
	auto v = [](const Vector3 &p_v) {
		return fx::Vec3{ float(p_v.x), float(p_v.y), float(p_v.z) };
	};
	fx::Transform t;
	t.right = v(p_t.basis.get_column(0));
	t.up = v(p_t.basis.get_column(1));
	t.forward = v(p_t.basis.get_column(2));
	t.trans = v(p_t.origin);
	return t;
}

fx::MuzzleSample muzzle_sample(const ViewmodelMuzzle &p_muzzle, uint8_t p_slot, const fx::Vec3 &p_velocity) {
	fx::MuzzleSample m;
	m.slot = p_slot;
	m.velocity = p_velocity;
	m.first_person = true;
	if (!p_muzzle.valid) {
		return m;
	}
	Transform3D w = p_muzzle.muzzle;
	if (p_muzzle.source == ViewmodelMuzzleSource::CameraStandIn) {
		// The camera frame (x right, y up, -z view): the weapon frame's barrel is -z, its right then -x.
		w.basis = Basis(-w.basis.get_column(0), w.basis.get_column(1), -w.basis.get_column(2));
	}
	w.orthonormalize();
	m.transform = fx::effect_frame_of_weapon(to_fx_transform(w));
	m.valid = std::isfinite(m.transform.trans.x) && std::isfinite(m.transform.trans.y) && std::isfinite(m.transform.trans.z);
	return m;
}

} // namespace bf3::godot_side
