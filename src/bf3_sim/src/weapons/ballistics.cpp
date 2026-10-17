// Projectiles as queries (docs/design/m2a-weapons.md §5.8; ballistics.h): double closed-form flight while
// unpowered, stepped thrust, segment tests against the static world and the provider's targets, bounces, fuses,
// detonations and blasts.
#include "bf3/sim/weapons/ballistics.h"

#include "bf3/core/level/collision_soup.h"
#include "bf3/sim/weapons/surface_materials.h"
#include "bf3/sim/weapons/weapon_rng.h"
#include "weapons_internal.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace bf3::sim::weapons {

namespace {

using core::gameplay::WorkBudget;
using namespace detail;

// Segments one projectile may fly in one step (a tick: one segment per tick for bullets, the missile's 4 sub-steps,
// the bounces and engine boundaries; refly calls it once per tick).
constexpr int kMaxSegmentsPerStep = 64;
constexpr double kRecastSkip = 1e-3; // m past a passed surface
constexpr double kBounceLift = 1e-2; // m off a surface after a bounce
constexpr float kRaycastBehind = 0.1f; // m: the R-W2 diagnostic window

struct Env {
	const WeaponTable &table;
	const BallisticsLimits &limits;
	const CollisionWorld &world;
	const HitTargetProvider &targets;
	TargetQueryKind kind;
	WorkBudget &budget;
	ProjectileStepOutput &out;
	DVec3 lo{};
	DVec3 hi{};
	bool exhausted = false;

