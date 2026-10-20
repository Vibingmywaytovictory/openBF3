#include "bf3/sim/vehicle_course.h"

#include "bf3/core/level/collision_soup.h"
#include "bf3/sim/collision_world.h"
#include "collision_world_internal.h"
#include "static_world_internal.h"
#include "vehicle_world_internal.h"

#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <string>
#include <unordered_map>

namespace bf3::sim {

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kDegrees = kPi / 180.0f;
constexpr float kPlaceLift = 0.05f;

Vec3 add(const Vec3 &p_a, const Vec3 &p_b) {
	return { p_a[0] + p_b[0], p_a[1] + p_b[1], p_a[2] + p_b[2] };
}
Vec3 sub(const Vec3 &p_a, const Vec3 &p_b) {
	return { p_a[0] - p_b[0], p_a[1] - p_b[1], p_a[2] - p_b[2] };
}
Vec3 mul(const Vec3 &p_a, float p_s) {
	return { p_a[0] * p_s, p_a[1] * p_s, p_a[2] * p_s };
}
float dot(const Vec3 &p_a, const Vec3 &p_b) {
	return p_a[0] * p_b[0] + p_a[1] * p_b[1] + p_a[2] * p_b[2];
}
Vec3 cross(const Vec3 &p_a, const Vec3 &p_b) {
	return { p_a[1] * p_b[2] - p_a[2] * p_b[1], p_a[2] * p_b[0] - p_a[0] * p_b[2], p_a[0] * p_b[1] - p_a[1] * p_b[0] };
}
float length(const Vec3 &p_a) {
	return std::sqrt(dot(p_a, p_a));
}
Vec3 normalized(const Vec3 &p_a) {
	const float l = length(p_a);
	return l > 0.0f ? mul(p_a, 1.0f / l) : Vec3{ 0.0f, 0.0f, 1.0f };
}
Vec3 horizontal(const Vec3 &p_a) {
	return normalized({ p_a[0], 0.0f, p_a[2] });
}

std::array<float, 4> quat_from_basis(const Vec3 &p_x, const Vec3 &p_y, const Vec3 &p_z) {
	const float m00 = p_x[0], m11 = p_y[1], m22 = p_z[2];
	const float trace = m00 + m11 + m22;
	std::array<float, 4> q{};
	if (trace > 0.0f) {
		const float s = std::sqrt(trace + 1.0f) * 2.0f;
		q = { (p_y[2] - p_z[1]) / s, (p_z[0] - p_x[2]) / s, (p_x[1] - p_y[0]) / s, 0.25f * s };
	} else if (m00 > m11 && m00 > m22) {
		const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
		q = { 0.25f * s, (p_y[0] + p_x[1]) / s, (p_z[0] + p_x[2]) / s, (p_y[2] - p_z[1]) / s };
	} else if (m11 > m22) {
		const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
		q = { (p_y[0] + p_x[1]) / s, 0.25f * s, (p_z[1] + p_y[2]) / s, (p_z[0] - p_x[2]) / s };
	} else {
		const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
		q = { (p_z[0] + p_x[2]) / s, (p_z[1] + p_y[2]) / s, 0.25f * s, (p_x[1] - p_y[0]) / s };
	}
	const float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
	for (float &c : q) {
		c /= n;
	}
	return q;
}

VehiclePlacement place_on(const Vec3 &p_point, const Vec3 &p_normal, const Vec3 &p_heading) {
	const Vec3 up = normalized(p_normal);
	Vec3 forward = sub(p_heading, mul(up, dot(p_heading, up)));
	forward = normalized(forward);
	const Vec3 x = normalized(cross(up, forward));
	VehiclePlacement p;
	p.position = add(p_point, mul(up, kPlaceLift));
	p.rotation = quat_from_basis(x, up, forward);
	return p;
}

Vec3 forward_of(const std::array<float, 4> &p_q) {
	const float x = p_q[0], y = p_q[1], z = p_q[2], w = p_q[3];
	return { 2.0f * (x * z + w * y), 2.0f * (y * z - w * x), 1.0f - 2.0f * (x * x + y * y) };
}

struct Cell {
	bool sampled = false;
	bool hit = false;
	float y = 0;
	Vec3 normal{};
	int8_t clear = -1;
};

class Sampler {
public:
	Sampler(const CollisionWorld &p_world, float p_grid) :
			world_(p_world), grid_(p_grid) {
		const Vec3 lo = p_world.shapes()->min(), hi = p_world.shapes()->max();
		top_ = hi[1] + 5.0f;
		drop_ = (hi[1] - lo[1]) + 10.0f;
	}

	void begin_kind(size_t p_budget) {
		budget_ = p_budget;
		queries_ = 0;
		exhausted_ = false;
	}
	size_t queries() const { return queries_; }
	bool exhausted() const { return exhausted_; }
	float grid() const { return grid_; }

	const Cell *ground(int32_t p_i, int32_t p_j) {
		Cell &c = cells_[key(p_i, p_j)];
		if (!c.sampled) {
			if (!charge()) {
				return nullptr;
			}
			c.sampled = true;
			StaticHit hit;
			if (front_ray({ float(p_i) * grid_, top_, float(p_j) * grid_ }, { 0.0f, -drop_, 0.0f }, &hit) && hit.normal[1] > 0.0f) {
				c.hit = true;
				c.y = hit.position[1];
				c.normal = hit.normal;
			}
		}
		return &c;
	}

