// A soldier's hit target posed from the capsule pose table (docs/design/m2a.md §6.5; as built: docs/design/m2a-lc.md).
// See posed_target.h.
//
// The local capsule is the bilinear blend of the table entries around (pitch, transition fraction): the two pitch
// buckets around the pitch, and during a transition the two fractions around its progress. Then the yaw rotation of
// the soldier frame (x left, y up, z forward; standin_target's convention) through the deterministic poly_sincos, and
// the feet. Everything in double, rounded to float once per coordinate, so the server and a client that call it with
// equal inputs get equal bits.
#include "bf3/sim/weapons/posed_target.h"

#include "bf3/sim/weapons/surface_materials.h"

#include "weapons_internal.h"

#include <algorithm>
#include <cmath>

namespace bf3::sim::weapons {

namespace {

using core::gameplay::CapsuleLocal;
using core::gameplay::CapsulePoseTable;
using core::gameplay::kCapsulePitchBuckets;
using core::gameplay::kCapsuleTransitionFractions;
using namespace detail;

constexpr double kRadToDeg = 180.0 / kPiD;

struct Blend {
	size_t index[4]{}; // entry offsets (first capsule)
	double weight[4]{};
	size_t count = 0;
};

// The bucket below p_deg and the weight of the one above (p_deg clamped to the table's range).
void pitch_bucket(const CapsulePoseTable &p_table, double p_deg, size_t &r_bucket, double &r_w) {
	const double lo = double(p_table.pitch_deg.front());
	const double hi = double(p_table.pitch_deg.back());
	const double d = std::isfinite(p_deg) ? std::clamp(p_deg, lo, hi) : 0.0;
	size_t b = 0;
	size_t e = kCapsulePitchBuckets - 1; // binary search: the last bucket <= d, at most kCapsulePitchBuckets - 2
	while (e - b > 1) {
		const size_t m = (b + e) / 2;
		if (double(p_table.pitch_deg[m]) <= d) {
			b = m;
		} else {
			e = m;
		}
	}
	const double p0 = double(p_table.pitch_deg[b]);
	const double p1 = double(p_table.pitch_deg[b + 1]);
	r_bucket = b;
	r_w = p1 > p0 ? std::clamp((d - p0) / (p1 - p0), 0.0, 1.0) : 0.0;
}

} // namespace

HitTarget posed_target(const CapsulePoseTable &p_table, const TargetPose &p_pose) {
	HitTarget t;
	t.id = p_pose.id;
	t.team = p_pose.team;
	t.spawn_protected = p_pose.spawn_protected;
	const size_t n = std::min<size_t>(p_table.capsule_count, kMaxZoneCapsules);
	if (n == 0) {
		return t;
	}
	size_t pb = 0;
	double pw = 0;
	pitch_bucket(p_table, double(p_pose.pitch) * kRadToDeg, pb, pw);

	Blend bl;
	const uint8_t from = uint8_t(std::min<size_t>(size_t(p_pose.pose), core::gameplay::kCapsuleStances - 1));
	const uint8_t to = uint8_t(std::min<size_t>(size_t(p_pose.target_pose), core::gameplay::kCapsuleStances - 1));
	core::gameplay::CapsuleTransition tr{};
	if (from != to && core::gameplay::capsule_transition_of(from, to, tr)) {
		const double f = std::isfinite(p_pose.transition) ? std::clamp(double(p_pose.transition), 0.0, 1.0) : 0.0;
		const double x = f * double(kCapsuleTransitionFractions - 1);
		size_t fb = std::min<size_t>(size_t(x), kCapsuleTransitionFractions - 2);
		const double fw = std::clamp(x - double(fb), 0.0, 1.0);
		const size_t row = size_t(tr);
		bl.index[0] = p_table.transition_index(row, fb, pb);
		bl.index[1] = p_table.transition_index(row, fb, pb + 1);
		bl.index[2] = p_table.transition_index(row, fb + 1, pb);
		bl.index[3] = p_table.transition_index(row, fb + 1, pb + 1);
		bl.weight[0] = (1.0 - fw) * (1.0 - pw);
		bl.weight[1] = (1.0 - fw) * pw;
		bl.weight[2] = fw * (1.0 - pw);
		bl.weight[3] = fw * pw;
		bl.count = 4;
	} else {
		bl.index[0] = p_table.steady_index(to, pb);
		bl.index[1] = p_table.steady_index(to, pb + 1);
		bl.weight[0] = 1.0 - pw;
		bl.weight[1] = pw;
		bl.count = 2;
	}
	const std::vector<CapsuleLocal> &src = bl.count == 4 ? p_table.transition : p_table.steady;
	for (size_t k = 0; k < bl.count; ++k) {
		if (bl.index[k] + n > src.size()) {
			return t; // a table that failed capsule_pose_table_valid (caller bug): no capsules
		}
	}

	double sy, cy;
	poly_sincos(std::isfinite(p_pose.yaw) ? double(p_pose.yaw) : 0.0, sy, cy);
	const DVec3 forward{ sy, 0, cy };
	const DVec3 left{ cy, 0, -sy };
	const DVec3 feet = to_d(p_pose.feet);
	auto world = [&](const double (&p_local)[3]) {
		return to_f(add(feet, add(add(scale(left, p_local[0]), DVec3{ 0, p_local[1], 0 }), scale(forward, p_local[2]))));
	};
	for (size_t c = 0; c < n; ++c) {
		double a[3] = { 0, 0, 0 }, b[3] = { 0, 0, 0 }, r = 0;
		for (size_t k = 0; k < bl.count; ++k) {
			const CapsuleLocal &e = src[bl.index[k] + c];
			const double w = bl.weight[k];
			for (size_t i = 0; i < 3; ++i) {
				a[i] += w * double(e.a[i]);
				b[i] += w * double(e.b[i]);
			}
			r += w * double(e.radius);
		}
		const CapsuleLocal &e0 = src[bl.index[0] + c];
		ZoneCapsule &z = t.capsules[c];
		z.a = world(a);
		z.b = world(b);
		z.radius = float(r);
		z.zone = HitZone(std::min<uint8_t>(e0.zone, uint8_t(kHitZoneCount - 1)));
		z.capsule = e0.capsule;
		z.property = e0.property;
		z.flags = e0.flags;
	}
	t.capsule_count = uint8_t(n);
	// The bound sphere around every capsule (standin_target's rule).
	DVec3 lo = to_d(t.capsules[0].a), hi = lo;
	for (size_t i = 0; i < n; ++i) {
		for (const Vec3 &e : { t.capsules[i].a, t.capsules[i].b }) {
			for (size_t k = 0; k < 3; ++k) {
				lo[k] = std::min(lo[k], double(e[k]));
				hi[k] = std::max(hi[k], double(e[k]));
			}
		}
	}
	const DVec3 center = scale(add(lo, hi), 0.5);
	double radius = 0;
	for (size_t i = 0; i < n; ++i) {
		for (const Vec3 &e : { t.capsules[i].a, t.capsules[i].b }) {
			radius = std::max(radius, length(sub(to_d(e), center)) + double(t.capsules[i].radius));
		}
	}
	t.bound_center = to_f(center);
	t.bound_radius = float(radius) + 1e-3f;
	return t;
}

core::Result<CapsuleTableData> capsule_table_data(const core::gameplay::SoldierDef &p_soldier, std::span<const core::gameplay::HitCapsule> p_capsules,
		const SurfaceMaterialTable &p_materials) {
	CapsuleTableData d;
	d.property.resize(p_capsules.size(), 0);
	d.zone.resize(p_capsules.size(), uint8_t(HitZone::Body));
	for (size_t i = 0; i < p_capsules.size(); ++i) {
		const SurfacePair pair = p_materials.pair_of(p_capsules[i].material);
		if (!pair.resolved) {
			continue;
		}
		d.property[i] = pair.property;
		for (size_t z = 0; z < kHitZoneCount; ++z) {
			if (p_materials.zone_property(HitZone(z)) == pair.property) {
				d.zone[i] = uint8_t(z);
				break;
			}
		}
	}
	for (size_t s = 0; s < core::gameplay::kCapsuleStances; ++s) {
		core::Result<HitTarget> t = standin_target(p_soldier, p_capsules, p_materials, Pose(s), Vec3{ 0, 0, 0 }, 0, 0);
		if (!t) {
			return t.error();
		}
		for (uint8_t c = 0; c < t->capsule_count; ++c) {
			const ZoneCapsule &z = t->capsules[c];
			core::gameplay::CapsuleLocal e{};
			for (size_t k = 0; k < 3; ++k) {
				e.a[k] = z.a[k];
				e.b[k] = z.b[k];
			}
			e.radius = z.radius;
			e.zone = uint8_t(z.zone);
			e.capsule = z.capsule;
			e.property = z.property;
			e.flags = z.flags;
			d.standin[s].push_back(e);
		}
	}
	return d;
}

} // namespace bf3::sim::weapons