	bool charge(size_t p_units) {
		if (exhausted || !budget.charge(p_units).ok()) {
			exhausted = true;
			out.budget_exhausted = true;
			return false;
		}
		return true;
	}
	const SurfaceMaterialTable *materials() const { return table.materials.get(); }
};

void free_state(const Projectile &p_p, double p_gravity, SimTime p_time, DVec3 &r_pos, DVec3 &r_vel) {
	const double tau = units_to_seconds(p_time - p_p.base_time);
	r_pos = { p_p.base_position[0] + p_p.base_velocity[0] * tau, p_p.base_position[1] + p_p.base_velocity[1] * tau + 0.5 * p_gravity * tau * tau,
		p_p.base_position[2] + p_p.base_velocity[2] * tau };
	r_vel = { p_p.base_velocity[0], p_p.base_velocity[1] + p_gravity * tau, p_p.base_velocity[2] };
}

void rebase(Projectile &r_p, SimTime p_time, const DVec3 &p_pos, const DVec3 &p_vel) {
	r_p.time = p_time;
	r_p.base_time = p_time;
	r_p.base_position = p_pos;
	r_p.base_velocity = p_vel;
	r_p.position = p_pos;
	r_p.velocity = p_vel;
}

bool stepped(const ProjectileSimDef &p_d) {
	return p_d.kind == ProjectileKind::Missile && (p_d.engine_acceleration > 0 || p_d.drag > 0);
}

struct SegmentEnd {
	SimTime time = 0;
	DVec3 position{};
	DVec3 velocity{};
	bool rebase = false;
	bool engine = false;
};

// The next piece of p's flight toward p_limit with no collision: closed form while unpowered (no re-base), one
// stepped sub-step under thrust (re-based), and a re-base at the engine's phase changes.
SegmentEnd next_segment(const Projectile &p_p, const ProjectileSimDef &p_d, SimTime p_limit, uint8_t p_substeps) {
	SegmentEnd e;
	e.time = p_limit;
	bool thrust = false;
	if (stepped(p_d)) {
		const SimTime ignition = p_p.spawn_time + p_d.engine_ignition;
		const SimTime off = ignition + p_d.engine_time_to_live;
		if (p_p.time < ignition) {
			if (ignition < e.time) {
				e.time = ignition;
				e.rebase = true;
			}
		} else if (p_p.time < off || p_d.drag > 0) {
			thrust = p_p.time < off;
			const SimTime step = kTimeUnitsPerTick / std::max<SimTime>(1, p_substeps);
			const SimTime boundary = (p_p.time / step + 1) * step;
			e.time = std::min(e.time, boundary);
			if (thrust) {
				e.time = std::min(e.time, off);
			}
			e.rebase = true;
			e.engine = thrust;
			const double dt = units_to_seconds(e.time - p_p.time);
			const DVec3 v = p_p.velocity;
			const double speed = length(v);
			DVec3 a{ 0, double(p_d.gravity), 0 };
			if (thrust && speed > 0) {
				a = add(a, scale(v, double(p_d.engine_acceleration) / speed));
			}
			a = sub(a, scale(v, double(p_d.drag)));
			e.position = add(add(p_p.position, scale(v, dt)), scale(a, 0.5 * dt * dt));
			e.velocity = add(v, scale(a, dt));
			const double s1 = length(e.velocity);
			if (thrust && p_d.max_speed > 0 && s1 > double(p_d.max_speed)) {
				e.velocity = scale(e.velocity, double(p_d.max_speed) / s1);
			}
			return e;
		}
	}
	free_state(p_p, double(p_d.gravity), e.time, e.position, e.velocity);
	return e;
}

struct SegResult {
	enum class Kind : uint8_t { None, Static, Target } kind = Kind::None;
	double fraction = 2.0;
	StaticHit stat;
	TargetHit target;
};

ImpactEvent impact_of(const Projectile &p_p, ImpactKind p_kind, SimTime p_time, const DVec3 &p_pos, const Vec3 &p_normal, const DVec3 &p_vel) {
	ImpactEvent e;
	e.time = p_time;
	e.position = to_f(p_pos);
	e.normal = p_normal;
	e.velocity = to_f(p_vel);
	e.player_slot = p_p.player_slot;
	e.shot_index = p_p.shot_index;
	e.def = p_p.def;
	e.pellet = p_p.pellet;
	e.kind = p_kind;
	return e;
}

void describe_static(const Env &p_env, const StaticHit &p_h, ImpactEvent &r_e) {
	r_e.grid_pair = p_h.tag.material & core::level::kMaterialPairMask;
	r_e.material_value = p_h.tag.material;
	r_e.surface = p_h.tag.surface;
	r_e.layer = p_h.layer;
	if (const SurfaceMaterialTable *m = p_env.materials()) {
		const SurfacePair pair = m->static_pair(p_h.tag.material);
		r_e.physics_material = pair.physics_material;
		r_e.physics_property = pair.property;
	}
}

// Whether the static surface lets this projectile through (penetrable pair, no NeverPenetrate in its cell), and
// the cell's damage factor.
bool passes_surface(const Env &p_env, const ProjectileSimDef &p_d, const StaticHit &p_h, float &r_factor) {
	const SurfaceMaterialTable *m = p_env.materials();
	if (m == nullptr || p_d.never_penetrate_any) {
		return false;
	}
	const SurfacePair pair = m->static_pair(p_h.tag.material);
	if (!pair.resolved || !pair.penetrable) {
		return false;
	}
	const core::gameplay::DamageRelation rel = m->relation(p_d.property, pair.property);
	if (rel.has_penetration && rel.never_penetrate) {
		return false;
	}
	r_factor = rel.present ? rel.penetration_multiplier : 1.0f;
	return true;
}

// The earliest target hit of the segment (the provider's answer to this projectile's query); leg capsules hidden
// behind static geometry from the shot origin are skipped (DeactivateIfBehindWall).
bool first_target(Env &r_env, const Projectile &p_p, const Vec3 &p_a, const Vec3 &p_b, SimTime p_t0, SimTime p_t1, TargetHit &r_hit) {
	TargetQuery q;
	q.time_start = p_t0;
	q.time_end = p_t1;
	q.player_slot = p_p.player_slot;
	q.shot_index = p_p.shot_index;
	q.def = p_p.def;
	q.pellet = p_p.pellet;
	q.kind = r_env.kind;
	const std::span<const HitTarget> list = r_env.targets.targets_for(q);
	bool any = false;
	float best = 2.0f;
	for (const HitTarget &t : list) {
		if (!r_env.charge(1 + size_t(std::min<uint8_t>(t.capsule_count, kMaxZoneCapsules)))) {
			return false;
		}
		TargetHit h;
		if (!segment_target(p_a, p_b, t, h)) {
			continue;
		}
		if ((h.capsule_flags & zone_capsule_flags::kDeactivateIfBehindWall) != 0) {
			const Vec3 o = to_f(p_p.origin);
			const Vec3 d{ h.position[0] - o[0], h.position[1] - o[1], h.position[2] - o[2] };
			StaticHit blocker;
			if (r_env.charge(1) && r_env.world.ray(o, d, query_mask::kProjectile, &blocker) && blocker.fraction < 0.999f) {
				// Test the target again without its behind-wall capsules.
				HitTarget rest = t;
				uint8_t n = 0;
				for (uint8_t i = 0; i < t.capsule_count && i < kMaxZoneCapsules; ++i) {
					if ((t.capsules[i].flags & zone_capsule_flags::kDeactivateIfBehindWall) == 0) {
						rest.capsules[n++] = t.capsules[i];
					}
				}
				rest.capsule_count = n;
				if (!segment_target(p_a, p_b, rest, h)) {
					continue;
				}
			}
		}
		if (h.fraction < best) {
			best = h.fraction;
			r_hit = h;
			any = true;
		}
	}
	return any;
}

// Tests one segment a -> b: the targets, then the static world in order along the segment, passing invisible
// blockers and penetrable surfaces (at most limits.max_recasts per projectile per step, counted in r_recasts).
SegResult test_segment(Env &r_env, Projectile &r_p, const ProjectileSimDef &p_d, const DVec3 &p_a, const DVec3 &p_b, SimTime p_t0, SimTime p_t1,
		uint8_t &r_recasts) {
	SegResult r;
	const Vec3 af = to_f(p_a);
	const Vec3 bf = to_f(p_b);
	const DVec3 seg = sub(p_b, p_a);
	const double seg_len = length(seg);
	TargetHit th;
	double target_fraction = 2.0;
	if (first_target(r_env, r_p, af, bf, p_t0, p_t1, th)) {
		target_fraction = th.fraction;
	}
	if (r_env.exhausted) {
		return r;
	}
	if (seg_len <= 0) {
		if (target_fraction <= 1.0) {
			r.kind = SegResult::Kind::Target;
			r.fraction = target_fraction;
			r.target = th;
		}
		return r;
	}
	double base = 0.0;
	for (;;) {
		const DVec3 from = add(p_a, scale(seg, base));
		const Vec3 fromf = to_f(from);
		const Vec3 delta = to_f(sub(p_b, from));
		StaticHit h;
		if (!r_env.world.ray(fromf, delta, query_mask::kProjectile, &h)) {
			break;
		}
		const double f = base + double(h.fraction) * (1.0 - base);
		if (f >= target_fraction) {
			break;
		}
		const bool invisible = (h.tag.flags & core::level::kSoupInvisible) != 0;
		float factor = 1.0f;
		const bool penetrate = !invisible && passes_surface(r_env, p_d, h, factor);
		if ((invisible || penetrate) && r_recasts < r_env.limits.max_recasts) {
			if (!r_env.charge(1)) {
				return r;
			}
			++r_recasts;
			if (invisible) {
				++r_env.out.invisible_passes;
			} else {
				++r_env.out.penetrations;
				++r_p.passes;
				r_p.damage_scale *= factor;
				const SimTime t = p_t0 + SimTime(std::llround(f * double(p_t1 - p_t0)));
				ImpactEvent e = impact_of(r_p, ImpactKind::Static, t, add(p_a, scale(seg, f)), h.normal, r_p.velocity);
				describe_static(r_env, h, e);
				e.penetrated = true;
				r_env.out.impacts.push_back(e);
			}
			base = std::min(1.0, f + kRecastSkip / seg_len);
			if (base >= 1.0) {
				break;
			}
			continue;
		}
		r.kind = SegResult::Kind::Static;
		r.fraction = f;
		r.stat = h;
		return r;
	}
	if (target_fraction <= 1.0) {
		r.kind = SegResult::Kind::Target;
		r.fraction = target_fraction;
		r.target = th;
	}
	return r;
}

void blast(Env &r_env, const Projectile &p_p, const ExplosionSimDef &p_x, bool p_dud, const DVec3 &p_pos, const Vec3 &p_normal, SimTime p_time) {
	if (!p_x.present) {
		return;
	}
	TargetQuery q;
	q.time_start = p_time;
	q.time_end = p_time;
	q.player_slot = p_p.player_slot;
	q.shot_index = p_p.shot_index;
	q.def = p_p.def;
	q.pellet = p_p.pellet;
	q.kind = TargetQueryKind::Blast;
	const std::span<const HitTarget> list = r_env.targets.targets_for(q);
	const Mechanics &mech = r_env.table.mechanics;
	const DVec3 eye = add(p_pos, scale(to_d(p_normal), 0.05)); // occlusion rays start just off the surface
	for (const HitTarget &t : list) {
		const uint8_t n = std::min<uint8_t>(t.capsule_count, kMaxZoneCapsules);
		if (!r_env.charge(1 + size_t(n))) {
			return;
		}
		double best = 1e300;
		HitZone zone = HitZone::Body;
		uint8_t capsule = 0;
		for (uint8_t i = 0; i < n; ++i) {
			const ZoneCapsule &c = t.capsules[i];
			const DVec3 a = to_d(c.a), b = to_d(c.b);
			const DVec3 ab = sub(b, a);
			const double l2 = dot(ab, ab);
			const double s = l2 > 0 ? std::clamp(dot(sub(p_pos, a), ab) / l2, 0.0, 1.0) : 0.0;
			const double d = std::max(0.0, length(sub(p_pos, add(a, scale(ab, s)))) - double(c.radius));
			if (d < best) {
				best = d;
				zone = c.zone;
				capsule = c.capsule;
			}
		}
		const double base = blast_damage(p_x, best, mech.blast_falloff);
		if (!(base > 0) || (t.spawn_protected && mech.spawn_protection_blocks)) {
			continue;
		}
		if (mech.blast_occlusion && !p_x.disable_occlusion) {
			bool visible = false;
			for (uint8_t i = 0; i < n && !visible; ++i) {
				if (!r_env.charge(1)) {
					return;
				}
				const ZoneCapsule &c = t.capsules[i];
				const DVec3 centre = scale(add(to_d(c.a), to_d(c.b)), 0.5);
				visible = !r_env.world.ray(to_f(eye), to_f(sub(centre, eye)), query_mask::kCollision, nullptr);
			}
			if (!visible) {
				continue;
			}
		}
		DamageEvent e;
		e.time = p_time;
		e.attacker_slot = p_p.player_slot;
		e.target = t.id;
		e.shot_index = p_p.shot_index;
		e.def = p_p.def;
		e.pellet = p_p.pellet;
		e.kind = p_dud ? DamageKind::DudBlast : DamageKind::Blast;
		e.zone = zone;
		e.capsule = capsule;
		e.distance = float(best);
		e.base = float(base);
		e.multiplier = p_x.zone_multiplier[size_t(zone)];
		e.penetration = 1.0f;
		e.amount = float(base * double(e.multiplier));
		r_env.out.damage.push_back(e);
	}
}

void detonate(Env &r_env, const Projectile &p_p, const ProjectileSimDef &p_d, const DVec3 &p_pos, const Vec3 &p_normal, SimTime p_time, bool p_timeout) {
	const bool armed = p_time - p_p.spawn_time >= p_d.time_to_arm;
	DetonationEvent e;
	e.time = p_time;
	e.position = to_f(p_pos);
	e.normal = p_normal;
	e.player_slot = p_p.player_slot;
	e.shot_index = p_p.shot_index;
	e.def = p_p.def;
	e.dud = !armed;
	e.timeout = p_timeout;
	r_env.out.detonations.push_back(e);
	blast(r_env, p_p, armed ? p_d.explosion : p_d.dud, !armed, p_pos, p_normal, p_time);
}

void damage_on_target(Env &r_env, const Projectile &p_p, const ProjectileSimDef &p_d, const TargetHit &p_h, const DVec3 &p_pos, SimTime p_time) {
	const Mechanics &mech = r_env.table.mechanics;
	const bool explosive = p_d.kind == ProjectileKind::Missile || p_d.explosion.present;
	DamageEvent e;
	e.time = p_time;
	e.attacker_slot = p_p.player_slot;
	e.target = p_h.target;
	e.shot_index = p_p.shot_index;
	e.def = p_p.def;
	e.pellet = p_p.pellet;
	e.kind = explosive ? DamageKind::Direct : DamageKind::Bullet;
	e.zone = p_h.zone;
	e.capsule = p_h.capsule;
	const double dist = mech.falloff_basis == FalloffBasis::PathLength ? p_p.path_length : length(sub(p_pos, p_p.origin));
	e.distance = float(dist);
	e.base = float(p_d.kind == ProjectileKind::Missile ? double(p_d.direct_damage) : falloff_damage(p_d, dist));
	e.multiplier = p_d.zone[size_t(p_h.zone)].protection_multiplier * mech.bullet_damage_modifier;
	e.penetration = p_p.damage_scale;
	e.amount = e.base * e.multiplier * e.penetration;
	if (e.amount > 0) {
		r_env.out.damage.push_back(e);
	}
}

bool outside(const Env &p_env, const DVec3 &p_pos) {
	for (size_t k = 0; k < 3; ++k) {
		if (!(p_pos[k] >= p_env.lo[k] && p_pos[k] <= p_env.hi[k])) {
			return true;
		}
	}
	return false;
}

// Flies p to p_t_end (or its expiry). False when it is gone (hit, detonated, expired, outside the world).
bool fly(Env &r_env, Projectile &r_p, SimTime p_t_end) {
	if (r_p.def >= r_env.table.defs.size()) {
		return false;
	}
	const ProjectileSimDef &d = r_env.table.defs[r_p.def].projectile;
	if (r_p.time >= p_t_end) {
		return true;
	}
	const SimTime expiry = r_p.spawn_time + d.time_to_live;
	if (d.instant_hit) {
		// One segment of speed x TTL at the spawn time (knives).
		if (!r_env.charge(1)) {
			return true;
		}
		const DVec3 b = add(r_p.position, scale(r_p.velocity, units_to_seconds(d.time_to_live)));
		uint8_t recasts = 0;
		const SegResult r = test_segment(r_env, r_p, d, r_p.position, b, r_p.time, r_p.time, recasts);
		if (r_env.exhausted) {
			return true;
		}
		const DVec3 pos = add(r_p.position, scale(sub(b, r_p.position), r.fraction <= 1.0 ? r.fraction : 1.0));
		if (r.kind == SegResult::Kind::Target) {
			ImpactEvent e = impact_of(r_p, ImpactKind::Target, r_p.time, pos, r.target.normal, r_p.velocity);
			e.target = r.target.target;
			e.zone = r.target.zone;
			e.physics_property = r.target.property;
			r_env.out.impacts.push_back(e);
			damage_on_target(r_env, r_p, d, r.target, pos, r_p.time);
		} else if (r.kind == SegResult::Kind::Static) {
			ImpactEvent e = impact_of(r_p, ImpactKind::Static, r_p.time, pos, r.stat.normal, r_p.velocity);
			describe_static(r_env, r.stat, e);
			r_env.out.impacts.push_back(e);
		}
		return false;
	}
	const SimTime limit = std::min(p_t_end, expiry);
	uint8_t recasts = 0;
	uint8_t bounces = 0;
	for (int seg = 0; seg < kMaxSegmentsPerStep && r_p.time < limit; ++seg) {
		if ((r_p.flags & projectile_flags::kResting) != 0) {
			r_p.time = limit;
			break;
		}
		const SegmentEnd e = next_segment(r_p, d, limit, r_env.limits.missile_substeps);
		if (!r_env.charge(1)) {
			return true;
		}
		if (!finite3(e.position) || !finite3(e.velocity)) {
			return false;
		}
		const SegResult r = test_segment(r_env, r_p, d, r_p.position, e.position, r_p.time, e.time, recasts);
		if (r_env.exhausted) {
			return true;
		}
		if (e.engine) {
			r_p.flags = uint8_t(r_p.flags | projectile_flags::kEngineOn);
		} else {
			r_p.flags = uint8_t(r_p.flags & ~projectile_flags::kEngineOn);
		}
		if (r.kind == SegResult::Kind::None) {
			r_p.path_length += length(sub(e.position, r_p.position));
			if (e.rebase) {
				rebase(r_p, e.time, e.position, e.velocity);
			} else {
				r_p.time = e.time;
				r_p.position = e.position;
				r_p.velocity = e.velocity;
			}
			if (r_p.time - r_p.spawn_time >= d.time_to_arm) {
				r_p.flags = uint8_t(r_p.flags | projectile_flags::kArmed);
			}
			if (outside(r_env, r_p.position)) {
				return false;
			}
			continue;
		}
		const double f = std::clamp(r.fraction, 0.0, 1.0);
		const SimTime t_hit = std::clamp(r_p.time + SimTime(std::llround(f * double(e.time - r_p.time))), r_p.time, e.time);
		const DVec3 pos = add(r_p.position, scale(sub(e.position, r_p.position), f));
		const DVec3 vel = add(r_p.velocity, scale(sub(e.velocity, r_p.velocity), f));
		r_p.path_length += length(sub(pos, r_p.position));
		const Vec3 normal = r.kind == SegResult::Kind::Static ? r.stat.normal : r.target.normal;
		if (d.kind == ProjectileKind::Grenade) {
			// Bounce [inferred]: v' = reflect(v) x CollisionSpeedMultiplier; rest below MinBounceSpeed.
			const DVec3 n = normalize(to_d(normal), { 0, 1, 0 });
			const DVec3 reflected = sub(vel, scale(n, 2.0 * dot(vel, n)));
			const DVec3 v2 = scale(reflected, double(d.bounce_speed_multiplier));
			ImpactEvent ie = impact_of(r_p, ImpactKind::Bounce, t_hit, pos, normal, vel);
			if (r.kind == SegResult::Kind::Static) {
				describe_static(r_env, r.stat, ie);
			} else {
				ie.target = r.target.target;
				ie.zone = r.target.zone;
			}
			r_env.out.impacts.push_back(ie);
			++r_p.bounces;
			++bounces;
			const DVec3 lifted = add(pos, scale(n, kBounceLift));
			if (bounces >= r_env.limits.max_bounces || length(v2) < double(d.min_bounce_speed)) {
				rebase(r_p, t_hit, lifted, { 0, 0, 0 });
				r_p.flags = uint8_t(r_p.flags | projectile_flags::kResting);
			} else {
				rebase(r_p, t_hit, lifted, v2);
			}
			continue;
		}
		if (r.kind == SegResult::Kind::Target) {
			ImpactEvent ie = impact_of(r_p, ImpactKind::Target, t_hit, pos, normal, vel);
			ie.target = r.target.target;
			ie.zone = r.target.zone;
			ie.physics_property = r.target.property;
			r_env.out.impacts.push_back(ie);
			damage_on_target(r_env, r_p, d, r.target, pos, t_hit);
		} else {
			ImpactEvent ie = impact_of(r_p, ImpactKind::Static, t_hit, pos, normal, vel);
			describe_static(r_env, r.stat, ie);
			r_env.out.impacts.push_back(ie);
			if (r.stat.layer == StaticLayer::Collision && r_env.charge(1)) {
				const DVec3 dir = normalize(sub(e.position, r_p.position));
				StaticHit behind;
				if (r_env.world.ray(to_f(pos), to_f(scale(dir, double(kRaycastBehind))), query_mask::kRaycast, &behind)) {
					++r_env.out.collision_before_raycast;
				}
			}
		}
		if (d.kind == ProjectileKind::Missile || d.explosion.present || d.dud.present) {
			detonate(r_env, r_p, d, pos, normal, t_hit, false);
		}
		r_p.time = t_hit;
		return false;
	}
	if (r_p.time >= expiry) {
		if (d.detonate_on_timeout) {
			detonate(r_env, r_p, d, r_p.position, { 0, 1, 0 }, expiry, true);
		} else {
			r_env.out.impacts.push_back(impact_of(r_p, ImpactKind::Expired, expiry, r_p.position, { 0, 0, 0 }, r_p.velocity));
		}
		return false;
	}
	return true;
}

// Projectile k of a shot (pellet k > 0 or a multi-pellet shot: a draw inside the pellet cone, from the seed).
Projectile make_projectile(const WeaponTable &p_table, const ShotEvent &p_shot, uint8_t p_pellet) {
	const WeaponSimDef &wd = p_table.defs[p_shot.def];
	Vec3 dir = p_shot.direction;
	if (p_shot.pellets > 1 && p_shot.pellet_cone > 0) {
		dir = sample_cone(p_shot.direction, p_shot.pellet_cone, unit_float(shot_random(p_shot.seed, RngPurpose::PelletRadius, p_pellet)),
				unit_float(shot_random(p_shot.seed, RngPurpose::PelletAngle, p_pellet)), p_table.mechanics.disc);
	}
	const DVec3 v = frame_apply(frame_of(normalize(to_d(dir))), wd.projectile.launch_velocity);
	Projectile p;
	p.spawn_time = p_shot.spawn_time;
	rebase(p, p_shot.spawn_time, to_d(p_shot.origin), v);
	p.origin = p.position;
	p.seed = p_shot.seed;
	p.player_slot = p_shot.player_slot;
	p.shot_index = p_shot.shot_index;
	p.def = p_shot.def;
	p.pellet = p_pellet;
	p.damage_scale = 1.0f;
	if (wd.projectile.time_to_arm <= 0) {
		p.flags = projectile_flags::kArmed;
	}
	return p;
}

Env make_env(const WeaponTable &p_table, const BallisticsLimits &p_limits, const CollisionWorld &p_world, const HitTargetProvider &p_targets,
		TargetQueryKind p_kind, WorkBudget &r_budget, ProjectileStepOutput &r_out) {
	Env env{ p_table, p_limits, p_world, p_targets, p_kind, r_budget, r_out };
	const Vec3 lo = p_world.shapes()->min();
	const Vec3 hi = p_world.shapes()->max();
	const double m = double(p_limits.world_margin);
	env.lo = { double(lo[0]) - m, double(lo[1]) - m, double(lo[2]) - m };
	env.hi = { double(hi[0]) + m, double(hi[1]) + m, double(hi[2]) + m };
	return env;
}

} // namespace