	bool clear(int32_t p_i, int32_t p_j) {
		const Cell *g = ground(p_i, p_j);
		if (g == nullptr || !g->hit) {
			return false;
		}
		Cell &c = cells_[key(p_i, p_j)];
		if (c.clear < 0) {
			if (!charge()) {
				return false;
			}
			const bool blocked = world_.overlap_box({ float(p_i) * grid_, c.y + 0.5f + 0.9f, float(p_j) * grid_ }, { 1.4f, 0.9f, 1.4f }, 0.0f,
					query_mask::kCollision, 0.01f);
			c.clear = blocked ? 0 : 1;
		}
		return c.clear == 1;
	}

	bool overlap(const Vec3 &p_center, const Vec3 &p_half) {
		if (!charge()) {
			return true;
		}
		return world_.overlap_box(p_center, p_half, 0.0f, query_mask::kCollision, 0.01f);
	}

	bool ray(const Vec3 &p_origin, const Vec3 &p_delta, StaticHit *r_hit) {
		if (!charge()) {
			return false;
		}
		return front_ray(p_origin, p_delta, r_hit);
	}

private:
	static uint64_t key(int32_t p_i, int32_t p_j) { return (uint64_t(uint32_t(p_i)) << 32) | uint32_t(p_j); }

	bool charge() {
		if (queries_ >= budget_) {
			exhausted_ = true;
			return false;
		}
		++queries_;
		return true;
	}

	bool front_ray(const Vec3 &p_origin, const Vec3 &p_delta, StaticHit *r_hit) const {
		const CollisionWorld::Impl &w = world_.impl();
		const JPH::Vec3 origin(p_origin[0], p_origin[1], p_origin[2]);
		const JPH::Vec3 delta(p_delta[0], p_delta[1], p_delta[2]);
		const JPH::RRayCast cast(JPH::RVec3(origin), delta);
		JPH::RayCastSettings settings;
		settings.SetBackFaceMode(JPH::EBackFaceMode::IgnoreBackFaces);
		JPH::ClosestHitCollisionCollector<JPH::CastRayCollector> collector;
		w.system->GetNarrowPhaseQueryNoLock().CastRay(cast, settings, collector, w.broadphase_filter(query_mask::kCollision),
				w.object_filter(query_mask::kCollision), w.body_filter, w.shape_filter);
		if (!collector.HadHit()) {
			return false;
		}
		if (r_hit != nullptr) {
			const JPH::RayCastResult &hit = collector.mHit;
			*r_hit = StaticHit();
			w.describe_hit(hit.mBodyID, hit.mSubShapeID2, *r_hit);
			r_hit->fraction = hit.mFraction;
			const JPH::Vec3 p = origin + delta * hit.mFraction;
			r_hit->position = { p.GetX(), p.GetY(), p.GetZ() };

			const JPH::BodyInterface &bi = w.system->GetBodyInterfaceNoLock();
			const uint64_t user = bi.GetUserData(hit.mBodyID);
			const uint64_t tile = user >> 2;
			const uint32_t layer = uint32_t(user & 3u);
			const StaticWorldShapes::Impl &s = w.shapes->impl();
			JPH::Vec3 n = JPH::Vec3::sAxisY();
			if (layer == kStaticTerrainBodyLayer) {
				const JPH::RefConst<JPH::Shape> shape = bi.GetShape(hit.mBodyID);
				n = static_face_normal(s, user, shape.GetPtr(), hit.mSubShapeID2);
			} else if (tile < s.tiles.size() && layer < kStaticLayerCount && s.tiles[tile].shapes[layer] != nullptr) {
				n = s.tiles[tile].shapes[layer]->GetSurfaceNormal(hit.mSubShapeID2, JPH::Vec3::sZero());
			}
			r_hit->normal = { n.GetX(), n.GetY(), n.GetZ() };
		}
		return true;
	}

