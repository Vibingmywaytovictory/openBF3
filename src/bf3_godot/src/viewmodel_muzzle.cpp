// G6 (W3.0 contract, docs/design/m2a-w3.md): the own 1p muzzle from a presented Bf3Viewmodel. See viewmodel_muzzle.h.

#include "viewmodel_muzzle.h"

#include "render_bridge.h"
#include "viewmodel.h"

#include <godot_cpp/classes/skeleton3d.hpp>
#include <godot_cpp/core/math.hpp>

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

} // namespace bf3::godot_side
