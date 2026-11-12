#include "bf3/sim/vehicles/vehicle_targets.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace bf3::sim::vehicles {

namespace {

float dot(const Vec3 &p_a, const Vec3 &p_b) {
	return p_a[0] * p_b[0] + p_a[1] * p_b[1] + p_a[2] * p_b[2];
}

std::array<float, 8> spread(float p_half, float p_radius, size_t p_count) {
	std::array<float, 8> out{};
	if (p_count <= 1) {
		return out;
	}
	const float reach = std::max(0.0f, p_half - p_radius);
	for (size_t i = 0; i < p_count; ++i) {
		out[i] = -reach + 2.0f * reach * float(i) / float(p_count - 1);
	}
	return out;
}

size_t rows(float p_half, float p_radius) {
	return std::clamp<size_t>(size_t(std::ceil(p_half / p_radius - 1e-3f)), 1, 8);
}

}

HullBox chassis_box(const VehicleModel &p_model) {
	Vec3 lo{ std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity(),
		std::numeric_limits<float>::infinity() };
	Vec3 hi{ -lo[0], -lo[1], -lo[2] };
	for (const ChassisPiece &piece : p_model.chassis) {
		for (const Vec3 &p : piece.points) {
			for (size_t k = 0; k < 3; ++k) {
				lo[k] = std::min(lo[k], p[k] - piece.convex_radius);
				hi[k] = std::max(hi[k], p[k] + piece.convex_radius);
			}
		}
	}
	HullBox box;
	if (!(lo[0] <= hi[0] && lo[1] <= hi[1] && lo[2] <= hi[2])) {
		return box;
	}
	for (size_t k = 0; k < 3; ++k) {
		box.centre[k] = 0.5f * (lo[k] + hi[k]);
		box.half[k] = std::max(0.05f, 0.5f * (hi[k] - lo[k]));
	}
	return box;
}

weapons::HitTarget hull_target(const HullBox &p_box, const core::Transform &p_hull, uint32_t p_id, uint8_t p_property) {
	weapons::HitTarget t;
	t.id = p_id;
	const float radius = std::min(p_box.half[0], p_box.half[1]);
	const size_t nx = rows(p_box.half[0], radius);
	const size_t ny = std::min(rows(p_box.half[1], radius), weapons::kMaxZoneCapsules / nx);
	const std::array<float, 8> xs = spread(p_box.half[0], radius, nx);
	const std::array<float, 8> ys = spread(p_box.half[1], radius, ny);
	const float reach = std::max(0.0f, p_box.half[2] - radius);
	for (size_t i = 0; i < nx; ++i) {
		for (size_t j = 0; j < ny; ++j) {
			weapons::ZoneCapsule &c = t.capsules[t.capsule_count];
			const Vec3 a{ p_box.centre[0] + xs[i], p_box.centre[1] + ys[j], p_box.centre[2] - reach };
			const Vec3 b{ a[0], a[1], p_box.centre[2] + reach };
			c.a = p_hull.transform_point(a);
			c.b = p_hull.transform_point(b);
			c.radius = radius;
			c.zone = weapons::HitZone::Body;
			c.capsule = t.capsule_count;
			c.property = p_property;
			++t.capsule_count;
		}
	}
	t.bound_center = p_hull.transform_point(p_box.centre);
	t.bound_radius = std::sqrt(dot(p_box.half, p_box.half)) + 0.01f;
	return t;
}

Vec3 to_hull(const core::Transform &p_hull, const Vec3 &p_world) {
	const Vec3 d{ p_world[0] - p_hull.trans[0], p_world[1] - p_hull.trans[1], p_world[2] - p_hull.trans[2] };
	return to_hull_direction(p_hull, d);
}

Vec3 to_hull_direction(const core::Transform &p_hull, const Vec3 &p_world) {
	return { dot(p_world, p_hull.right), dot(p_world, p_hull.up), dot(p_world, p_hull.forward) };
}

std::span<const weapons::HitTarget> JoinedTargets::targets_for(const weapons::TargetQuery &p_query) const {
	const std::span<const weapons::HitTarget> first = first_.targets_for(p_query);
	if (vehicles_.empty()) {
		return first;
	}
	joined_.assign(first.begin(), first.end());
	for (const VehicleTarget &v : vehicles_) {
		const auto crew_end = v.crew.begin() + std::min<size_t>(v.crew_count, kMaxVehicleTargetCrew);
		if (std::find(v.crew.begin(), crew_end, p_query.player_slot) == crew_end) {
			joined_.push_back(v.target);
		}
	}
	return joined_;
}

}