	const CollisionWorld &world_;
	float grid_;
	float top_ = 0, drop_ = 0;
	size_t budget_ = 0, queries_ = 0;
	bool exhausted_ = false;
	std::unordered_map<uint64_t, Cell> cells_;
};

struct Dir {
	int32_t di, dj;
};
constexpr Dir kDirs[8] = { { 0, 1 }, { 1, 1 }, { 1, 0 }, { 1, -1 }, { 0, -1 }, { -1, -1 }, { -1, 0 }, { -1, 1 } };

float dir_step(const Dir &p_d, float p_grid) {
	return (p_d.di != 0 && p_d.dj != 0 ? 1.41421356f : 1.0f) * p_grid;
}

Vec3 dir_vec(const Dir &p_d) {
	return normalized({ float(p_d.di), 0.0f, float(p_d.dj) });
}

Vec3 cell_point(const Sampler &p_s, int32_t p_i, int32_t p_j, float p_y) {
	return { float(p_i) * p_s.grid(), p_y, float(p_j) * p_s.grid() };
}

std::vector<Dir> ring_order(float p_radius, float p_grid) {
	const int32_t r = int32_t(std::ceil(p_radius / p_grid));
	std::vector<Dir> out;
	for (int32_t di = -r; di <= r; ++di) {
		for (int32_t dj = -r; dj <= r; ++dj) {
			if (float(di * di + dj * dj) * p_grid * p_grid <= p_radius * p_radius) {
				out.push_back({ di, dj });
			}
		}
	}
	std::stable_sort(out.begin(), out.end(), [](const Dir &p_a, const Dir &p_b) {
		const int32_t a = p_a.di * p_a.di + p_a.dj * p_a.dj, b = p_b.di * p_b.di + p_b.dj * p_b.dj;
		if (a != b) {
			return a < b;
		}
		return p_a.di != p_b.di ? p_a.di < p_b.di : p_a.dj < p_b.dj;
	});
	return out;
}

struct Leg {
	int32_t i = 0, j = 0;
	Dir d{};
	int32_t cells = 0;
	float feature = 0;
};

bool check_leg(Sampler &r_s, int32_t p_i, int32_t p_j, const Dir &p_d, float p_length, float p_max_degrees, bool p_width, Leg &r_leg) {
	const float step = dir_step(p_d, r_s.grid());
	const int32_t n = int32_t(std::ceil(p_length / step));
	const float min_up = std::cos(p_max_degrees * kDegrees);
	const float max_step = step * std::tan(p_max_degrees * kDegrees) + 0.15f;
	float prev_y = 0, prev2_y = 0, crest = 0;
	for (int32_t k = 0; k <= n; ++k) {
		const int32_t ci = p_i + k * p_d.di, cj = p_j + k * p_d.dj;
		const Cell *c = r_s.ground(ci, cj);
		if (c == nullptr || !c->hit || c->normal[1] < min_up || !r_s.clear(ci, cj)) {
			return false;
		}
		if (k > 0 && std::fabs(c->y - prev_y) > max_step) {
			return false;
		}
		if (k > 1) {
			crest = std::max(crest, -(c->y - 2.0f * prev_y + prev2_y) / (step * step));
		}
		if (p_width) {

			for (const int32_t side : { -1, 1 }) {
				const int32_t si = ci - side * p_d.dj, sj = cj + side * p_d.di;
				const Cell *s = r_s.ground(si, sj);
				if (s == nullptr || !s->hit || s->normal[1] < min_up || !r_s.clear(si, sj) || std::fabs(s->y - c->y) > max_step) {
					return false;
				}
			}
		}
		prev2_y = prev_y;
		prev_y = c->y;
	}
	r_leg.i = p_i;
	r_leg.j = p_j;
	r_leg.d = p_d;
	r_leg.cells = n;
	r_leg.feature = crest;
	return true;
}

bool check_turnaround(Sampler &r_s, int32_t p_i, int32_t p_j, const Dir &p_d, float p_radius, float p_max_degrees) {
	const Vec3 dv = dir_vec(p_d);
	const float ci = float(p_i) + dv[0] * p_radius / r_s.grid();
	const float cj = float(p_j) + dv[2] * p_radius / r_s.grid();
	const int32_t r = int32_t(std::ceil(p_radius / r_s.grid()));
	const float min_up = std::cos(p_max_degrees * kDegrees);
	const int32_t i0 = int32_t(std::lround(ci)), j0 = int32_t(std::lround(cj));
	for (int32_t di = -r; di <= r; ++di) {
		for (int32_t dj = -r; dj <= r; ++dj) {
			const float x = float(i0 + di) - ci, z = float(j0 + dj) - cj;
			if ((x * x + z * z) * r_s.grid() * r_s.grid() > p_radius * p_radius) {
				continue;
			}
			const Cell *c = r_s.ground(i0 + di, j0 + dj);
			if (c == nullptr || !c->hit || c->normal[1] < min_up || !r_s.clear(i0 + di, j0 + dj)) {
				return false;
			}
		}
	}
	return true;
}

void fill_leg_course(Sampler &r_s, const Leg &p_leg, VehicleCourse &r_course) {
	const Cell *start = r_s.ground(p_leg.i, p_leg.j);
	const Cell *end = r_s.ground(p_leg.i + p_leg.cells * p_leg.d.di, p_leg.j + p_leg.cells * p_leg.d.dj);
	r_course.a = cell_point(r_s, p_leg.i, p_leg.j, start->y);
	r_course.b = cell_point(r_s, p_leg.i + p_leg.cells * p_leg.d.di, p_leg.j + p_leg.cells * p_leg.d.dj, end->y);
	Vec3 normal = start->normal, heading = dir_vec(p_leg.d);
	if (r_course.b[1] > r_course.a[1] + 0.5f) {

		std::swap(r_course.a, r_course.b);
		normal = end->normal;
		heading = { -heading[0], 0.0f, -heading[2] };
	}
	r_course.start = place_on(r_course.a, normal, heading);
	r_course.feature = p_leg.feature;
}

}

const char *vehicle_course_name(VehicleCourseKind p_kind) {
	switch (p_kind) {
		case VehicleCourseKind::Straight:
			return "straight";
		case VehicleCourseKind::Brake:
			return "brake";
		case VehicleCourseKind::Slalom:
			return "slalom";
		case VehicleCourseKind::Circle:
			return "circle";
		case VehicleCourseKind::Bumps:
			return "bumps";
		case VehicleCourseKind::Collision:
			return "collision";
		case VehicleCourseKind::SlopeIdle:
			return "slope_idle";
	}
	return "?";
}