ProjectileWorld::ProjectileWorld(std::shared_ptr<const WeaponTable> p_table, const BallisticsLimits &p_limits) :
		table_(std::move(p_table)), limits_(p_limits) {}

core::Status ProjectileWorld::spawn(const ShotEvent &p_shot) {
	if (table_ == nullptr || p_shot.def >= table_->defs.size()) {
		++refused_;
		return core::make_error("ProjectileWorld::spawn: def " + std::to_string(p_shot.def) + " is not in the table");
	}
	const ProjectileSimDef &d = table_->defs[p_shot.def].projectile;
	if (d.kind == ProjectileKind::None || d.kind == ProjectileKind::Other) {
		++refused_;
		return core::make_error("ProjectileWorld::spawn: " + table_->defs[p_shot.def].variant + " fires no simulated projectile");
	}
	if (!finite3f(p_shot.origin) || !finite3f(p_shot.direction) || !std::isfinite(p_shot.pellet_cone)) {
		++refused_;
		return core::make_error("ProjectileWorld::spawn: a non-finite shot");
	}
	const uint8_t pellets = uint8_t(std::clamp<int>(p_shot.pellets, 1, kMaxPellets));
	if (live_.size() + pellets > limits_.max_projectiles) {
		refused_ += pellets;
		return core::make_error("ProjectileWorld::spawn: " + std::to_string(limits_.max_projectiles) + " projectiles alive");
	}
	for (uint8_t k = 0; k < pellets; ++k) {
		live_.push_back(make_projectile(*table_, p_shot, k));
	}
	return core::Status::success();
}

