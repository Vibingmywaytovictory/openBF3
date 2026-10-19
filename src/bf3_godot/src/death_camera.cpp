// Bf3DeathCamera: the own death's view (docs/design/m2b.md §4, §2.6 #17; owner cam; as built: docs/design/m2b-cam.md).
// See death_camera.h. Godot-free: m2b.contract2 compiles this file into its test, and the GDExtension builds it beside
// the M2b session (gx), which copies the view's transform onto its Camera3D.
//
// The view over the presentation clock t (seconds since begin):
//   [0, orbit)        the camera leaves the 1p eye for a point behind the corpse (back_m against the death's view yaw,
//                     up_m above the feet) while its look point moves from straight ahead to the corpse (eye_height_m
//                     above the feet); smoothstep in both;
//   [orbit, +turn)    with the killer camera on and the killer drawn (a position seen at least once): the camera swings
//                     to the point behind the corpse on the killer's side (back_m from the corpse, away from the
//                     killer), and the look point moves from the corpse to the killer (killer_height_m above its feet);
//                     then it follows the killer's presented position (the last one seen when it is no longer drawn);
//   >= seconds        finished (the game moves on to the spawn screen); the view holds until end().
// Without a killer it stays on the corpse. With a CollisionWorld the camera is pulled in along the ray from the corpse
// look point to the camera, camera_radius_m short of the first static hit (query_mask::kProjectile: what bullets and
// sight see). Every input is checked: a non-finite start or clock ends the view, a non-finite killer position is ignored;
// both are counted, and the transform is always finite and orthonormal.
#include "death_camera.h"

#include "bf3/sim/collision_world.h"
#include "bf3/sim/static_world.h"

#include <algorithm>
#include <cmath>

namespace bf3::godot_side {

namespace {

using fx::Vec3;

constexpr float kAheadM = 5.0f; // the 1p look point at the start of the view (straight ahead of the eye)
constexpr float kMinPivotM = 0.3f; // the pull-in never puts the camera closer than this to the corpse look point

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
	return l > 1e-5f && std::isfinite(l) ? mul(p_a, 1.0f / l) : p_fallback;
}
Vec3 lerp(const Vec3 &p_a, const Vec3 &p_b, float p_t) {
	return add(p_a, mul(sub(p_b, p_a), p_t));
}
bool finite3(const Vec3 &p_a) {
	return std::isfinite(p_a.x) && std::isfinite(p_a.y) && std::isfinite(p_a.z);
}
// 0..1 over [0, p_span] (p_span <= 0: already at 1), eased in and out.
float ease(double p_t, float p_span) {
	if (!(p_span > 0.0f)) {
		return 1.0f;
	}
	const float u = std::clamp(float(p_t / double(p_span)), 0.0f, 1.0f);
	return u * u * (3.0f - 2.0f * u);
}
float sane(float p_value, float p_default, float p_lo, float p_hi) {
	return std::isfinite(p_value) && p_value >= p_lo && p_value <= p_hi ? p_value : p_default;
}

// The m2a_frame_of rule (level_session_m2a.cpp): right = up x forward, up = forward x right.
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

} // namespace

Bf3DeathCamera::Bf3DeathCamera(const DeathCameraParams &p_params) :
		params_(p_params) {
	// Hostile or careless params fall back to the named defaults, one by one (never a NaN view).
	const DeathCameraParams d{};
	params_.seconds = sane(params_.seconds, d.seconds, 0.05f, 600.0f);
	params_.orbit_seconds = sane(params_.orbit_seconds, d.orbit_seconds, 0.0f, 600.0f);
	params_.turn_seconds = sane(params_.turn_seconds, d.turn_seconds, 0.0f, 600.0f);
	params_.back_m = sane(params_.back_m, d.back_m, 0.0f, 100.0f);
	params_.up_m = sane(params_.up_m, d.up_m, -10.0f, 100.0f);
	params_.eye_height_m = sane(params_.eye_height_m, d.eye_height_m, -10.0f, 10.0f);
	params_.camera_radius_m = sane(params_.camera_radius_m, d.camera_radius_m, 0.0f, 10.0f);
	params_.killer_height_m = sane(params_.killer_height_m, d.killer_height_m, -10.0f, 10.0f);
}