core::Result<std::array<VehicleCourse, kVehicleCourseKindCount>> find_vehicle_courses(const CollisionWorld &p_world, std::span<const Vec3> p_seeds,
		const VehicleModel &, const VehicleCourseSearch &p_search) {
	if (!std::isfinite(p_search.radius) || !std::isfinite(p_search.grid) || p_search.grid < 1.0f || p_search.radius < p_search.grid ||
			p_search.radius > 5000.0f || !(p_search.turnaround_radius >= 0.0f && p_search.turnaround_radius <= 200.0f)) {
		return core::make_error("vehicle courses: search radius, grid or turnaround radius out of range");
	}
	if (p_seeds.size() > 256) {
		return core::make_error("vehicle courses: more than 256 seeds");
	}
	for (const Vec3 &s : p_seeds) {
		if (!std::isfinite(s[0]) || !std::isfinite(s[1]) || !std::isfinite(s[2])) {
			return core::make_error("vehicle courses: a non-finite seed");
		}
	}
	std::array<VehicleCourse, kVehicleCourseKindCount> out;
	for (size_t k = 0; k < kVehicleCourseKindCount; ++k) {
		out[k].kind = VehicleCourseKind(k);
	}
	Sampler s(p_world, p_search.grid);
	const std::vector<Dir> rings = ring_order(p_search.radius, p_search.grid);
	constexpr float kLegLength = 150.0f;
	const float grid = p_search.grid;

	for (size_t k = 0; k < kVehicleCourseKindCount; ++k) {
		VehicleCourse &course = out[k];
		const VehicleCourseKind kind = VehicleCourseKind(k);
		s.begin_kind(p_search.max_queries_per_kind);
		Leg best;
		bool have_best = false;
		uint32_t best_seed = 0;
		for (uint32_t seed = 0; seed < p_seeds.size() && !course.found && !s.exhausted(); ++seed) {
			const int32_t si = int32_t(std::lround(p_seeds[seed][0] / grid)), sj = int32_t(std::lround(p_seeds[seed][2] / grid));
			for (const Dir &o : rings) {
				if (course.found || s.exhausted()) {
					break;
				}
				const int32_t ci = si + o.di, cj = sj + o.dj;
				const Cell *c = s.ground(ci, cj);
				if (c == nullptr || !c->hit) {
					continue;
				}
				switch (kind) {
					case VehicleCourseKind::Straight:
					case VehicleCourseKind::Brake:
					case VehicleCourseKind::Slalom:
					case VehicleCourseKind::Bumps: {
						const bool bumps = kind == VehicleCourseKind::Bumps;

						const float max_deg = bumps ? 8.0f : p_search.max_flat_degrees;
						constexpr float kBumpsRoom = 16.0f;
						for (const Dir &d : kDirs) {
							Leg leg;
							if (!check_leg(s, ci, cj, d, kLegLength + (bumps ? 2.0f * kBumpsRoom : 0.0f), max_deg, kind == VehicleCourseKind::Slalom || bumps,
										leg)) {
								continue;
							}
							if (bumps && (leg.feature < 0.02f || (have_best && leg.feature <= best.feature))) {
								continue;
							}
							if (bumps) {
								const int32_t room = int32_t(std::ceil(kBumpsRoom / dir_step(d, grid)));
								leg.i += room * d.di;
								leg.j += room * d.dj;
								leg.cells -= 2 * room;
							}
							const Dir back{ -d.di, -d.dj };
							const int32_t ei = ci + leg.cells * d.di, ej = cj + leg.cells * d.dj;
							if (!bumps && (!check_turnaround(s, ei, ej, d, p_search.turnaround_radius, p_search.max_turnaround_degrees) ||
												  !check_turnaround(s, ci, cj, back, p_search.turnaround_radius, p_search.max_turnaround_degrees))) {
								continue;
							}
							if (bumps) {
								best = leg;
								have_best = true;
								best_seed = seed;
								continue;
							}
							fill_leg_course(s, leg, course);
							course.found = true;
							course.seed = seed;
							break;
						}
						break;
					}
					case VehicleCourseKind::Circle: {

						const float min_up = std::cos(p_search.max_flat_degrees * kDegrees);
						if (c->normal[1] < min_up) {
							break;
						}
						const int32_t r = int32_t(std::ceil(25.0f / grid));
						bool ok = true;
						float lo = c->y, hi = c->y;
						for (int32_t di = -r; di <= r && ok; ++di) {
							for (int32_t dj = -r; dj <= r && ok; ++dj) {
								if (float(di * di + dj * dj) * grid * grid > 25.0f * 25.0f) {
									continue;
								}
								const Cell *n = s.ground(ci + di, cj + dj);
								ok = n != nullptr && n->hit && n->normal[1] >= min_up && s.clear(ci + di, cj + dj);
								if (ok) {
									lo = std::min(lo, n->y);
									hi = std::max(hi, n->y);
								}
							}
						}
						if (!ok || hi - lo > 2.0f * 25.0f * std::tan(p_search.max_flat_degrees * kDegrees)) {
							break;
						}
						course.a = cell_point(s, ci, cj, c->y);
						course.b = course.a;
						course.start = place_on(course.a, c->normal, { 0.0f, 0.0f, 1.0f });
						course.found = true;
						course.seed = seed;
						break;
					}
					case VehicleCourseKind::Collision: {

						const float min_up = std::cos(6.0f * kDegrees);
						if (c->normal[1] < min_up || !s.clear(ci, cj)) {
							break;
						}
						for (const Dir &d : kDirs) {
							const Vec3 dv = dir_vec(d);
							const Vec3 base = cell_point(s, ci, cj, c->y);
							StaticHit wall;
							if (!s.ray(add(base, { 0.0f, 0.9f, 0.0f }), mul(dv, 40.0f), &wall)) {
								if (s.exhausted()) {
									break;
								}
								continue;
							}
							const float dist = wall.fraction * 40.0f;
							if (dist < 18.0f || dist > 35.0f || dot(wall.normal, dv) > -0.85f || std::fabs(wall.normal[1]) > 0.3f) {
								continue;
							}
							const Vec3 side = { -dv[2], 0.0f, dv[0] };
							bool tall = true;
							float height = 1.6f;
							for (const Vec3 &offset : { Vec3{ 0.0f, 1.6f, 0.0f }, mul(side, 1.0f), mul(side, -1.0f) }) {
								StaticHit h;
								const Vec3 from = add(add(base, { 0.0f, 0.9f, 0.0f }), offset);
								if (!s.ray(from, mul(dv, 40.0f), &h) || std::fabs(h.fraction * 40.0f - dist) > 1.5f) {
									tall = false;
									break;
								}
							}
							if (!tall) {
								continue;
							}
							StaticHit top;
							if (s.ray(add(base, { 0.0f, 2.4f, 0.0f }), mul(dv, 40.0f), &top) && std::fabs(top.fraction * 40.0f - dist) <= 1.5f) {
								height = 2.4f;
							}

							const float step = dir_step(d, grid);
							const int32_t ahead = int32_t((dist - 4.0f) / step);
							const int32_t behind = int32_t(std::ceil(15.0f / step));
							bool ok = true;
							float prev = c->y;
							for (int32_t m = -behind; m <= ahead && ok; ++m) {
								const Cell *n = s.ground(ci + m * d.di, cj + m * d.dj);
								ok = n != nullptr && n->hit && n->normal[1] >= min_up && s.clear(ci + m * d.di, cj + m * d.dj) &&
										(m == -behind || std::fabs(n->y - prev) < step * 0.11f + 0.15f);
								if (ok) {
									prev = n->y;
								}
							}
							if (!ok) {
								continue;
							}
							course.a = base;
							course.b = { wall.position[0], c->y, wall.position[2] };
							course.start = place_on(base, c->normal, dv);
							course.feature = height;
							course.found = true;
							course.seed = seed;
							break;
						}
						break;
					}
					case VehicleCourseKind::SlopeIdle: {

						const float ny = c->normal[1];
						if (ny > std::cos(8.0f * kDegrees) || ny < std::cos(20.0f * kDegrees)) {
							break;
						}
						bool ok = true;
						for (const Dir &d : { Dir{ 1, 0 }, Dir{ -1, 0 }, Dir{ 0, 1 }, Dir{ 0, -1 } }) {
							const Cell *n = s.ground(ci + d.di, cj + d.dj);
							if (n == nullptr || !n->hit || dot(n->normal, c->normal) < std::cos(5.0f * kDegrees)) {
								ok = false;
								break;
							}

							const float dx = float(d.di) * grid, dz = float(d.dj) * grid;
							const float predicted = c->y - (c->normal[0] * dx + c->normal[2] * dz) / ny;
							if (std::fabs(n->y - predicted) > 0.15f) {
								ok = false;
								break;
							}
						}
						if (!ok) {
							break;
						}
						const Vec3 p = cell_point(s, ci, cj, c->y);
						if (s.overlap(add(p, { 0.0f, 1.4f + 0.6f, 0.0f }), { 1.0f, 0.6f, 1.0f })) {
							break;
						}

						const Vec3 uphill = horizontal({ -c->normal[0], 0.0f, -c->normal[2] });
						course.a = p;
						course.b = add(p, mul(uphill, 10.0f));
						course.start = place_on(p, c->normal, uphill);
						course.slope_degrees = std::acos(std::min(1.0f, ny)) / kDegrees;
						course.found = true;
						course.seed = seed;
						break;
					}
				}
			}
		}
		if (kind == VehicleCourseKind::Bumps && have_best) {
			fill_leg_course(s, best, course);
			course.found = true;
			course.seed = best_seed;
		}
		course.queries = s.queries();
		course.budget_exhausted = s.exhausted();
	}
	return out;
}

