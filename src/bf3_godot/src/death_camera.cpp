#include "death_camera.h"

#include "bf3/core/gameplay/death_timeline.h"
#include "bf3/core/util/finite.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/static_world.h"

#include <algorithm>
#include <cmath>

namespace bf3::godot_side {

namespace {

using bf3::core::finite;
using fx::Vec3;

constexpr float kDegToRad = 0.017453292519943295f;

Vec3 add(const Vec3 &p_a, const Vec3 &p_b) {
	return Vec3{ p_a.x + p_b.x, p_a.y + p_b.y, p_a.z + p_b.z };
}
Vec3 sub(const Vec3 &p_a, const Vec3 &p_b) {
	return Vec3{ p_a.x - p_b.x, p_a.y - p_b.y, p_a.z - p_b.z };
}
Vec3 mul(const Vec3 &p_a, float p_s) {
	return Vec3{ p_a.x * p_s, p_a.y * p_s, p_a.z * p_s };
}
float dot(const Vec3 &p_a, const Vec3 &p_b) {
	return p_a.x * p_b.x + p_a.y * p_b.y + p_a.z * p_b.z;
}
Vec3 cross(const Vec3 &p_a, const Vec3 &p_b) {
	return Vec3{ p_a.y * p_b.z - p_a.z * p_b.y, p_a.z * p_b.x - p_a.x * p_b.z, p_a.x * p_b.y - p_a.y * p_b.x };
}
float length(const Vec3 &p_a) {
	return std::sqrt(dot(p_a, p_a));
}
Vec3 normalized(const Vec3 &p_a, const Vec3 &p_fallback) {
	const float l = length(p_a);
	return l > 1e-6f && std::isfinite(l) ? mul(p_a, 1.0f / l) : p_fallback;
}
Vec3 lerp(const Vec3 &p_a, const Vec3 &p_b, float p_t) {
	return add(p_a, mul(sub(p_b, p_a), p_t));
}
fx::Transform look_from(const Vec3 &p_from, const Vec3 &p_forward) {
	const Vec3 f = p_forward;
	Vec3 r = cross(Vec3{ 0, 1, 0 }, f);
	if (length(r) < 1e-4f) {
		r = cross(Vec3{ 0, 0, 1 }, f);
	}
	r = normalized(r, Vec3{ 1, 0, 0 });
	fx::Transform t;
	t.right = r;
	t.up = normalized(cross(f, r), Vec3{ 0, 1, 0 });
	t.forward = f;
	t.trans = p_from;
	return t;
}

float update_rate(const core::gameplay::ChaseCameraDef &p_def) {
	return std::max(std::isfinite(p_def.update_rate) ? p_def.update_rate : 0.0f, kChaseMinUpdateRate);
}

float max_length(const core::gameplay::ChaseCameraDef &p_def) {
	return std::max(kChasePillMinLength, p_def.wanted_distance - 2.0f * p_def.pill_maximum_collision_radius);
}

}

ChaseFrame chase_frame(const core::gameplay::ChaseCameraDef &p_def, const ChaseTarget &p_target) {
	ChaseFrame c;
	const Vec3 f{ std::sin(p_target.yaw_rad) * std::cos(p_target.pitch_rad), std::sin(p_target.pitch_rad),
		std::cos(p_target.yaw_rad) * std::cos(p_target.pitch_rad) };
	const float horizontal = std::sqrt(f.x * f.x + f.z * f.z);
	c.pivot = p_target.eye;
	c.pivot.y += p_def.target_offset.y;
	Vec3 fh{ 0.0f, 0.0f, 1.0f };
	if (horizontal > 1e-6f) {
		fh = Vec3{ f.x / horizontal, 0.0f, f.z / horizontal };
	}
	Vec3 r{ fh.z, 0.0f, -fh.x };
	if (horizontal > 1e-6f) {
		c.pivot = add(c.pivot, add(mul(r, p_def.target_offset.x), mul(fh, p_def.target_offset.z)));
		const float a = p_def.target_rotation_offset_deg * kDegToRad;
		const Vec3 turned_r = sub(mul(r, std::cos(a)), mul(fh, std::sin(a)));
		fh = add(mul(r, std::sin(a)), mul(fh, std::cos(a)));
		r = turned_r;
	}
	const float wanted = p_def.wanted_angle_deg * kDegToRad;
	c.direction = normalized(
			add(mul(fh, -std::cos(wanted)), mul(Vec3{ 0.0f, 1.0f, 0.0f }, std::sin(wanted))), Vec3{ 0, 0, -1 });
	c.forward = add(mul(fh, std::cos(p_target.pitch_rad)), Vec3{ 0.0f, std::sin(p_target.pitch_rad), 0.0f });
	c.right = r;
	return c;
}

ChaseCamera::ChaseCamera(const core::gameplay::ChaseCameraDef &p_def) : def_(p_def) {}

void ChaseCamera::activate(const ChaseTarget &p_target, const sim::CollisionWorld *p_world) {
	const ChaseFrame c = chase_frame(def_, p_target);
	radius_ = std::max(0.0f, def_.pill_maximum_collision_radius);
	length_ = max_length(def_);
	grow_speed_ = 0.0f;
	accumulated_ = 0.0;
	pill_accumulated_ = 0.0;
	pill(c, 0.0, p_world);
	position_ = previous_ = wanted_;
	velocity_ = Vec3{};
}

void ChaseCamera::pill(const ChaseFrame &p_frame, double p_dt, const sim::CollisionWorld *p_world) {
	const float longest = max_length(def_);
	const float widest = std::max(0.0f, def_.pill_maximum_collision_radius);
	const float shortest = std::clamp(def_.pill_minimum_collision_length, kChasePillMinLength, longest);
	const float thinnest = std::clamp(def_.pill_minimum_collision_radius, 0.0f, widest);
	const auto collide = [&]() {
		if (!def_.has_collision || p_world == nullptr) {
			return;
		}
		for (int pass = 0; pass < 2; ++pass) {
			const Vec3 from = add(p_frame.pivot, mul(p_frame.direction, radius_));
			const Vec3 delta = mul(p_frame.direction, length_);
			sim::StaticHit hit;
			const bool blocked = radius_ > 0.0f
					? p_world->sweep_capsule(sim::Vec3{ from.x, from.y - radius_, from.z }, radius_, 2.0f * radius_,
							  sim::Vec3{ delta.x, delta.y, delta.z }, sim::query_mask::kProjectile, &hit)
					: p_world->ray(sim::Vec3{ from.x, from.y, from.z }, sim::Vec3{ delta.x, delta.y, delta.z },
							  sim::query_mask::kProjectile, &hit);
			if (!blocked || !std::isfinite(hit.fraction)) {
				return;
			}
			const float reach = std::clamp(hit.fraction, 0.0f, 1.0f) * length_;
			length_ = std::max(shortest, reach);
			grow_speed_ = 0.0f;
			if (reach >= shortest || radius_ <= thinnest) {
				return;
			}
			radius_ = thinnest;
		}
	};
	pill_accumulated_ = std::min(pill_accumulated_ + std::max(0.0, p_dt), double(kChaseMaxAccumulated));
	while (pill_accumulated_ >= double(kChasePillStep)) {
		pill_accumulated_ -= double(kChasePillStep);
		grow_speed_ += std::max(0.0f, def_.pill_expand_size_speed_acceleration) * kChasePillStep;
		length_ = std::min(longest, length_ + grow_speed_ * kChasePillStep);
		radius_ = std::min(widest, radius_ + grow_speed_ * kChasePillStep);
		collide();
	}
	if (!(p_dt > 0.0)) {
		collide();
	}
	wanted_ = add(p_frame.pivot, mul(p_frame.direction, radius_ + length_));
}

void ChaseCamera::dynamics(const ChaseFrame &p_frame, const ChaseTarget &p_target) {
	const float h = 1.0f / update_rate(def_);
	const Vec3 up = normalized(cross(p_frame.forward, p_frame.right), Vec3{ 0, 1, 0 });
	const Vec3 d = sub(wanted_, position_);
	Vec3 force = add(add(mul(p_frame.right, dot(p_frame.right, d) * def_.to_wanted_position_scale.x),
							 mul(up, dot(up, d) * def_.to_wanted_position_scale.y)),
			mul(p_frame.forward, dot(p_frame.forward, d) * def_.to_wanted_position_scale.z));
	const Vec3 away = sub(position_, p_frame.pivot);
	const float away2 = dot(away, away);
	if (away2 > 1e-8f) {
		force = add(force, mul(away, def_.away_from_target_force_scale / (away2 * away2)));
	}
	velocity_ = add(velocity_, mul(sub(force, mul(velocity_, def_.velocity_drag)), h));
	const float speed = length(velocity_);
	if (def_.max_velocity >= 0.0f && speed > def_.max_velocity && speed > 0.0f) {
		velocity_ = mul(velocity_, def_.max_velocity / speed);
	}
	previous_ = position_;
	position_ = add(position_, mul(velocity_, h));
	if (def_.inherit_target_velocity) {
		position_ = add(position_, mul(p_target.velocity, h));
	}
	if (def_.snap_distance != 0.0f) {
		const Vec3 off = sub(position_, wanted_);
		const float l = length(off);
		if (l > def_.snap_distance && l > 0.0f) {
			position_ = add(wanted_, mul(off, def_.snap_distance / l));
		}
	}
}

fx::Transform ChaseCamera::view(const ChaseFrame &p_frame) const {
	const float t = std::clamp(float(accumulated_) * update_rate(def_), 0.0f, 1.0f);
	const Vec3 eye = lerp(previous_, position_, t);
	const Vec3 rel = sub(eye, p_frame.pivot);
	const float rel2 = dot(rel, rel);
	Vec3 look = add(p_frame.pivot, mul(p_frame.forward, def_.look_distance_in_front_of_target));
	if (rel2 > 1e-8f) {
		look = add(look, mul(p_frame.forward, def_.look_distance_scale / rel2));
	}
	return look_from(eye, normalized(sub(look, eye), p_frame.forward));
}

fx::Transform ChaseCamera::step(double p_dt, const ChaseTarget &p_target, const sim::CollisionWorld *p_world) {
	const ChaseFrame c = chase_frame(def_, p_target);
	const double dt = std::isfinite(p_dt) ? std::max(0.0, p_dt) : 0.0;
	pill(c, dt, p_world);
	const double h = 1.0 / double(update_rate(def_));
	accumulated_ = std::min(accumulated_ + dt, double(kChaseMaxAccumulated));
	while (accumulated_ >= h) {
		accumulated_ -= h;
		dynamics(c, p_target);
	}
	return view(c);
}

DeathCameraParams death_camera_params(const core::gameplay::DeathTimeline &p_death,
		const core::gameplay::SoldierCameras *p_cameras, const core::gameplay::PlayerCameraDef *p_killer_camera) {
	DeathCameraParams p;
	p.killer_camera_s = p_death.kill_card_s;
	if (p_killer_camera == nullptr) {
		return p;
	}
	p.fade_out_s = p_killer_camera->fade_out_s;
	p.fade_in_s = p_killer_camera->fade_in_s;
	p.release_if_target_lost = p_killer_camera->release_if_target_lost;
	const core::gameplay::SoldierCameraDef *c = p_cameras != nullptr ? p_cameras->target_of(*p_killer_camera) : nullptr;
	if (c != nullptr && c->kind == core::gameplay::SoldierCameraKind::Chase) {
		p.has_chase = true;
		p.chase = c->chase;
	}
	return p;
}

Bf3DeathCamera::Bf3DeathCamera(const DeathCameraParams &p_params) : params_(p_params), chase_(p_params.chase) {
	if (!std::isfinite(params_.killer_camera_s) || params_.killer_camera_s < 0.0f) {
		params_.killer_camera_s = DeathCameraParams{}.killer_camera_s;
	}
	if (!std::isfinite(params_.fade_out_s) || params_.fade_out_s < 0.0f) {
		params_.fade_out_s = 0.0f;
	}
	if (!std::isfinite(params_.fade_in_s) || params_.fade_in_s < 0.0f) {
		params_.fade_in_s = 0.0f;
	}
}

void Bf3DeathCamera::begin(const DeathCameraStart &p_start, double p_now_s) {
	++stats_.views;
	start_ = p_start;
	start_s_ = last_s_ = p_now_s;
	taken_ = false;
	active_ = std::isfinite(p_now_s);
	if (!active_) {
		++stats_.bad_inputs;
	}
}

void Bf3DeathCamera::end() {
	active_ = false;
	taken_ = false;
}

bool Bf3DeathCamera::active() const {
	return active_;
}

DeathCameraView Bf3DeathCamera::step(double p_now_s, const ChaseTarget *p_killer, const sim::CollisionWorld *p_world) {
	DeathCameraView v;
	if (!active_) {
		return v;
	}
	if (!std::isfinite(p_now_s)) {
		++stats_.bad_inputs;
		end();
		return v;
	}
	const double dt = std::max(0.0, p_now_s - last_s_);
	last_s_ = p_now_s;
	if (!params_.has_chase || !start_.killer_camera || start_.killer_slot == 0xFF ||
			p_now_s - start_s_ < double(params_.killer_camera_s)) {
		return v;
	}
	const bool usable = p_killer != nullptr && finite(p_killer->eye) && std::isfinite(p_killer->yaw_rad) &&
			std::isfinite(p_killer->pitch_rad) && finite(p_killer->velocity);
	if (p_killer != nullptr && !usable) {
		++stats_.bad_inputs;
	}
	if (!usable) {
		if (taken_ && params_.release_if_target_lost) {
			++stats_.released_lost;
			end();
		} else if (taken_) {
			v.active = true;
			v.camera = last_;
			v.black = black_at(p_now_s);
		}
		return v;
	}
	fx::Transform t;
	if (!taken_) {
		taken_ = true;
		taken_s_ = p_now_s;
		++stats_.killer_views;
		chase_.activate(*p_killer, p_world);
		t = chase_.step(0.0, *p_killer, p_world);
	} else {
		t = chase_.step(dt, *p_killer, p_world);
	}
	if (!finite(t.trans) || !finite(t.forward)) {
		++stats_.bad_inputs;
		end();
		return v;
	}
	last_ = t;
	v.active = true;
	v.camera = t;
	v.black = black_at(p_now_s);
	return v;
}

float Bf3DeathCamera::black_at(double p_now_s) const {
	const double since = p_now_s - taken_s_;
	if (since < double(params_.fade_out_s)) {
		return std::clamp(float(since / double(params_.fade_out_s)), 0.0f, 1.0f);
	}
	if (params_.fade_in_s <= 0.0f) {
		return 0.0f;
	}
	return 1.0f - std::clamp(float((since - double(params_.fade_out_s)) / double(params_.fade_in_s)), 0.0f, 1.0f);
}
}
