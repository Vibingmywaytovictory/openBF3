// M2a weapons: the shooting range on Metro's static world (ctest sim.weapons.range; docs/design/m2a-weapons.md §1, §10).
// The install is read as it is (read-only); exits 77 = skipped without one; every case prints install.unchanged.
// W3 end to end (5 m and 60 m, exact aim), W5 per-hit damage on stand-in capsules, W7 on two CollisionWorlds over one
// StaticWorldShapes, W9 blasts on stand-ins, and the impact descriptor of every static hit; the lane and the
// static-world diagnostics (invisible passes, penetrations, Collision-before-Raycast) are printed.
#include "weapons_install_support.h"

#include "bf3/sim/weapons/damage.h"
#include "bf3/sim/weapons/hit_target.h"
#include "bf3/sim/weapons/shooting_range.h"

#include <doctest/doctest.h>

#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

using namespace bf3;
using namespace bf3::sim;
using namespace bf3::sim::weapons;
using namespace bf3::test::weapons;

namespace {

constexpr std::string_view kM16A3 = "weapons/m16a4/u_m16a4";
constexpr std::string_view kM98B = "weapons/model98b/u_m98b";
constexpr std::string_view k870 = "weapons/remington870/u_870";
constexpr std::string_view kM67 = "weapons/m67/u_m67";
constexpr std::string_view kRpg = "weapons/rpg7/u_rpg7";
constexpr std::string_view kRangeVariants[] = { kM16A3, kM98B, k870, kM67, kRpg };

std::shared_ptr<const WeaponTable> range_table(const WeaponsInstall &p_w) {
	core::Result<std::shared_ptr<const WeaponTable>> t = install_weapon_table(p_w.weapons, kRangeVariants, install_table_options(p_w.weapons));
	REQUIRE_MESSAGE(t.ok(), (t.ok() ? std::string() : t.error().message));
	return *t;
}

uint16_t id(std::string_view p_variant) {
	for (size_t i = 0; i < std::size(kRangeVariants); ++i) {
		if (kRangeVariants[i] == p_variant) {
			return uint16_t(i);
		}
	}
	FAIL("no range variant");
	return 0;
}

// The world W3's 60 m rows run on: Metro TDM's longest lane, else the other Metro modes' spawns, else the synthetic
// wall world (docs §10), with the reason printed.
struct LaneWorld {
	const CollisionWorld *world = nullptr;
	RangeLane lane;
	std::string where;
	std::unique_ptr<CollisionWorld> owned;
	std::optional<core::level::LevelCollision> other;
	std::shared_ptr<const StaticWorldShapes> other_shapes;
};
LaneWorld lane_world(WeaponsInstall &p_w, MetroWorld &p_metro, float p_min_length) {
	LaneWorld lw;
	const float eye = p_w.weapons.stand_eye_height;
	if (const std::optional<RangeLane> l = find_range_lane(*p_metro.world, p_metro.points, eye, p_min_length)) {
		lw.world = p_metro.world.get();
		lw.lane = *l;
		lw.where = "Metro TeamDeathMatch0 spawn " + std::to_string(l->spawn_index);
		return lw;
	}
	std::printf("no %.0f m lane among Metro TDM's spawns: trying the other Metro modes\n", double(p_min_length));
	for (const char *mode : { "ConquestLarge0", "ConquestSmall0", "RushLarge0", "SquadDeathMatch0" }) {
		core::Result<core::level::LevelCollision> c = core::level::load_level_collision(p_w.install, "MP_Subway", mode);
		if (!c) {
			std::printf("  %s: %s\n", mode, c.error().message.c_str());
			continue;
		}
		core::Result<core::level::LevelSpawns> spawns = core::level::read_level_spawns(*c->world);
		if (!spawns) {
			continue;
		}
		const std::vector<SpawnPoint> points = to_spawn_points(*spawns);
		if (const std::optional<RangeLane> l = find_range_lane(*p_metro.world, points, eye, p_min_length)) {
			lw.world = p_metro.world.get();
			lw.lane = *l;
			lw.where = std::string("Metro ") + mode + " spawn " + std::to_string(l->spawn_index) + " (on the TDM static world)";
			return lw;
		}
		std::printf("  %s: %zu spawns, no %.0f m lane\n", mode, points.size(), double(p_min_length));
	}
	lw.owned = wall_world({}, eye);
	lw.world = lw.owned.get();
	lw.lane.feet = { 0, 0, 0 };
	lw.lane.eye = { 0, eye, 0 };
	lw.lane.yaw = 0;
	lw.lane.length = 500.0f;
	lw.where = "the synthetic wall world (no Metro lane reached the distance)";
	return lw;
}

RangeResult range(const WeaponTable &p_table, const CollisionWorld &p_world, const WeaponsInstall &p_w, const RangeLane &p_lane, const RangeScenario &p_s) {
	core::Result<RangeResult> r = run_range(p_table, p_world, p_w.weapons.soldier, p_w.weapons.capsules, p_lane, p_s);
	REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
	return std::move(*r);
}

// Every static impact carries a resolved grid pair and its physics material (the impact descriptor, §5.8).
struct ImpactAudit {
	size_t statics = 0, unresolved = 0, wrong_material = 0;
	void add(const SurfaceMaterialTable &p_mt, const RangeResult &p_r) {
		for (const ImpactEvent &e : p_r.impacts) {
			if (e.kind != ImpactKind::Static) {
				continue;
			}
			++statics;
			const SurfacePair p = p_mt.static_pair(e.material_value);
			unresolved += p.resolved ? 0 : 1;
			wrong_material += (p.resolved && (p.physics_material != e.physics_material || p.property != e.physics_property)) ? 1 : 0;
		}
	}
};

void print_diagnostics(const char *p_name, const RangeResult &p_r) {
	std::printf("  %s: %zu shots, %zu impacts (%zu static), %zu damage events, invisible passes %zu, penetrations %zu, Collision-before-Raycast %zu, "
				"refused %zu, peak live %zu, shots %016llx impacts %016llx\n",
			p_name, p_r.shots.size(), p_r.impacts.size(), p_r.static_hits, p_r.damage.size(), p_r.invisible_passes, p_r.penetrations, p_r.collision_before_raycast,
			p_r.refused_projectiles, p_r.peak_live_projectiles, (unsigned long long)p_r.shot_list_hash, (unsigned long long)p_r.impact_list_hash);
}

} // namespace