std::array<VehicleCourse, kVehicleCourseKindCount> synthetic_vehicle_courses(const SyntheticVehicleGround &p_ground) {
	namespace sg = synthetic_ground;
	const float y = p_ground.y;
	const Vec3 up{ 0.0f, 1.0f, 0.0f };
	const Vec3 plus_z{ 0.0f, 0.0f, 1.0f };
	std::array<VehicleCourse, kVehicleCourseKindCount> out;
	for (size_t k = 0; k < kVehicleCourseKindCount; ++k) {
		VehicleCourse &c = out[k];
		c.kind = VehicleCourseKind(k);
		c.found = true;
		switch (c.kind) {
			case VehicleCourseKind::Straight:
			case VehicleCourseKind::Brake:
			case VehicleCourseKind::Slalom:
				c.a = { 0.0f, y, -75.0f };
				c.b = { 0.0f, y, 75.0f };
				c.start = place_on(c.a, up, plus_z);
				break;
			case VehicleCourseKind::Circle:
				c.a = { -200.0f, y, 0.0f };
				c.b = c.a;
				c.start = place_on(c.a, up, plus_z);
				break;
			case VehicleCourseKind::Bumps:
				c.a = { sg::kBumpLineX, y, -90.0f };
				c.b = { sg::kBumpLineX, y, 90.0f };
				c.start = place_on(c.a, up, plus_z);
				c.feature = sg::kKickerHeight / (sg::kKickerRamp * sg::kKickerRamp);
				break;
			case VehicleCourseKind::Collision:
				c.a = { 0.0f, y, sg::kWallZ - 25.0f };
				c.b = { 0.0f, y, sg::kWallZ };
				c.start = place_on(c.a, up, plus_z);
				c.feature = sg::kWallHeight;
				break;
			case VehicleCourseKind::SlopeIdle: {
				const float x = 0.5f * (sg::kSlopeX0 + sg::kSlopeX1);
				const Vec3 p{ x, y + (x - sg::kSlopeX0) * sg::kSlopeTan, 0.0f };
				const Vec3 n = normalized({ -sg::kSlopeTan, 1.0f, 0.0f });
				c.a = p;
				c.b = add(p, { 10.0f, 10.0f * sg::kSlopeTan, 0.0f });
				c.start = place_on(p, n, { 1.0f, 0.0f, 0.0f });
				c.slope_degrees = 12.0f;
				break;
			}
		}
	}
	return out;
}