void ProjectileWorld::step(Tick p_tick, const CollisionWorld &p_world, const HitTargetProvider &p_targets, ProjectileStepOutput &r_out) {
	WorkBudget budget(limits_.budget_per_step);
	Env env = make_env(*table_, limits_, p_world, p_targets, TargetQueryKind::Flight, budget, r_out);
	const SimTime t_end = tick_end(p_tick);
	size_t w = 0;
	for (size_t i = 0; i < live_.size(); ++i) {
		Projectile &p = live_[i];
		const bool keep = env.exhausted ? true : fly(env, p, t_end);
		if (keep) {
			live_[w++] = p;
		} else {
			++r_out.removed;
		}
	}
	live_.resize(w);
	r_out.budget_used = budget.used();
}

DVec3 ballistic_position(const DVec3 &p_origin, const DVec3 &p_velocity, double p_gravity, double p_seconds) {
	return { p_origin[0] + p_velocity[0] * p_seconds, p_origin[1] + p_velocity[1] * p_seconds + 0.5 * p_gravity * p_seconds * p_seconds,
		p_origin[2] + p_velocity[2] * p_seconds };
}

Projectile fly_free(const WeaponSimDef &p_def, const Projectile &p_start, SimTime p_until) {
	Projectile p = p_start;
	const ProjectileSimDef &d = p_def.projectile;
	BallisticsLimits limits;
	// Bounded: one piece per sub-step boundary (4 per tick under thrust) up to 60 s, else one closed-form piece.
	for (int i = 0; i < 4 * 60 * int(kSimHz) + 8 && p.time < p_until; ++i) {
		const SegmentEnd e = next_segment(p, d, p_until, limits.missile_substeps);
		if (e.rebase) {
			rebase(p, e.time, e.position, e.velocity);
		} else {
			p.time = e.time;
			p.position = e.position;
			p.velocity = e.velocity;
		}
	}
	return p;
}