void Bf3DeathCamera::begin(const DeathCameraStart &p_start, double p_now_s) {
	++stats_.views;
	start_ = p_start;
	start_s_ = p_now_s;
	killer_seen_ = false;
	last_killer_ = fx::Vec3{};
	turned_counted_ = false;
	active_ = finite3(p_start.corpse) && finite3(p_start.eye) && std::isfinite(p_start.yaw_rad) && std::isfinite(p_now_s);
	if (!active_) {
		++stats_.bad_inputs;
	}
}

void Bf3DeathCamera::end() {
	active_ = false;
}

bool Bf3DeathCamera::active() const {
	return active_;
}

DeathCameraView Bf3DeathCamera::step(double p_now_s, const fx::Vec3 *p_killer, const sim::CollisionWorld *p_world) {
	DeathCameraView v;
	if (!active_) {
		return v;
	}
	if (!std::isfinite(p_now_s)) {
		++stats_.bad_inputs;
		active_ = false;
		return v;
	}
	const double t = std::max(0.0, p_now_s - start_s_);
	const DeathCameraParams &p = params_;
	const Vec3 fwd{ std::sin(start_.yaw_rad), 0.0f, std::cos(start_.yaw_rad) }; // sim yaw: 0 looks down +Z
	const Vec3 up{ 0.0f, 1.0f, 0.0f };
	const Vec3 pivot = add(start_.corpse, mul(up, p.eye_height_m));
	const Vec3 orbit = add(sub(start_.corpse, mul(fwd, p.back_m)), mul(up, p.up_m));
	const bool want_killer = start_.killer_camera && start_.killer_slot != 0xFF;
	if (want_killer && p_killer != nullptr) {
		if (finite3(*p_killer)) {
			last_killer_ = *p_killer;
			killer_seen_ = true;
		} else {
			++stats_.bad_inputs; // ignored: the last good position (or the corpse) stays the target
		}
	}
	// 1. Out of the eye, behind the corpse, looking at it.
	const float s = ease(t, p.orbit_seconds);
	Vec3 pos = lerp(start_.eye, orbit, s);
	Vec3 target = lerp(add(start_.eye, mul(fwd, kAheadM)), pivot, s);
	// 2. The turn to the killer.
	if (want_killer && killer_seen_ && t >= double(p.orbit_seconds)) {
		v.on_killer = true;
		if (!turned_counted_) {
			turned_counted_ = true;
			++stats_.killer_views;
		}
		const float k = ease(t - double(p.orbit_seconds), p.turn_seconds);
		const Vec3 killer = add(last_killer_, mul(up, p.killer_height_m));
		Vec3 flat = sub(killer, start_.corpse);
		flat.y = 0.0f;
		const Vec3 dir = normalized(flat, fwd);
		const Vec3 over = add(sub(start_.corpse, mul(dir, p.back_m)), mul(up, p.up_m));
		pos = lerp(orbit, over, k);
		target = lerp(pivot, killer, k);
	}
	// 3. Out of the walls: along the ray from the corpse look point.
	if (p_world != nullptr) {
		const Vec3 delta = sub(pos, pivot);
		const float dist = length(delta);
		sim::StaticHit hit;
		if (dist > kMinPivotM && p_world->ray(sim::Vec3{ pivot.x, pivot.y, pivot.z }, sim::Vec3{ delta.x, delta.y, delta.z }, sim::query_mask::kProjectile, &hit) &&
				std::isfinite(hit.fraction)) {
			const float keep = std::max(kMinPivotM, std::clamp(hit.fraction, 0.0f, 1.0f) * dist - p.camera_radius_m);
			if (keep < dist) {
				pos = add(pivot, mul(delta, keep / dist));
				++stats_.pulled_in;
			}
		}
	}
	const Vec3 forward = normalized(sub(target, pos), normalized(sub(pivot, pos), fwd));
	if (!finite3(pos) || !finite3(forward)) {
		++stats_.bad_inputs; // unreachable with finite inputs; never a NaN transform
		active_ = false;
		return DeathCameraView{};
	}
	v.active = true;
	v.finished = t >= double(p.seconds);
	v.camera = look_from(pos, forward);
	return v;
}

} // namespace bf3::godot_side