std::array<VehicleCourse, 3> synthetic_vehicle_group(const SyntheticVehicleGround &p_ground) {
	const float y = p_ground.y;
	const Vec3 up{ 0.0f, 1.0f, 0.0f };
	std::array<VehicleCourse, 3> out;

	VehicleCourse &idle = out[0];
	idle.kind = VehicleCourseKind::SlopeIdle;
	idle.found = true;
	idle.a = { 10.0f, y, 384.0f };
	idle.b = idle.a;
	idle.start = place_on(idle.a, up, { 1.0f, 0.0f, 0.0f });

	VehicleCourse &ram = out[1];
	ram.kind = VehicleCourseKind::Collision;
	ram.found = true;
	ram.a = { 10.0f, y, 355.0f };
	ram.b = { 10.0f, y, 382.8f };
	ram.start = place_on(ram.a, up, { 0.0f, 0.0f, 1.0f });
	ram.feature = 1.5f;

	VehicleCourse &weave = out[2];
	weave.kind = VehicleCourseKind::Slalom;
	weave.found = true;
	weave.a = { -90.0f, y, 387.5f };
	weave.b = { 110.0f, y, 387.5f };
	weave.start = place_on(weave.a, up, { 1.0f, 0.0f, 0.0f });
	return out;
}

namespace {

namespace phase {
constexpr uint8_t kDrive = 0, kStop = 1, kTurn = 2, kBrakeTest = 3, kUnstick = 4, kHold = 5, kBack = 6, kPress = 7;
}

struct Pilot {
	Vec3 position{};
	Vec3 forward{};
	Vec3 right{};
	float forward_speed = 0;
	float speed = 0;

	explicit Pilot(const VehicleState &p_state) {
		position = p_state.body.origin;
		forward = horizontal(forward_of(p_state.body.rotation));
		right = { -forward[2], 0.0f, forward[0] };
		forward_speed = dot(p_state.body.linear_velocity, forward);
		speed = length(p_state.body.linear_velocity);
	}