bool refly_projectile(const WeaponTable &p_table, const ShotEvent &p_shot, uint8_t p_pellet, SimTime p_until, const CollisionWorld &p_world,
		const HitTargetProvider &p_targets, core::gameplay::WorkBudget &p_budget, ImpactEvent &r_hit, core::Status &r_status) {
	r_status = core::Status::success();
	if (p_shot.def >= p_table.defs.size() || p_pellet >= std::max<uint8_t>(p_shot.pellets, 1)) {
		r_status = core::make_error("refly_projectile: no such def or pellet");
		return false;
	}
	const ProjectileSimDef &d = p_table.defs[p_shot.def].projectile;
	if (d.kind == ProjectileKind::None || d.kind == ProjectileKind::Other) {
		r_status = core::make_error("refly_projectile: the def fires no simulated projectile");
		return false;
	}
	Projectile p = make_projectile(p_table, p_shot, p_pellet);
	BallisticsLimits limits;
	ProjectileStepOutput out;
	Env env = make_env(p_table, limits, p_world, p_targets, TargetQueryKind::Refly, p_budget, out);
	const SimTime limit = std::min(p_until, p.spawn_time + d.time_to_live);
	bool alive = true;
	for (Tick t = tick_containing(std::max<SimTime>(0, p.time)); alive && !env.exhausted && tick_start(t) < limit; ++t) {
		alive = fly(env, p, std::min(tick_end(t), limit));
		for (const ImpactEvent &e : out.impacts) {
			if (e.kind == ImpactKind::Target) {
				r_hit = e;
				return true;
			}
		}
	}
	if (env.exhausted) {
		r_status = p_budget.status();
	}
	return false;
}

} // namespace bf3::sim::weapons