TEST_SUITE("weapons-range") {
	TEST_CASE("sim.weapons.range: the Metro lane, W3 end to end at 5 m and 60 m, W5 per-hit damage") {
		WeaponsInstall *w = weapons_install();
		if (w == nullptr) {
			return;
		}
		MetroWorld *m = metro_world(*w);
		const std::shared_ptr<const WeaponTable> t = range_table(*w);
		const std::optional<RangeLane> longest = find_range_lane(*m->world, m->points, w->weapons.stand_eye_height, 0.0f);
		REQUIRE(longest.has_value());
		std::printf("Metro TDM longest lane: spawn %u, feet (%.2f, %.2f, %.2f), yaw %u, %.1f m clear\n", longest->spawn_index, double(longest->feet[0]),
				double(longest->feet[1]), double(longest->feet[2]), unsigned(longest->yaw), double(longest->length));
		ImpactAudit audit;

		// W3: M16A3 hits to kill with exact aim (chest 4 / head 2 at 5 m; 6 / 3 at 60 m).
		struct Row {
			float distance;
			HitZone zone;
			uint32_t hits;
		};
		const Row rows[] = { { 5.0f, HitZone::Chest, 4 }, { 5.0f, HitZone::Head, 2 }, { 60.0f, HitZone::Chest, 6 }, { 60.0f, HitZone::Head, 3 } };
		LaneWorld far = lane_world(*w, *m, 61.0f);
		std::printf("W3 60 m rows on %s (lane %.1f m)\n", far.where.c_str(), double(far.lane.length));
		for (const Row &row : rows) {
			const LaneWorld *lw = row.distance > 50.0f ? &far : nullptr;
			const CollisionWorld &world = lw != nullptr ? *lw->world : *m->world;
			const RangeLane &lane = lw != nullptr ? lw->lane : *longest;
			RangeScenario s;
			s.weapon = id(kM16A3);
			s.targets = { { row.distance, Pose::Stand, row.zone } };
			s.script = FireScript::Hold;
			s.ticks = 240;
			s.exact_aim = true;
			const RangeResult r = range(*t, world, *w, lane, s);
			audit.add(*w->weapons.materials, r);
			REQUIRE(r.targets.size() == 1);
			const RangeTargetResult &tr = r.targets[0];
			uint32_t hits = 0;
			for (const uint32_t h : tr.hits) {
				hits += h;
			}
			std::printf("W3 M16A3 %s at %.0f m: killed %s after %u hits (zone hits %u/%u/%u/%u), %.2f damage\n", hit_zone_name(row.zone), double(row.distance),
					tr.killed ? "yes" : "no", hits, tr.hits[0], tr.hits[1], tr.hits[2], tr.hits[3], double(tr.damage));
			CHECK(tr.killed);
			CHECK(hits == row.hits);
			CHECK(tr.hits[size_t(row.zone)] == row.hits);
		}

		// W5: the first hit on each zone at 5 m (exact aim), damage within 1e-3.
		struct Hit {
			std::string_view variant;
			HitZone zone;
			double damage;
		};
		const Hit hits[] = { { kM16A3, HitZone::Head, 50.0 }, { kM16A3, HitZone::Chest, 25.0 }, { kM16A3, HitZone::Body, 25.0 }, { kM16A3, HitZone::Legs, 22.75 },
			{ kM98B, HitZone::Head, 223.25 }, { kM98B, HitZone::Chest, 100.70 }, { kM98B, HitZone::Legs, 86.45 }, { k870, HitZone::Head, 18.0 } };
		for (const Hit &h : hits) {
			RangeScenario s;
			s.weapon = id(h.variant);
			s.targets = { { 5.0f, Pose::Stand, h.zone } };
			s.script = FireScript::Tap;
			s.ticks = 30;
			s.exact_aim = true;
			const RangeResult r = range(*t, *m->world, *w, *longest, s);
			audit.add(*w->weapons.materials, r);
			REQUIRE_FALSE(r.damage.empty());
			const DamageEvent &d = r.damage[0];
			std::printf("W5 %s %s at 5 m: %.4f (zone %s, x%.2f, distance %.2f m)\n", std::string(h.variant).c_str(), hit_zone_name(h.zone), double(d.amount),
					hit_zone_name(d.zone), double(d.multiplier), double(d.distance));
			CHECK(d.zone == h.zone);
			CHECK(std::fabs(double(d.amount) - h.damage) <= 1e-3);
		}
		std::printf("static impacts: %zu, unresolved pair %zu, physics material differs %zu\n", audit.statics, audit.unresolved, audit.wrong_material);
		CHECK(audit.unresolved == 0);
		CHECK(audit.wrong_material == 0);
		check_install_unchanged(*w);
	}

	TEST_CASE("sim.weapons.range: W7 identical shot and impact lists on two worlds over one StaticWorldShapes") {
		WeaponsInstall *w = weapons_install();
		if (w == nullptr) {
			return;
		}
		MetroWorld *m = metro_world(*w);
		const std::shared_ptr<const WeaponTable> t = range_table(*w);
		core::Result<std::unique_ptr<CollisionWorld>> second = CollisionWorld::create(m->shapes);
		REQUIRE(second.ok());
		const std::optional<RangeLane> lane = find_range_lane(*m->world, m->points, w->weapons.stand_eye_height, 0.0f);
		REQUIRE(lane.has_value());
		const float far = std::min(40.0f, lane->length - 1.0f);
		ImpactAudit audit;
		struct Run {
			const char *name;
			RangeScenario s;
		};
		std::vector<Run> runs;
		{
			RangeScenario s;
			s.weapon = id(kM16A3);
			s.targets = { { std::min(20.0f, far), Pose::Stand, HitZone::Chest }, { far, Pose::Crouch, HitZone::Head } };
			s.script = FireScript::Hold;
			s.ticks = 600; // 10 s of auto fire (the real magazine: reloads included)
			s.ads = false;
			runs.push_back({ "M16A3 10 s hip fire", s });
		}
		{
			RangeScenario s;
			s.weapon = id(k870);
			s.targets = { { std::min(12.0f, far), Pose::Stand, HitZone::Chest } };
			s.script = FireScript::Tap;
			s.ticks = 240;
			runs.push_back({ "870 taps", s });
		}
		{
			RangeScenario s;
			s.weapon = id(kM67);
			s.targets = { { std::min(15.0f, far), Pose::Stand, HitZone::Chest } };
			s.script = FireScript::Throw;
			s.ticks = 60;
			s.hold_ticks = 40;
			s.settle_ticks = 300;
			runs.push_back({ "M67 throw", s });
		}
		for (const Run &run : runs) {
			const RangeResult a = range(*t, *m->world, *w, *lane, run.s);
			const RangeResult b = range(*t, **second, *w, *lane, run.s);
			print_diagnostics(run.name, a);
			audit.add(*w->weapons.materials, a);
			CHECK(a.shot_list_hash == b.shot_list_hash);
			CHECK(a.impact_list_hash == b.impact_list_hash);
			CHECK(a.shots.size() == b.shots.size());
			CHECK(a.damage.size() == b.damage.size());
			CHECK_FALSE(a.shots.empty());
			RangeScenario other = run.s;
			other.match_seed = 2;
			const RangeResult c = range(*t, *m->world, *w, *lane, other);
			if (run.s.weapon == id(kM16A3)) {
				size_t changed = 0;
				for (size_t i = 0; i < std::min(a.shots.size(), c.shots.size()); ++i) {
					changed += a.shots[i].direction != c.shots[i].direction ? 1 : 0;
				}
				std::printf("  seed 2: %zu of %zu hip-fire directions differ\n", changed, a.shots.size());
				CHECK(double(changed) >= 0.99 * double(a.shots.size()));
			}
		}
		std::printf("static impacts: %zu, unresolved pair %zu, physics material differs %zu\n", audit.statics, audit.unresolved, audit.wrong_material);
		CHECK(audit.unresolved == 0);
		CHECK(audit.wrong_material == 0);
		check_install_unchanged(*w);
	}

	TEST_CASE("sim.weapons.range: W9 blasts and direct hits on stand-ins on Metro") {
		WeaponsInstall *w = weapons_install();
		if (w == nullptr) {
			return;
		}
		MetroWorld *m = metro_world(*w);
		const std::shared_ptr<const WeaponTable> t = range_table(*w);
		const std::optional<RangeLane> lane = find_range_lane(*m->world, m->points, w->weapons.stand_eye_height, 0.0f);
		REQUIRE(lane.has_value());
		const Mechanics &mech = t->mechanics;

		// M67: throw along the lane with no targets, read where it goes off, then stand stand-ins beside that point and
		// throw the same grenade again.
		RangeScenario s;
		s.weapon = id(kM67);
		s.script = FireScript::Throw;
		s.ticks = 60;
		s.hold_ticks = 40;
		s.settle_ticks = 300;
		const RangeResult first = range(*t, *m->world, *w, *lane, s);
		REQUIRE(first.shots.size() == 1);
		REQUIRE(first.detonations.size() == 1);
		const DetonationEvent det = first.detonations[0];
		std::printf("M67 on Metro: thrown at %lld, detonated at %lld (spawn + %lld) at (%.2f, %.2f, %.2f), timeout %d\n", (long long)first.shots[0].time,
				(long long)det.time, (long long)(det.time - first.shots[0].spawn_time), double(det.position[0]), double(det.position[1]), double(det.position[2]),
				det.timeout ? 1 : 0);
		CHECK(det.time - first.shots[0].spawn_time == 216'000'000);
		const double yaw = double(yaw_radians(lane->yaw));
		const Vec3 left{ float(std::cos(yaw)), 0.0f, float(-std::sin(yaw)) };
		bool found = false;
		StaticHit floor_hit;
		const Vec3 probe{ det.position[0], det.position[1] + 0.5f, det.position[2] };
		found = m->world->ground_below(probe, 3.0f, query_mask::kCollision, &floor_hit);
		const float floor_y = found ? floor_hit.position[1] : det.position[1];
		std::vector<HitTarget> targets;
		const float offsets[] = { 1.0f, 3.0f, 5.0f, 8.0f };
		for (size_t i = 0; i < std::size(offsets); ++i) {
			const Vec3 feet{ det.position[0] + left[0] * offsets[i], floor_y, det.position[2] + left[2] * offsets[i] };
			core::Result<HitTarget> st = standin_target(w->weapons.soldier, w->weapons.capsules, *w->weapons.materials, Pose::Stand, feet, lane->yaw, uint32_t(i + 1));
			REQUIRE(st.ok());
			targets.push_back(*st);
		}
		const StaticTargets provider(targets);
		ProjectileWorld pw(t);
		REQUIRE(pw.spawn(first.shots[0]).ok());
		std::vector<DamageEvent> blast;
		std::vector<DetonationEvent> dets;
		ProjectileStepOutput out;
		for (Tick tick = tick_containing(first.shots[0].time); tick < tick_containing(first.shots[0].time) + 400 && !pw.live().empty(); ++tick) {
			out.clear();
			pw.step(tick, *m->world, provider, out);
			blast.insert(blast.end(), out.damage.begin(), out.damage.end());
			dets.insert(dets.end(), out.detonations.begin(), out.detonations.end());
		}
		REQUIRE(dets.size() == 1);
		CHECK(dets[0].position == det.position); // the stand-ins beside the rest point did not change the flight
		const ExplosionSimDef &x = t->defs[id(kM67)].projectile.explosion;
		std::array<bool, 4> hit{};
		for (const DamageEvent &d : blast) {
			CHECK(d.kind == DamageKind::Blast);
			const double want = blast_damage(x, double(d.distance), mech.blast_falloff) * double(x.zone_multiplier[size_t(d.zone)]);
			std::printf("  M67 blast on the stand-in %u m to the side: distance %.3f m, zone %s, %.3f hp (expected %.3f)\n", unsigned(offsets[d.target - 1]),
					double(d.distance), hit_zone_name(d.zone), double(d.amount), want);
			CHECK(std::fabs(double(d.amount) - want) <= 0.01);
			hit[d.target - 1] = true;
		}
		CHECK(hit[0]); // 1 m beside the blast
		CHECK_FALSE(hit[3]); // 8 m: beyond BlastRadius 7

		// RPG-7 at a stand-in 30 m down the lane (past the 0.5 s arm time: 30 m take about 0.65 s): the direct hit
		// (225 x 2.5, H:W7) and the armed blast. The shot leaves from the eye (no launch offset) aimed so the flight
		// (fly_free: thrust and gravity) meets the chest capsule's centre.
		const WeaponSimDef &rd = t->defs[id(kRpg)];
		const float dist = std::min(30.0f, lane->length - 1.0f);
		const Vec3 heading{ float(std::sin(yaw)), 0.0f, float(std::cos(yaw)) };
		const Vec3 tfeet{ lane->feet[0] + heading[0] * dist, lane->feet[1], lane->feet[2] + heading[2] * dist };
		core::Result<HitTarget> victim = standin_target(w->weapons.soldier, w->weapons.capsules, *w->weapons.materials, Pose::Stand, tfeet,
				uint16_t(lane->yaw + 32768u), 1);
		REQUIRE(victim.ok());
		Vec3 chest{};
		REQUIRE(zone_aim_point(*victim, HitZone::Chest, chest));
		const Vec3 eye = lane->eye;
		const double speed = std::sqrt(double(rd.projectile.launch_velocity[0]) * rd.projectile.launch_velocity[0] +
				double(rd.projectile.launch_velocity[1]) * rd.projectile.launch_velocity[1] + double(rd.projectile.launch_velocity[2]) * rd.projectile.launch_velocity[2]);
		Vec3 aim_point = chest;
		Vec3 dir{};
		for (int it = 0; it < 4; ++it) {
			const double dx = aim_point[0] - eye[0], dy = aim_point[1] - eye[1], dz = aim_point[2] - eye[2];
			const double n = std::sqrt(dx * dx + dy * dy + dz * dz);
			dir = { float(dx / n), float(dy / n), float(dz / n) };
			Projectile p0;
			p0.base_position = { eye[0], eye[1], eye[2] };
			p0.position = p0.base_position;
			p0.base_velocity = { dir[0] * speed, dir[1] * speed, dir[2] * speed };
			p0.velocity = p0.base_velocity;
			SimTime lo = 0, hi = 2 * kTimeUnitsPerSecond;
			for (int b = 0; b < 60; ++b) {
				const SimTime mid = (lo + hi) / 2;
				const Projectile q = fly_free(rd, p0, mid);
				const double h = std::hypot(q.position[0] - eye[0], q.position[2] - eye[2]);
				(h < std::hypot(double(chest[0]) - eye[0], double(chest[2]) - eye[2]) ? lo : hi) = mid;
			}
			const Projectile q = fly_free(rd, p0, lo);
			aim_point[1] += float(double(chest[1]) - q.position[1]);
		}
		ShotEvent shot;
		shot.def = id(kRpg);
		shot.seed = 1;
		shot.origin = eye;
		shot.aim = dir;
		shot.direction = dir;
		const StaticTargets one({ *victim });
		ProjectileWorld rp(t);
		REQUIRE(rp.spawn(shot).ok());
		std::vector<DamageEvent> rdmg;
		std::vector<DetonationEvent> rdet;
		for (Tick tick = 1; tick <= 600 && !rp.live().empty(); ++tick) {
			out.clear();
			rp.step(tick, *m->world, one, out);
			rdmg.insert(rdmg.end(), out.damage.begin(), out.damage.end());
			rdet.insert(rdet.end(), out.detonations.begin(), out.detonations.end());
		}
		bool direct = false, blasted = false;
		for (const DamageEvent &d : rdmg) {
			std::printf("  RPG-7 on a stand-in at %.1f m: %s %.2f hp at %lld (distance %.2f m, zone %s)\n", double(dist),
					d.kind == DamageKind::Direct ? "direct" : (d.kind == DamageKind::Blast ? "blast" : "other"), double(d.amount), (long long)d.time,
					double(d.distance), hit_zone_name(d.zone));
			if (d.kind == DamageKind::Direct) {
				direct = true;
				CHECK(d.amount == doctest::Approx(225.0f * 2.5f));
			}
			blasted = blasted || d.kind == DamageKind::Blast;
		}
		CHECK(direct);
		REQUIRE(rdet.size() == 1);
		CHECK_FALSE(rdet[0].dud);
		CHECK(rdet[0].time >= rd.projectile.time_to_arm);
		CHECK(blasted);
		check_install_unchanged(*w);
	}
}