	float angle_to(const Vec3 &p_target) const {
		const Vec3 d = sub(p_target, position);
		return std::atan2(d[0] * right[0] + d[2] * right[2], d[0] * forward[0] + d[2] * forward[2]);
	}
};

int8_t steer_code(float p_value) {
	const float v = std::clamp(p_value, -1.0f, 1.0f) * 127.0f;
	return int8_t(std::lround(v));
}

void hold_speed(const Pilot &p_pilot, float p_target, VehicleCommand &r_command) {
	const float err = p_target - p_pilot.forward_speed;
	if (err > 0.0f) {
		r_command.throttle = int8_t(std::lround(std::clamp(0.2f + err * 0.4f, 0.0f, 1.0f) * 127.0f));
	} else if (err < -1.0f) {
		r_command.brake = uint8_t(std::lround(std::clamp(-err * 0.3f, 0.0f, 1.0f) * 255.0f));
	}
}

Vec3 lookahead(const Pilot &p_pilot, const Vec3 &p_from, const Vec3 &p_to, float p_ahead) {
	const Vec3 line = sub(p_to, p_from);
	const float len = length({ line[0], 0.0f, line[2] });
	const Vec3 dir = horizontal(line);
	const float along = std::clamp(dot(sub(p_pilot.position, p_from), dir), 0.0f, len);
	return add(p_from, mul(dir, std::min(along + p_ahead, len)));
}

void set_phase(VehicleScriptMemory &r_memory, uint8_t p_phase) {
	r_memory.phase = p_phase;
	r_memory.phase_ticks = 0;
}

Vec3 lookback(const Pilot &p_pilot, const Vec3 &p_from, const Vec3 &p_to, float p_back) {
	const Vec3 line = sub(p_to, p_from);
	const float len = length({ line[0], 0.0f, line[2] });
	const Vec3 dir = horizontal(line);
	const float along = std::clamp(dot(sub(p_pilot.position, p_from), dir), 0.0f, len);
	return add(p_from, mul(dir, std::max(along - p_back, 0.0f)));
}

float reverse_steer(const Pilot &p_pilot, const Vec3 &p_target, float p_gain) {
	const Vec3 d = sub(p_target, p_pilot.position);
	const float back_lateral = -(d[0] * p_pilot.right[0] + d[2] * p_pilot.right[2]);
	const float back_along = -(d[0] * p_pilot.forward[0] + d[2] * p_pilot.forward[2]);
	return -p_gain * std::atan2(back_lateral, back_along);
}

VehicleCommand drive_leg(const VehicleCourse &p_course, const Pilot &p_p, VehicleScriptMemory &r_m) {
	VehicleCommand c;
	const Vec3 dir = horizontal(sub(p_course.b, p_course.a));
	const float along = dot(sub(p_p.position, p_course.a), dir);
	const float remaining = dot(sub(p_course.b, p_p.position), dir);
	const VehicleCourseKind kind = p_course.kind;
	constexpr float kReverseSpeed = 30.0f / 3.6f;

	constexpr float kDecel = 8.0f, kOvershoot = 6.0f;
	switch (r_m.phase) {
		case phase::kDrive: {

			const float cruise = kind == VehicleCourseKind::Slalom ? 40.0f / 3.6f : (kind == VehicleCourseKind::Bumps ? 55.0f / 3.6f : 200.0f);
			if (kind == VehicleCourseKind::Straight || kind == VehicleCourseKind::Brake) {
				c.throttle = 127;
			} else {
				hold_speed(p_p, cruise, c);
			}
			float steer = 2.5f * p_p.angle_to(lookahead(p_p, p_course.a, p_course.b, 12.0f));
			if (kind == VehicleCourseKind::Slalom && p_p.speed > 15.0f / 3.6f && remaining > 15.0f) {

				const float weave = ((r_m.phase_ticks / 60u) % 2u) == 0 ? 1.0f : -1.0f;
				steer = weave + 0.6f * steer;
			}
			c.steer = steer_code(steer);
			const float stop = p_p.forward_speed * p_p.forward_speed / (2.0f * kDecel) - kOvershoot;

			if (kind == VehicleCourseKind::Brake && r_m.measured[1] == 0.0f && p_p.forward_speed >= 60.0f / 3.6f) {
				r_m.measured[0] = p_p.forward_speed;
				r_m.measured[1] = 1.0f;
				set_phase(r_m, phase::kBrakeTest);
			} else if (remaining < stop) {
				set_phase(r_m, phase::kStop);
			} else if (r_m.phase_ticks > 240 && p_p.speed < 0.5f) {
				set_phase(r_m, phase::kBack);
			}
			break;
		}
		case phase::kBrakeTest:
			c.brake = 255;
			if (p_p.speed < 0.3f) {
				set_phase(r_m, phase::kHold);
			}
			break;
		case phase::kHold:
			c.brake = 255;
			if (r_m.phase_ticks >= 30) {
				set_phase(r_m, phase::kDrive);
			}
			break;
		case phase::kStop:
			c.brake = 255;
			if (p_p.speed < 0.5f && r_m.phase_ticks >= 10) {
				set_phase(r_m, phase::kBack);
			}
			break;
		case phase::kBack: {
			const float back_speed = -p_p.forward_speed;
			const float err = kReverseSpeed - back_speed;
			if (err > 0.0f) {
				c.throttle = int8_t(-std::lround(std::clamp(0.2f + err * 0.4f, 0.0f, 1.0f) * 127.0f));
			} else if (err < -1.0f) {
				c.brake = uint8_t(std::lround(std::clamp(-err * 0.3f, 0.0f, 1.0f) * 255.0f));
			}
			float steer = reverse_steer(p_p, lookback(p_p, p_course.a, p_course.b, 8.0f), 1.5f);
			if (kind == VehicleCourseKind::Slalom && p_p.speed > 15.0f / 3.6f && along > 15.0f) {

				const float weave = ((r_m.phase_ticks / 60u) % 2u) == 0 ? 1.0f : -1.0f;
				steer = weave + 0.6f * steer;
			}
			c.steer = steer_code(steer);
			const float stop = back_speed * back_speed / (2.0f * kDecel) - kOvershoot;
			if (along < stop) {
				set_phase(r_m, phase::kTurn);
			} else if (r_m.phase_ticks > 240 && p_p.speed < 0.5f) {
				set_phase(r_m, phase::kDrive);
			}
			break;
		}
		case phase::kTurn:
		default:
			c.brake = 255;
			if (p_p.speed < 0.5f && r_m.phase_ticks >= 10) {
				r_m.measured[1] = 0.0f;
				set_phase(r_m, phase::kDrive);
			}
			break;
	}
	return c;
}

}

