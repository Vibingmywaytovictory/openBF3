#include "fp_camera.h"

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/transform3d.hpp>

#include <algorithm>
#include <cmath>

namespace bf3::godot_side {

using namespace godot;

namespace {

Basis view_basis(float p_yaw, float p_pitch) {
	// Godot cameras look down -Z; the sim's yaw 0 looks down +Z (sim_types.h): rotate_y(yaw + pi) * rotate_x(pitch).
	return Basis(Vector3(0, 1, 0), float(double(p_yaw) + 3.14159265358979323846)) * Basis(Vector3(1, 0, 0), p_pitch);
}

} // namespace

Vector3 presented_eye(const bf3::host::PresentationFrame &p_frame, double p_fraction) {
	const double t = std::clamp(p_fraction, 0.0, 1.0);
	double p[3];
	for (size_t k = 0; k < 3; ++k) {
		const double a = double(p_frame.own_prev.position[k]);
		const double b = double(p_frame.own_curr.position[k]);
		p[k] = a + (b - a) * t + double(p_frame.visual_offset[k]);
	}
	p[1] += double(presented_eye_height(p_frame, t));
	return Vector3(float(p[0]), float(p[1]), float(p[2]));
}

float presented_eye_height(const bf3::host::PresentationFrame &p_frame, double p_fraction) {
	const double t = std::clamp(p_fraction, 0.0, 1.0);
	return float(double(p_frame.eye_height_prev) + (double(p_frame.eye_height_curr) - double(p_frame.eye_height_prev)) * t);
}

void FpCamera::attach(Node *p_parent) {
	fp_ = memnew(Camera3D);
	fp_->set_name("FirstPersonCamera");
	fp_->set_fov(kFovDegrees);
	fp_->set_near(0.05f);
	fp_->set_far(5000.0f);
	p_parent->add_child(fp_);
	free_cam_ = memnew(Camera3D);
	free_cam_->set_name("FreeCamera");
	free_cam_->set_fov(kFovDegrees);
	free_cam_->set_near(0.1f);
	free_cam_->set_far(8000.0f);
	p_parent->add_child(free_cam_);
	fp_->make_current();
}

Vector3 FpCamera::update_first_person(const bf3::host::PresentationFrame &p_frame, uint64_t p_now_ns, float p_yaw, float p_pitch) {
	if (fp_ == nullptr || !p_frame.valid) {
		return last_eye_;
	}
	last_fraction_ = bf3::host::present_own_fraction(p_frame, p_now_ns);
	last_eye_ = presented_eye(p_frame, last_fraction_);
	if (!free_) {
		fp_->set_global_transform(Transform3D(view_basis(p_yaw, p_pitch), last_eye_));
	}
	return last_eye_;
}

void FpCamera::update_free(double p_delta, const Vector3 &p_move, float p_factor, float p_yaw, float p_pitch) {
	if (free_cam_ == nullptr || !free_) {
		return;
	}
	const Basis basis = view_basis(p_yaw, p_pitch);
	const Vector3 forward = -basis.get_column(2);
	const Vector3 right = basis.get_column(0);
	Vector3 dir = right * p_move.x + Vector3(0, 1, 0) * p_move.y + forward * p_move.z;
	if (dir.length_squared() > 0.0f) {
		free_position_ += dir.normalized() * float(double(free_speed_ * p_factor) * p_delta);
	}
	free_cam_->set_global_transform(Transform3D(basis, free_position_));
}

void FpCamera::set_free(bool p_on) {
	if (p_on == free_) {
		return;
	}
	free_ = p_on;
	if (free_) {
		free_position_ = last_eye_;
		if (free_cam_ != nullptr) {
			free_cam_->set_global_transform(fp_ != nullptr ? fp_->get_global_transform() : Transform3D(Basis(), free_position_));
			free_cam_->make_current();
		}
	} else if (fp_ != nullptr) {
		fp_->make_current();
	}
}

void FpCamera::place_free(const Vector3 &p_position, float p_yaw, float p_pitch) {
	free_position_ = p_position;
	if (free_cam_ != nullptr) {
		free_cam_->set_global_transform(Transform3D(view_basis(p_yaw, p_pitch), free_position_));
	}
}

void FpCamera::scale_free_speed(float p_factor) {
	free_speed_ = std::clamp(free_speed_ * p_factor, 1.0f, 400.0f);
}

} // namespace bf3::godot_side

// M2a vm (docs/design/m2a-vm.md): ADS FOV, recoil and kick on the first-person camera (fp_camera.h's M2a vm block).
#include "bf3/anim/viewmodel_anim.h"

namespace bf3::godot_side {

void apply_view_effects(godot::Camera3D *p_camera, const godot::Vector3 &p_eye, double p_yaw, double p_pitch, const ViewEffects &p_effects) {
	if (p_camera == nullptr) {
		return;
	}
	const bf3::anim::ViewAngles a = bf3::anim::camera_view_angles(p_yaw, p_pitch, p_effects.recoil_pitch_deg, p_effects.recoil_yaw_deg,
			p_effects.kick_pitch_deg, p_effects.kick_yaw_deg);
	const godot::Basis basis = godot::Basis(godot::Vector3(0, 1, 0), real_t(a.yaw + 3.14159265358979323846)) * godot::Basis(godot::Vector3(1, 0, 0), real_t(a.pitch));
	p_camera->set_keep_aspect_mode(godot::Camera3D::KEEP_HEIGHT);
	p_camera->set_fov(p_effects.world_fov_deg);
	p_camera->set_global_transform(godot::Transform3D(basis, p_eye));
}

void camera_view_angles_of(const godot::Camera3D *p_camera, double &r_yaw, double &r_pitch) {
	r_yaw = 0;
	r_pitch = 0;
	if (p_camera == nullptr) {
		return;
	}
	const godot::Vector3 f = -p_camera->get_global_transform().basis.get_column(2);
	r_pitch = std::asin(std::clamp(double(f.y), -1.0, 1.0));
	r_yaw = std::atan2(double(f.x), double(f.z));
}

} // namespace bf3::godot_side
