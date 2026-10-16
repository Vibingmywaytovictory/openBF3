// Hit targets: exact segment-vs-capsule tests and the stationary stand-in layout (docs/design/m2a-weapons.md §5.7).
#include "bf3/sim/weapons/hit_target.h"

#include "bf3/sim/weapons/surface_materials.h"
#include "weapons_internal.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <optional>
#include <string>

namespace bf3::sim::weapons {

namespace {

using detail::add;
using detail::dot;
using detail::scale;
using detail::sub;

// The earliest t in [0, 1] where A + t D enters the sphere (p_center, r); A outside it.
std::optional<double> segment_sphere(const DVec3 &p_a, const DVec3 &p_d, const DVec3 &p_center, double p_r) {
	const DVec3 m = sub(p_a, p_center);
	const double a = dot(p_d, p_d);
	const double b = dot(m, p_d);
	const double c = dot(m, m) - p_r * p_r;
	if (a <= 0) {
		return std::nullopt;
	}
	const double disc = b * b - a * c;
	if (disc < 0) {
		return std::nullopt;
	}
	const double t = (-b - std::sqrt(disc)) / a;
	if (t < 0 || t > 1) {
		return std::nullopt;
	}
	return t;
}

// Squared distance from p_p to the segment p_a..p_b and the closest point.
double point_segment_sq(const DVec3 &p_p, const DVec3 &p_a, const DVec3 &p_b, DVec3 *r_closest = nullptr) {
	const DVec3 ab = sub(p_b, p_a);
	const double l2 = dot(ab, ab);
	double t = l2 > 0 ? dot(sub(p_p, p_a), ab) / l2 : 0.0;
	t = std::clamp(t, 0.0, 1.0);
	const DVec3 c = add(p_a, scale(ab, t));
	if (r_closest != nullptr) {
		*r_closest = c;
	}
	const DVec3 d = sub(p_p, c);
	return dot(d, d);
}

// Squared distance between the segments p..q and a..b (closest points, Ericson 5.1.9), used by the bound test.
double segment_segment_sq(const DVec3 &p_p, const DVec3 &p_q, const DVec3 &p_a, const DVec3 &p_b) {
	const DVec3 d1 = sub(p_q, p_p);
	const DVec3 d2 = sub(p_b, p_a);
	const DVec3 r = sub(p_p, p_a);
	const double a = dot(d1, d1);
	const double e = dot(d2, d2);
	const double f = dot(d2, r);
	double s = 0, t = 0;
	if (a <= 1e-18 && e <= 1e-18) {
		return dot(r, r);
	}
	if (a <= 1e-18) {
		t = std::clamp(f / e, 0.0, 1.0);
	} else {
		const double c = dot(d1, r);
		if (e <= 1e-18) {
			s = std::clamp(-c / a, 0.0, 1.0);
		} else {
			const double b = dot(d1, d2);
			const double denom = a * e - b * b;
			s = denom > 0 ? std::clamp((b * f - c * e) / denom, 0.0, 1.0) : 0.0;
			t = (b * s + f) / e;
			if (t < 0) {
				t = 0;
				s = std::clamp(-c / a, 0.0, 1.0);
			} else if (t > 1) {
				t = 1;
				s = std::clamp((b - c) / a, 0.0, 1.0);
			}
		}
	}
	const DVec3 c1 = add(p_p, scale(d1, s));
	const DVec3 c2 = add(p_a, scale(d2, t));
	const DVec3 d = sub(c1, c2);
	return dot(d, d);
}

enum class Role : uint8_t { Head, Chest, LowerSpine, Arm, Thigh, Shin };

struct Placed {
	Vec3 a{};
	Vec3 b{};
};

// The [inferred] stand-in layout (docs §5.7 table), metres in the soldier frame (x left, y up, z forward) for the
// data's poses (stand eye 1.6, crouch eye 1.0, prone 0.3); p_side is +1 left, -1 right.
Placed layout(Pose p_pose, Role p_role, float p_side) {
	switch (p_pose) {
		case Pose::Stand:
			switch (p_role) {
				case Role::Head:
					return { { 0, 1.65f, 0 }, { 0, 1.65f, 0 } };
				case Role::Chest:
					return { { 0, 1.30f, 0 }, { 0, 1.30f, 0 } };
				case Role::LowerSpine:
					return { { 0, 1.00f, 0 }, { 0, 1.10f, 0 } };
				case Role::Arm:
					return { { 0.22f * p_side, 1.40f, 0 }, { 0.22f * p_side, 1.40f, 0.28f } };
				case Role::Thigh:
					return { { 0.10f * p_side, 0.92f, 0 }, { 0.10f * p_side, 0.52f, 0 } };
				case Role::Shin:
					return { { 0.10f * p_side, 0.48f, 0 }, { 0.10f * p_side, 0.05f, 0 } };
			}
			break;
		case Pose::Crouch:
			switch (p_role) {
				case Role::Head:
					return { { 0, 1.05f, 0.10f }, { 0, 1.05f, 0.10f } };
				case Role::Chest:
					return { { 0, 0.80f, 0.05f }, { 0, 0.80f, 0.05f } };
				case Role::LowerSpine:
					return { { 0, 0.55f, 0 }, { 0, 0.65f, 0 } };
				case Role::Arm:
					return { { 0.22f * p_side, 0.85f, 0 }, { 0.22f * p_side, 0.85f, 0.28f } };
				case Role::Thigh:
					return { { 0.12f * p_side, 0.45f, 0 }, { 0.12f * p_side, 0.45f, 0.40f } };
				case Role::Shin:
					return { { 0.12f * p_side, 0.45f, 0.40f }, { 0.12f * p_side, 0.05f, 0.40f } };
			}
			break;
		case Pose::Prone:
			switch (p_role) {
				case Role::Head:
					return { { 0, 0.25f, 0.95f }, { 0, 0.25f, 0.95f } };
				case Role::Chest:
					return { { 0, 0.20f, 0.55f }, { 0, 0.20f, 0.55f } };
				case Role::LowerSpine:
					return { { 0, 0.18f, 0.20f }, { 0, 0.18f, 0.30f } };
				case Role::Arm:
					return { { 0.20f * p_side, 0.12f, 0.65f }, { 0.20f * p_side, 0.12f, 0.93f } };
				case Role::Thigh:
					return { { 0.12f * p_side, 0.12f, -0.05f }, { 0.12f * p_side, 0.12f, -0.45f } };
				case Role::Shin:
					return { { 0.12f * p_side, 0.10f, -0.50f }, { 0.12f * p_side, 0.10f, -0.93f } };
			}
			break;
	}
	return {};
}

constexpr float kLayoutEye[kPoseCount] = { 1.6f, 1.0f, 0.3f };

std::string lower(std::string_view p_s) {
	std::string o(p_s);
	for (char &c : o) {
		c = char(std::tolower(static_cast<unsigned char>(c)));
	}
	return o;
}

} // namespace

bool segment_capsule(const Vec3 &p_from, const Vec3 &p_to, const ZoneCapsule &p_capsule, float &r_fraction, Vec3 &r_normal) {
	using namespace detail;
	const DVec3 a = to_d(p_from);
	const DVec3 d = sub(to_d(p_to), a);
	const DVec3 c0 = to_d(p_capsule.a);
	const DVec3 c1 = to_d(p_capsule.b);
	const double r = double(p_capsule.radius);
	if (!(r > 0) || !finite3(a) || !finite3(d) || !finite3(c0) || !finite3(c1)) {
		return false;
	}
	// A start inside the capsule hits at 0, the normal opposite the segment.
	if (point_segment_sq(a, c0, c1) <= r * r) {
		r_fraction = 0.0f;
		r_normal = to_f(normalize(scale(d, -1.0), { 0, 1, 0 }));
		return true;
	}
	double best = 2.0;
	DVec3 best_normal{ 0, 1, 0 };
	const DVec3 ab = sub(c1, c0);
	const double len = length(ab);
	if (len > 1e-9) {
		const DVec3 u = scale(ab, 1.0 / len);
		const DVec3 ao = sub(a, c0);
		const DVec3 dp = sub(d, scale(u, dot(d, u)));
		const DVec3 op = sub(ao, scale(u, dot(ao, u)));
		const double qa = dot(dp, dp);
		const double qb = dot(op, dp);
		const double qc = dot(op, op) - r * r;
		if (qa > 0) {
			const double disc = qb * qb - qa * qc;
			if (disc >= 0) {
				const double t = (-qb - std::sqrt(disc)) / qa;
				const double s = dot(add(ao, scale(d, t)), u);
				if (t >= 0 && t <= 1 && s >= 0 && s <= len) {
					best = t;
					best_normal = normalize(add(op, scale(dp, t)), { 0, 1, 0 });
				}
			}
		}
	}
	for (const DVec3 &end : { c0, c1 }) {
		if (const std::optional<double> t = segment_sphere(a, d, end, r); t && *t < best) {
			best = *t;
			best_normal = normalize(sub(add(a, scale(d, *t)), end), { 0, 1, 0 });
		}
	}
	if (best > 1.0) {
		return false;
	}
	r_fraction = float(best);
	r_normal = to_f(best_normal);
	return true;
}

bool segment_target(const Vec3 &p_from, const Vec3 &p_to, const HitTarget &p_target, TargetHit &r_hit) {
	using namespace detail;
	if (p_target.capsule_count == 0 || p_target.capsule_count > kMaxZoneCapsules) {
		return false;
	}
	const DVec3 a = to_d(p_from);
	const DVec3 b = to_d(p_to);
	const DVec3 c = to_d(p_target.bound_center);
	const double br = double(p_target.bound_radius);
	if (segment_segment_sq(a, b, c, c) > br * br) {
		return false;
	}
	bool hit = false;
	float best = 2.0f;
	for (uint8_t i = 0; i < p_target.capsule_count; ++i) {
		const ZoneCapsule &cap = p_target.capsules[i];
		float f = 0;
		Vec3 n{};
		if (segment_capsule(p_from, p_to, cap, f, n) && f < best) { // strict: ties keep the lower index
			best = f;
			hit = true;
			r_hit.fraction = f;
			r_hit.normal = n;
			r_hit.position = to_f(add(a, scale(sub(b, a), double(f))));
			r_hit.target = p_target.id;
			r_hit.capsule = cap.capsule;
			r_hit.zone = cap.zone;
			r_hit.property = cap.property;
			r_hit.capsule_flags = cap.flags;
		}
	}
	return hit;
}

core::Result<HitTarget> standin_target(const core::gameplay::SoldierDef &p_soldier, std::span<const core::gameplay::HitCapsule> p_capsules,
		const SurfaceMaterialTable &p_materials, Pose p_pose, const Vec3 &p_feet, uint16_t p_yaw, uint32_t p_id, uint32_t p_team) {
	using namespace detail;
	const size_t pose_index = std::min<size_t>(size_t(p_pose), kPoseCount - 1);
	const core::gameplay::PoseDef *pose = p_soldier.pose(core::gameplay::SoldierPose(pose_index));
	if (pose == nullptr || !(pose->eye_height > 0) || !std::isfinite(pose->eye_height)) {
		return core::make_error(std::string("standin_target: the soldier has no ") + core::gameplay::soldier_pose_name(core::gameplay::SoldierPose(pose_index)) +
				" pose with an eye height");
	}
	const float yscale = pose->eye_height / kLayoutEye[pose_index];
	HitTarget t;
	t.id = p_id;
	t.team = p_team;
	double sy, cy;
	poly_sincos(double(yaw_radians(p_yaw)), sy, cy);
	const DVec3 forward{ sy, 0, cy };
	const DVec3 left{ cy, 0, -sy };
	const DVec3 feet = to_d(p_feet);
	auto world = [&](const Vec3 &p_local) {
		return to_f(add(feet, add(add(scale(left, double(p_local[0])), DVec3{ 0, double(p_local[1]) * double(yscale), 0 }), scale(forward, double(p_local[2])))));
	};
	size_t arms = 0, thighs = 0, shins = 0;
	for (size_t i = 0; i < p_capsules.size(); ++i) {
		const core::gameplay::HitCapsule &cap = p_capsules[i];
		if (!cap.valid_hi_lod) {
			continue;
		}
		if (t.capsule_count >= kMaxZoneCapsules) {
			return core::make_error("standin_target: more than " + std::to_string(kMaxZoneCapsules) + " hi-LOD capsules");
		}
		if (!std::isfinite(cap.radius) || !(cap.radius > 0) || cap.radius > 2.0f) {
			return core::make_error("standin_target: capsule " + cap.bone + " has radius " + std::to_string(cap.radius));
		}
		const SurfacePair pair = p_materials.pair_of(cap.material);
		std::optional<HitZone> zone;
		for (size_t z = 0; z < kHitZoneCount && pair.resolved; ++z) {
			if (p_materials.zone_property(HitZone(z)) == pair.property) {
				zone = HitZone(z);
				break;
			}
		}
		if (!zone) {
			return core::make_error("standin_target: capsule " + cap.bone + " has no zone property");
		}
		const std::string name = lower(cap.bone);
		Role role = Role::Arm;
		size_t *counter = &arms;
		switch (*zone) {
			case HitZone::Head:
				role = Role::Head;
				counter = nullptr;
				break;
			case HitZone::Chest:
				role = Role::Chest;
				counter = nullptr;
				break;
			case HitZone::Body:
				role = cap.radius >= 0.15f ? Role::LowerSpine : Role::Arm;
				counter = role == Role::Arm ? &arms : nullptr;
				break;
			case HitZone::Legs:
				role = cap.radius >= 0.105f ? Role::Thigh : Role::Shin;
				counter = role == Role::Thigh ? &thighs : &shins;
				break;
		}
		float side = 1.0f;
		if (counter != nullptr) {
			if (name.find("left") != std::string::npos) {
				side = 1.0f;
			} else if (name.find("right") != std::string::npos) {
				side = -1.0f;
			} else {
				side = (*counter % 2) == 0 ? 1.0f : -1.0f;
			}
			++*counter;
		}
		const Placed pl = layout(Pose(pose_index), role, side);
		ZoneCapsule &z = t.capsules[t.capsule_count++];
		z.a = world(pl.a);
		z.b = world(pl.b);
		z.radius = cap.radius;
		z.zone = *zone;
		z.capsule = uint8_t(std::min<size_t>(i, 255));
		z.property = pair.property;
		z.flags = cap.deactivate_if_behind_wall ? zone_capsule_flags::kDeactivateIfBehindWall : uint8_t(0);
	}
	if (t.capsule_count == 0) {
		return core::make_error("standin_target: no hi-LOD capsule");
	}
	DVec3 lo = to_d(t.capsules[0].a), hi = lo;
	for (uint8_t i = 0; i < t.capsule_count; ++i) {
		for (const Vec3 &e : { t.capsules[i].a, t.capsules[i].b }) {
			for (size_t k = 0; k < 3; ++k) {
				lo[k] = std::min(lo[k], double(e[k]));
				hi[k] = std::max(hi[k], double(e[k]));
			}
		}
	}
	const DVec3 center = scale(add(lo, hi), 0.5);
	double radius = 0;
	for (uint8_t i = 0; i < t.capsule_count; ++i) {
		for (const Vec3 &e : { t.capsules[i].a, t.capsules[i].b }) {
			radius = std::max(radius, length(sub(to_d(e), center)) + double(t.capsules[i].radius));
		}
	}
	t.bound_center = to_f(center);
	t.bound_radius = float(radius) + 1e-3f;
	return t;
}

bool zone_aim_point(const HitTarget &p_target, HitZone p_zone, Vec3 &r_point) {
	for (uint8_t i = 0; i < p_target.capsule_count && i < kMaxZoneCapsules; ++i) {
		const ZoneCapsule &c = p_target.capsules[i];
		if (c.zone == p_zone) {
			r_point = { 0.5f * (c.a[0] + c.b[0]), 0.5f * (c.a[1] + c.b[1]), 0.5f * (c.a[2] + c.b[2]) };
			return true;
		}
	}
	return false;
}

} // namespace bf3::sim::weapons