VehicleCommand drive_vehicle_course(const VehicleCourse &p_course, const VehicleModel &, const VehicleState &p_state, Tick p_tick,
		VehicleScriptMemory &r_memory) {
	const Pilot p(p_state);
	VehicleCommand c;
	switch (p_course.kind) {
		case VehicleCourseKind::Straight:
		case VehicleCourseKind::Brake:
		case VehicleCourseKind::Slalom:
		case VehicleCourseKind::Bumps:
			c = drive_leg(p_course, p, r_memory);
			break;
		case VehicleCourseKind::Circle:
			hold_speed(p, 15.0f / 3.6f, c);
			c.steer = -127;
			break;
		case VehicleCourseKind::Collision: {
			const Vec3 dir = horizontal(sub(p_course.b, p_course.a));
			const float along = dot(sub(p.position, p_course.a), dir);
			switch (r_memory.phase) {
				case phase::kDrive:
					hold_speed(p, 30.0f / 3.6f, c);
					c.steer = steer_code(2.5f * p.angle_to(lookahead(p, p_course.a, p_course.b, 10.0f)));
					r_memory.measured[0] = std::max(r_memory.measured[0], p.forward_speed);
					r_memory.measured[2] = r_memory.phase_ticks == 0 ? along : std::max(r_memory.measured[2], along);
					if (r_memory.measured[0] > 5.0f && p.speed < 1.5f && r_memory.phase_ticks > 30) {
						set_phase(r_memory, phase::kPress);
					} else if (r_memory.phase_ticks > 900 || along > length(sub(p_course.b, p_course.a)) + 3.0f) {
						r_memory.measured[1] = along;
						set_phase(r_memory, phase::kBack);
					}
					break;
				case phase::kPress:

					hold_speed(p, 1.0f, c);
					c.steer = steer_code(2.5f * p.angle_to(lookahead(p, p_course.a, p_course.b, 10.0f)));
					if (along > r_memory.measured[2] - 0.1f && p.speed < 0.3f) {
						r_memory.measured[3] += 1.0f;
					}
					if (r_memory.measured[3] >= 40.0f || r_memory.phase_ticks >= 600) {
						r_memory.measured[1] = along;
						r_memory.measured[3] = 0.0f;
						set_phase(r_memory, phase::kBack);
					}
					break;
				case phase::kBack:
					c.throttle = -127;
					if (along < r_memory.measured[1] - 15.0f || r_memory.phase_ticks > 360) {
						set_phase(r_memory, phase::kStop);
					}
					break;
				case phase::kStop:
				default:
					c.brake = 255;
					if (p.speed < 0.5f && r_memory.phase_ticks >= 10) {
						r_memory.measured[0] = 0.0f;
						set_phase(r_memory, phase::kDrive);
					}
					break;
			}
			break;
		}
		case VehicleCourseKind::SlopeIdle:

			if (p_tick <= 240) {
				c.brake = 255;
			}
			break;
	}
	++r_memory.phase_ticks;

	if (p_course.kind != VehicleCourseKind::SlopeIdle && c.throttle == 0 && c.brake == 0 && c.steer == 0 && c.buttons == 0) {
		c.throttle = 1;
	}
	c.tick = p_tick;
	return c;
}

namespace {

constexpr std::string_view kLogHeader = "OBF3VLOG 1";
constexpr size_t kMaxLogLineBytes = 128;

bool parse_int(std::string_view p_text, int64_t &r_value) {
	if (p_text.empty()) {
		return false;
	}
	const std::from_chars_result r = std::from_chars(p_text.data(), p_text.data() + p_text.size(), r_value);
	return r.ec == std::errc() && r.ptr == p_text.data() + p_text.size();
}

}

std::string format_vehicle_command_log(const VehicleCommandLog &p_log) {
	std::string out(kLogHeader);
	out += '\n';
	for (const VehicleCommand &c : p_log.commands) {
		out += std::to_string(c.tick) + ' ' + std::to_string(int(c.throttle)) + ' ' + std::to_string(int(c.brake)) + ' ' + std::to_string(int(c.steer)) + ' ' +
				std::to_string(int(c.buttons)) + '\n';
	}
	return out;
}

core::Result<VehicleCommandLog> parse_vehicle_command_log(std::string_view p_text, size_t p_max_commands) {
	VehicleCommandLog log;
	size_t at = 0;
	size_t line_no = 0;
	bool header = false;
	while (at < p_text.size()) {
		const size_t end = std::min(p_text.find('\n', at), p_text.size());
		std::string_view line = p_text.substr(at, end - at);
		at = end + 1;
		++line_no;
		const std::string where = "vehicle command log line " + std::to_string(line_no);
		if (line.size() > kMaxLogLineBytes) {
			return core::make_error(where + ": longer than " + std::to_string(kMaxLogLineBytes) + " bytes");
		}
		if (!line.empty() && line.back() == '\r') {
			line.remove_suffix(1);
		}
		if (!header) {
			if (line != kLogHeader) {
				return core::make_error(where + ": expected the header `OBF3VLOG 1`");
			}
			header = true;
			continue;
		}
		if (line.empty() || line[0] == '#') {
			continue;
		}
		int64_t f[5] = {};
		size_t field = 0;
		size_t p = 0;
		while (p <= line.size() && field < 6) {
			const size_t space = std::min(line.find(' ', p), line.size());
			if (field == 5 || !parse_int(line.substr(p, space - p), f[field])) {
				return core::make_error(where + ": expected `tick throttle brake steer buttons`");
			}
			++field;
			p = space + 1;
			if (space == line.size()) {
				break;
			}
		}
		if (field != 5) {
			return core::make_error(where + ": expected 5 fields, got " + std::to_string(field));
		}
		if (f[0] < 0 || f[0] > int64_t(0xFFFFFFFFu) || f[1] < -127 || f[1] > 127 || f[2] < 0 || f[2] > 255 || f[3] < -127 || f[3] > 127 || f[4] < 0 ||
				(f[4] & ~int64_t(vehicle_buttons::kWireMask)) != 0) {
			return core::make_error(where + ": a field is out of range");
		}
		if (!log.commands.empty() && uint64_t(f[0]) != uint64_t(log.commands.back().tick) + 1u) {
			return core::make_error(where + ": tick " + std::to_string(f[0]) + " does not follow " + std::to_string(log.commands.back().tick));
		}
		if (log.commands.size() >= p_max_commands) {
			return core::make_error(where + ": more than " + std::to_string(p_max_commands) + " commands");
		}
		VehicleCommand c;
		c.tick = Tick(f[0]);
		c.throttle = int8_t(f[1]);
		c.brake = uint8_t(f[2]);
		c.steer = int8_t(f[3]);
		c.buttons = uint8_t(f[4]);
		log.commands.push_back(c);
	}
	if (!header) {
		return core::make_error("vehicle command log: no `OBF3VLOG 1` header");
	}
	return log;
}

}
