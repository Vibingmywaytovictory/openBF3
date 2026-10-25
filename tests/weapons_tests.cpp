#include "test_support.h"
#include "weapons_support.h"

#include "bf3/sim/soldier_motor.h"
#include "bf3/sim/soldier_state.h"
#include "bf3/sim/weapons/ballistics.h"
#include "bf3/sim/weapons/damage.h"
#include "bf3/sim/weapons/hit_target.h"
#include "bf3/sim/weapons/spread_recoil.h"
#include "bf3/sim/weapons/weapon_rng.h"
#include "bf3/sim/weapons/weapon_sim.h"
#include "bf3/sim_harness/shooting_range.h"
#include "bf3/sim_harness/weapon_checks.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <random>
#include <set>
#include <string>
#include <vector>

using namespace bf3;
using namespace bf3::sim;
using namespace bf3::sim::weapons;
using namespace bf3::test::weapons;

namespace {

constexpr const char *kM16A3 = "weapons/m16a4/u_m16a4";
constexpr const char *kM16A4 = "weapons/m16a4/u_m16_burst";
constexpr const char *kM98B = "weapons/model98b/u_m98b";
constexpr const char *k870 = "weapons/remington870/u_870";
constexpr const char *kM9 = "weapons/m9/u_m9";
constexpr const char *kRpg = "weapons/rpg7/u_rpg7";
constexpr const char *kM67 = "weapons/m67/u_m67";
constexpr const char *kKnife = "weapons/knife/u_knife";

const Synthetic &synthetic(const std::string &p_variant) {
	static const std::vector<Synthetic> all = synthetic_gate_weapons();
	for (const Synthetic &s : all) {
		if (s.def.variant == p_variant) {
			return s;
		}
	}
	FAIL("no synthetic " << p_variant);
	return all[0];
}

std::shared_ptr<const WeaponTable> table_of(std::vector<Synthetic> p_defs, bool p_infinite = false, const Mechanics &p_mechanics = {}) {
	auto materials = std::make_shared<const SurfaceMaterialTable>();
	auto t = std::make_shared<WeaponTable>();
	for (const Synthetic &s : p_defs) {
		t->defs.push_back(sim_def(s, *materials));
	}
	t->materials = materials;
	t->mechanics = p_mechanics;
	t->infinite_magazine = p_infinite;
	t->hash = weapon_table_hash(*t);
	return t;
}

struct Driver {
	std::shared_ptr<const WeaponTable> table;
	SoldierWeaponsState st;
	WeaponContext ctx;
	WeaponTickOutput out;
	uint64_t seed = 1;
	std::vector<ShotEvent> shots;
	std::vector<TimedWeaponEvent> timed;
	uint32_t all_events = 0;
	uint32_t refused = 0;
	uint32_t dropped = 0;

	explicit Driver(std::shared_ptr<const WeaponTable> p_table, std::vector<uint16_t> p_loadout, uint8_t p_active = 0) :
			table(std::move(p_table)) {
		ctx.eye_start = { 0, 1.6f, 0 };
		ctx.eye_end = ctx.eye_start;
		ctx.sway.zoom = Zoom::Zoom;
		const core::Status s = spawn_soldier_weapons(st, *table, p_loadout, p_active, 0);
		REQUIRE_MESSAGE(s.ok(), (s.ok() ? std::string() : s.error().message));
	}
	void step(const WeaponInput &p_in) {
		Command c;
		c.tick = st.tick + 1;
		c.buttons = with_weapon_input(0, p_in);
		step_soldier_weapons(*table, st, c, ctx, seed, out);
		shots.insert(shots.end(), out.shots.begin(), out.shots.begin() + out.shot_count);
		timed.insert(timed.end(), out.timed.begin(), out.timed.begin() + out.timed_count);
		all_events |= out.events;
		refused += out.refused_shots;
		dropped += out.timed_dropped;
	}
	void idle_until_ready() {
		for (int i = 0; i < 600 && st.current().phase != WeaponPhase::Ready; ++i) {
			step(WeaponInput{ .zoom = true });
		}
		REQUIRE(st.current().phase == WeaponPhase::Ready);
	}
	size_t count(uint32_t p_event) const {
		return size_t(std::count_if(timed.begin(), timed.end(), [&](const TimedWeaponEvent &e) { return e.event == p_event; }));
	}
	const TimedWeaponEvent *first(uint32_t p_event) const {
		for (const TimedWeaponEvent &e : timed) {
			if (e.event == p_event) {
				return &e;
			}
		}
		return nullptr;
	}
};

WeaponInput fire(bool p_fire, bool p_zoom = true) {
	WeaponInput in;
	in.fire = p_fire;
	in.zoom = p_zoom;
	return in;
}

ShotEvent level_shot(uint16_t p_def, const Vec3 &p_origin, SimTime p_time = 0, SimTime p_spawn_delay = 0, uint64_t p_seed = 1, uint32_t p_index = 0) {
	ShotEvent s;
	s.time = p_time;
	s.spawn_time = p_time + p_spawn_delay;
	s.seed = p_seed;
	s.shot_index = p_index;
	s.def = p_def;
	s.origin = p_origin;
	s.aim = { 0, 0, 1 };
	s.direction = { 0, 0, 1 };
	return s;
}

struct Flown {
	std::vector<ImpactEvent> impacts;
	std::vector<DamageEvent> damage;
	std::vector<DetonationEvent> detonations;
	size_t invisible = 0;
};
Flown fly(ProjectileWorld &r_pw, const CollisionWorld &p_world, const HitTargetProvider &p_targets, Tick p_from, Tick p_to) {
	Flown f;
	ProjectileStepOutput out;
	for (Tick t = p_from; t <= p_to; ++t) {
		out.clear();
		r_pw.step(t, p_world, p_targets, out);
		f.impacts.insert(f.impacts.end(), out.impacts.begin(), out.impacts.end());
		f.damage.insert(f.damage.end(), out.damage.begin(), out.damage.end());
		f.detonations.insert(f.detonations.end(), out.detonations.begin(), out.detonations.end());
		f.invisible += out.invisible_passes;
	}
	return f;
}

double len3(const Vec3 &p_v) {
	return std::sqrt(double(p_v[0]) * p_v[0] + double(p_v[1]) * p_v[1] + double(p_v[2]) * p_v[2]);
}
double angle_between(const Vec3 &p_a, const Vec3 &p_b) {
	const double c = (double(p_a[0]) * p_b[0] + double(p_a[1]) * p_b[1] + double(p_a[2]) * p_b[2]) / (len3(p_a) * len3(p_b));
	return std::acos(std::clamp(c, -1.0, 1.0)) * 180.0 / 3.14159265358979323846;
}

}

TEST_SUITE("weapons-unit") {
	TEST_CASE("sim.weapons.unit: the oracle rows on synthetic defs with the 7.1 numbers") {
		const std::shared_ptr<const WeaponTable> table = synthetic_table();
		const std::shared_ptr<const WeaponTable> oracle = synthetic_table(true);
		WeaponCheckInputs in;
		in.table = table;
		in.oracle = oracle;
		const std::vector<WeaponCheckRow> rows = run_weapon_checks(in);
		std::printf("%s", format_weapon_checks(rows).c_str());
		size_t gated = 0;
		std::set<std::string> gates;
		for (const WeaponCheckRow &r : rows) {
			CHECK_MESSAGE(r.pass, r.gate << " " << r.name << ": " << r.measured << " (" << r.threshold << ")");
			gated += r.reported ? 0 : 1;
			gates.insert(r.gate);
		}
		CHECK(gated >= 30);
		CHECK(gates == std::set<std::string>{ "W1", "W2", "W3", "W5", "W6", "W9" });

		for (const std::string_view v : weapon_check_variants()) {
			bool found = false;
			for (const WeaponSimDef &d : table->defs) {
				found = found || d.variant == v;
			}
			CHECK_MESSAGE(found, std::string(v));
		}
	}

	TEST_CASE("sim.weapons.unit: def build: sub-tick units, saturation, the table hash") {
		const std::shared_ptr<const WeaponTable> t = synthetic_table();
		const WeaponSimDef &a3 = t->defs[id_of(*t, kM16A3)];
		CHECK(a3.modes[0].interval == 4'500'000);
		CHECK(a3.mode_count == 2);
		CHECK(a3.modes[1].logic == FireLogic::Single);
		CHECK(a3.reload_tactical == 108'000'000);
		CHECK(a3.reload_empty == 142'200'000);
		CHECK(a3.reload_commit_tactical == 91'800'000);
		CHECK(a3.reload_commit_empty == 120'870'000);
		CHECK(a3.deploy_time == 40'200'000);
		CHECK(a3.trace_frequency == 1);
		CHECK(a3.projectile.muzzle_speed == 650.0f);
		CHECK(a3.projectile.time_to_live == 90'000'000);
		CHECK(t->defs[id_of(*t, kM98B)].bolt_cycle == 78'000'000);
		CHECK(t->defs[id_of(*t, kM98B)].bolt_hold_until_fire_release);
		CHECK(t->defs[id_of(*t, kM98B)].hold_off_reload_until_fire_release);
		const WeaponSimDef &s870 = t->defs[id_of(*t, k870)];
		CHECK(s870.bolt_cycle == 28'800'000);
		CHECK_FALSE(s870.bolt_hold_until_fire_release);
		CHECK(s870.pellets == 12);
		CHECK(s870.pellet_cone[0].min_angle == 3.0f);
		CHECK(s870.reload_delay == 69'000'000);
		CHECK(s870.reload_empty == 40'200'000);
		const WeaponSimDef &m67 = t->defs[id_of(*t, kM67)];
		CHECK(m67.hold_max_time == kMaxDefTime);
		CHECK(m67.spawn_delay == 10'200'000);
		CHECK(m67.projectile.detonate_on_timeout);
		CHECK_FALSE(m67.switch_cancels_reload);
		const WeaponSimDef &knife = t->defs[id_of(*t, kKnife)];
		CHECK(knife.unlimited_ammo);
		CHECK(knife.trace_frequency == 0);
		CHECK(knife.projectile.instant_hit);
		const WeaponSimDef &he = t->defs[id_of(*t, "weapons/gadgets/m320/u_m320_he")];
		CHECK(he.projectile.time_to_live == kMaxFlightTime);
		CHECK(he.projectile.time_to_arm == 30'000'000);
		CHECK(sim_shot_interval_seconds(a3) == doctest::Approx(0.075));
		CHECK(sim_effective_rpm(t->defs[id_of(*t, kM98B)]) == doctest::Approx(60.0 / 1.3));

		CHECK(weapon_table_hash(*synthetic_table()) == t->hash);
		CHECK(synthetic_table(true)->hash != t->hash);
		Mechanics m;
		m.disc = DiscDistribution::UniformRadius;
		CHECK(synthetic_table(false, m)->hash != t->hash);
		m = {};
		m.recovery = RecoveryShape::Exponential;
		CHECK(synthetic_table(false, m)->hash != t->hash);
		auto changed = std::make_shared<WeaponTable>(*t);
		changed->sprint_recover_time += 1;
		CHECK(weapon_table_hash(*changed) != t->hash);
		changed = std::make_shared<WeaponTable>(*t);
		changed->defs[0].reload_commit_tactical += 1;
		CHECK(weapon_table_hash(*changed) != t->hash);

		std::vector<core::gameplay::WeaponDef> defs;
		for (const Synthetic &s : synthetic_gate_weapons()) {
			defs.push_back(s.def);
		}
		core::Result<std::shared_ptr<const WeaponTable>> built = make_weapon_table(defs, std::make_shared<const SurfaceMaterialTable>());
		REQUIRE_MESSAGE(built.ok(), (built.ok() ? std::string() : built.error().message));
		CHECK((*built)->defs.size() == defs.size());
		CHECK((*built)->hash == weapon_table_hash(**built));
	}

	TEST_CASE("sim.weapons.unit: W8 hostile def fields are refused with the field named") {
		const auto materials = std::make_shared<const SurfaceMaterialTable>();
		struct Case {
			const char *field;
			std::function<void(core::gameplay::WeaponDef &)> mutate;
		};
		const float nan = std::numeric_limits<float>::quiet_NaN();
		const float inf = std::numeric_limits<float>::infinity();
		const std::vector<Case> cases = {
			{ "RateOfFire", [&](auto &w) { w.fire.rate_of_fire = nan; } },
			{ "RateOfFire", [](auto &w) { w.fire.rate_of_fire = 0; } },
			{ "RateOfFire", [](auto &w) { w.fire.rate_of_fire = -800; } },
			{ "RateOfFire", [](auto &w) { w.fire.rate_of_fire = 28'801; } },
			{ "RateOfFireForBurst", [](auto &w) {
				 w.fire.logic = core::gameplay::FireLogic::Burst;
				 w.fire.rate_of_fire_for_burst = 1e6f;
			 } },
			{ "NumberOfBulletsPerBurst", [](auto &w) {
				 w.fire.logic = core::gameplay::FireLogic::Burst;
				 w.fire.bullets_per_burst = 33;
			 } },
			{ "NumberOfBulletsPerBurst", [](auto &w) {
				 w.fire.logic = core::gameplay::FireLogic::Burst;
				 w.fire.bullets_per_burst = 0;
			 } },
			{ "NumberOfBulletsPerShell", [](auto &w) { w.fire.bullets_per_shell = 65; } },
			{ "NumberOfBulletsPerShell", [](auto &w) { w.fire.bullets_per_shell = 0; } },
			{ "MagazineCapacity", [](auto &w) { w.fire.magazine_capacity = 10'001; } },
			{ "MagazineCapacity", [](auto &w) { w.fire.magazine_capacity = -2; } },
			{ "NumberOfMagazines", [](auto &w) { w.fire.number_of_magazines = 1'001; } },
			{ "TraceFrequency", [](auto &w) { w.fire.trace_frequency = 1'001; } },
			{ "ShotConfigData.InitialSpeed", [](auto &w) { w.fire.initial_speed = { 0, 0, 5'001 }; } },
			{ "ShotConfigData.InitialSpeed", [&](auto &w) { w.fire.initial_speed = { 0, 0, inf }; } },
			{ "ShotConfigData.InitialPosition", [&](auto &w) { w.fire.initial_position = { nan, 0, 0 }; } },
			{ "ProjectileData.Gravity", [](auto &w) { w.projectile.gravity = -101; } },
			{ "ProjectileData.TimeToLive", [](auto &w) { w.projectile.time_to_live = 61; } },
			{ "BulletEntityData.StartDamage", [](auto &w) { w.projectile.start_damage = 1e6f; } },
			{ "BulletEntityData.StartDamage", [&](auto &w) { w.projectile.start_damage = nan; } },
			{ "BulletEntityData.DamageFalloffEndDistance", [&](auto &w) { w.projectile.damage_falloff_end = inf; } },
			{ "DeployTime", [](auto &w) { w.deploy_time = -1; } },
			{ "DeployTime", [](auto &w) { w.deploy_time = 61; } },
			{ "ReloadThreshold", [](auto &w) { w.fire.reload_threshold = 1.5f; } },
			{ "ReloadTimeBulletsLeft", [](auto &w) { w.fire.reload_time_bullets_left = 61; } },
			{ "ReloadType", [](auto &w) { w.fire.reload_type = core::gameplay::ReloadType::Unknown; } },
			{ "ReloadTime", [](auto &w) {
				 w.fire.reload_type = core::gameplay::ReloadType::SingleBullet;
				 w.fire.reload_time = 0;
			 } },
			{ "BoltActionTime", [](auto &w) {
				 w.fire.logic = core::gameplay::FireLogic::SingleWithBoltAction;
				 w.fire.bolt_action_time = 61;
			 } },
			{ "MinPowerModifier", [](auto &w) {
				 w.fire.logic = core::gameplay::FireLogic::HoldAndRelease;
				 w.fire.hold_min_power = 2;
				 w.fire.hold_max_power = 1;
			 } },
			{ "MaxPowerModifier", [](auto &w) {
				 w.fire.logic = core::gameplay::FireLogic::HoldAndRelease;
				 w.fire.hold_max_power = 11;
			 } },
			{ "MaxHoldTime", [&](auto &w) {
				 w.fire.logic = core::gameplay::FireLogic::HoldAndRelease;
				 w.fire.hold_max_time = nan;
			 } },
			{ "BlastRadius", [](auto &w) { w.projectile.explosion = explosion(112, 0.35f, 101); } },
			{ "BlastDamage", [](auto &w) { w.projectile.explosion = explosion(1e6f, 0.35f, 5); } },
			{ "MissileEntityData.MaxSpeed", [](auto &w) {
				 w.projectile.kind = core::gameplay::ProjectileKind::Missile;
				 w.projectile.max_speed = 5'001;
			 } },
			{ "", [](auto &w) { w.fire.logic = core::gameplay::FireLogic::Unknown; } },
			{ "", [](auto &w) { w.fire.extra_modes = { core::gameplay::FireLogic::Single, core::gameplay::FireLogic::Burst, core::gameplay::FireLogic::Single,
											  core::gameplay::FireLogic::Automatic }; } },
			{ "", [](auto &w) { w.sway.state[0][0].move[0].max_angle = 91; } },
			{ "", [&](auto &w) { w.sway.state[1][1].recoil.up_per_shot = nan; } },
			{ "", [](auto &w) { w.sway.state[2][0].move[1].decrease_per_second = 1e5f; } },
			{ "", [](auto &w) { w.sway.first_shot_recoil_multiplier = -1; } },
			{ "DecreasePerSecondModifier", [](auto &w) { w.sway.suppression[0].decrease_per_second = -1e5f; } },
			{ "RecoilAngleModifier", [&](auto &w) { w.sway.suppression[1].recoil_angle = nan; } },
			{ "", [](auto &w) { w.fire.weapon_dispersion[0].min_angle = 91; } },
		};
		const Synthetic &base = synthetic(kM16A3);
		size_t refused = 0;
		for (size_t i = 0; i < cases.size(); ++i) {
			core::gameplay::WeaponDef w = base.def;
			cases[i].mutate(w);
			const core::Result<WeaponSimDef> d = make_weapon_sim_def(w, *materials);
			CAPTURE(i);
			CAPTURE(cases[i].field);
			REQUIRE_FALSE(d.ok());
			const std::string &msg = d.error().message;
			CHECK_MESSAGE(msg.rfind(std::string(kM16A3) + ": ", 0) == 0, msg);
			CHECK_MESSAGE(msg.find(cases[i].field) != std::string::npos, msg);
			refused += d.ok() ? 0 : 1;
		}
		std::printf("W8 bounds: %zu of %zu hostile defs refused with the variant and field named\n", refused, cases.size());

		core::gameplay::WeaponDef w = base.def;
		w.fire.rate_of_fire = 28'800;
		w.fire.bullets_per_shell = 64;
		w.deploy_time = 60;
		core::Result<WeaponSimDef> ok = make_weapon_sim_def(w, *materials);
		REQUIRE_MESSAGE(ok.ok(), (ok.ok() ? std::string() : ok.error().message));
		CHECK(ok->modes[0].interval == 125'000);
		CHECK(ok->pellets == 64);
		w = base.def;
		w.fire.logic = core::gameplay::FireLogic::Burst;
		w.fire.bullets_per_burst = 32;
		CHECK(make_weapon_sim_def(w, *materials).ok());
		CHECK(make_weapon_sim_def(synthetic(k870).def, *materials).ok());

		w = base.def;
		w.sway.suppression[0].decrease_per_second = -10;
		w.sway.suppression[0].min_angle = -2;
		core::Result<WeaponSimDef> lmg = make_weapon_sim_def(w, *materials);
		REQUIRE_MESSAGE(lmg.ok(), (lmg.ok() ? std::string() : lmg.error().message));
		SwayContext supp;
		supp.suppressed = true;
		const Dispersion sd = effective_dispersion(*lmg, supp);
		CHECK(sd.min_angle == 0.0f);
		CHECK(sd.decrease_per_second < 0);
		CHECK(decay_spread(1.0f, sd, kTimeUnitsPerSecond / 100) > 1.0f);
		CHECK(decay_spread(1.0f, sd, kTimeUnitsPerSecond * 100) == sd.max_angle);
		CHECK(make_weapon_sim_def(synthetic(kKnife).def, *materials).ok());
	}

	TEST_CASE("sim.weapons.unit: W1 modes: cycling, sub-tick origins, the per-tick cap, sprint and death") {

		{
			Driver d(synthetic_table(), { id_of(*synthetic_table(), kM16A3) });
			d.idle_until_ready();
			WeaponInput in = fire(false);
			in.fire_mode_counter = 1;
			d.step(in);
			CHECK((d.out.events & weapon_events::kFireModeChanged) != 0);
			CHECK(d.st.current().mode == 1);
			in.fire = true;
			for (int i = 0; i < 60; ++i) {
				d.step(in);
			}
			CHECK(d.shots.size() == 1);
			in.fire = false;
			in.fire_mode_counter = 3;
			d.step(in);
			CHECK(d.st.current().mode == 1);
			in.fire_mode_counter = 0;
			d.step(in);
			CHECK(d.st.current().mode == 0);
			d.step(in);
			CHECK((d.out.events & weapon_events::kFireModeChanged) == 0);
		}

		{
			const std::shared_ptr<const WeaponTable> t = synthetic_table(true);
			Driver d(t, { id_of(*t, kM16A3) });
			d.idle_until_ready();
			d.ctx.eye_start = { 0, 1.6f, 0 };
			d.ctx.eye_end = { 0.1f, 1.6f, 0 };
			for (int i = 0; i < 120; ++i) {
				d.step(fire(true));
			}
			REQUIRE(d.shots.size() >= 26);
			size_t off_tick = 0;
			double worst = 0;
			for (const ShotEvent &s : d.shots) {
				const double f = tick_fraction(s.time);
				off_tick += f > 0 ? 1 : 0;
				worst = std::max(worst, std::fabs(double(s.origin[0]) - 0.1 * f));
			}
			CHECK(off_tick >= d.shots.size() / 2);
			CHECK(worst <= 1e-6);
			MESSAGE("sub-tick shots: " << off_tick << " of " << d.shots.size() << " between ticks, origin error " << worst << " m");
		}

		{
			Synthetic fast = synthetic(kM16A3);
			fast.def.fire.rate_of_fire = 28'800;
			const std::shared_ptr<const WeaponTable> t = table_of({ fast }, true);
			Driver d(t, { 0 });
			d.idle_until_ready();
			bool eight = true;
			for (int i = 0; i < 60; ++i) {
				d.step(fire(true));
				eight = eight && d.out.shot_count == kMaxShotsPerTick;
			}
			CHECK(eight);
			CHECK(d.refused == 0);
			for (size_t i = 1; i < d.shots.size(); ++i) {
				REQUIRE(d.shots[i].time - d.shots[i - 1].time == 125'000);
			}
		}

		{
			const std::shared_ptr<const WeaponTable> t = synthetic_table();
			Driver d(t, { id_of(*t, kM16A3) });
			d.idle_until_ready();
			d.ctx.sprinting = true;
			for (int i = 0; i < 10; ++i) {
				d.step(fire(true));
			}
			CHECK(d.shots.empty());
			CHECK((d.all_events & weapon_events::kFireBlocked) != 0);
			d.ctx.sprinting = false;
			const SimTime sprint_end = tick_end(d.st.tick);
			for (int i = 0; i < 30; ++i) {
				d.step(fire(true));
			}
			REQUIRE_FALSE(d.shots.empty());
			CHECK(d.shots[0].time >= sprint_end + t->sprint_recover_time);
			CHECK(d.shots[0].time < sprint_end + t->sprint_recover_time + kTimeUnitsPerTick);
			const size_t before = d.shots.size();
			d.ctx.alive = false;
			for (int i = 0; i < 30; ++i) {
				d.step(fire(true));
			}
			CHECK(d.shots.size() == before);
		}

		{
			Synthetic empty = synthetic(kM16A3);
			empty.def.fire.magazine_capacity = 2;
			empty.def.fire.number_of_magazines = 0;
			Driver d(table_of({ empty }), { 0 });
			d.idle_until_ready();
			for (int i = 0; i < 30; ++i) {
				d.step(fire(true));
			}
			CHECK(d.shots.size() == 2);
			CHECK((d.all_events & weapon_events::kDryFire) == 0);
			d.step(fire(false));
			d.step(fire(true));
			CHECK((d.out.events & weapon_events::kDryFire) != 0);
			CHECK(d.count(weapon_events::kReloadStarted) == 0);
		}

		{
			const std::shared_ptr<const WeaponTable> t = synthetic_table();
			Driver d(t, { id_of(*t, kKnife) });
			d.idle_until_ready();
			for (int i = 0; i < 60; ++i) {
				d.step(fire(i % 2 == 0));
			}
			REQUIRE_FALSE(d.shots.empty());
			CHECK(std::none_of(d.shots.begin(), d.shots.end(), [](const ShotEvent &s) { return s.tracer; }));
			CHECK(d.shots[0].spawn_time - d.shots[0].time == 18'000'000);
		}
	}

	TEST_CASE("sim.weapons.unit: W6 reloads across switches, the 870 interrupt, a refused switch") {
		const std::shared_ptr<const WeaponTable> t = synthetic_table();
		const uint16_t a3 = id_of(*t, kM16A3), m9 = id_of(*t, kM9), s870 = id_of(*t, k870), m67 = id_of(*t, kM67);
		auto select = [](uint8_t p_slot) {
			WeaponInput in = fire(false);
			in.select_slot = p_slot;
			return in;
		};
		auto reload = [] {
			WeaponInput in = fire(false);
			in.reload = true;
			return in;
		};

		{
			Driver d(t, { a3, m9 });
			d.idle_until_ready();
			for (int i = 0; i < 5; ++i) {
				d.step(fire(true));
			}
			d.step(fire(false));
			const int32_t mag = d.st.slots[0].magazine;
			const int32_t reserve = d.st.slots[0].reserve;
			CHECK(mag < 31);
			d.step(reload());
			REQUIRE(d.st.current().phase == WeaponPhase::Reloading);
			for (int i = 0; i < 30; ++i) {
				d.step(fire(false));
			}
			d.step(select(1));
			CHECK(d.count(weapon_events::kReloadCancelled) == 1);
			CHECK(d.st.active == 1);
			CHECK(d.st.slots[0].magazine == mag);
			CHECK(d.st.slots[0].reserve == reserve);
			CHECK(d.st.slots[0].phase == WeaponPhase::Ready);
			CHECK(d.st.slots[1].phase == WeaponPhase::Deploying);
		}

		{
			Driver d(t, { a3, m9 });
			d.idle_until_ready();
			for (int i = 0; i < 5; ++i) {
				d.step(fire(true));
			}
			d.step(fire(false));
			const int32_t reserve = d.st.slots[0].reserve;
			const int32_t mag = d.st.slots[0].magazine;
			d.step(reload());
			for (int i = 0; i < 95; ++i) {
				d.step(fire(false));
			}
			CHECK(d.count(weapon_events::kReloadCommitted) == 1);
			d.step(select(1));
			CHECK(d.count(weapon_events::kReloadCancelled) == 0);
			CHECK(d.st.slots[0].magazine == 31);
			CHECK(d.st.slots[0].reserve == reserve - (31 - mag));
		}

		{
			Driver d(t, { a3, m9 });
			d.idle_until_ready();
			d.step(select(3));
			CHECK((d.out.events & weapon_events::kSwitchRefused) != 0);
			CHECK(d.st.active == 0);
			CHECK(d.st.pending == 0xFF);
		}

		{
			Driver d(t, { m67, a3 });
			d.idle_until_ready();
			for (int i = 0; i < 40; ++i) {
				d.step(fire(true));
			}
			d.step(fire(false));
			for (int i = 0; i < 30 && d.st.current().phase != WeaponPhase::Reloading; ++i) {
				d.step(fire(false));
			}
			REQUIRE(d.st.current().phase == WeaponPhase::Reloading);
			d.step(select(1));
			CHECK(d.count(weapon_events::kReloadCancelled) == 0);
			CHECK(d.st.slots[0].magazine == 1);
		}

		{
			Driver d(t, { s870 });
			d.idle_until_ready();
			d.step(fire(true));
			d.step(fire(false));
			for (int i = 0; i < 40; ++i) {
				d.step(fire(false));
			}
			d.step(fire(true));
			d.step(fire(false));
			for (int i = 0; i < 40; ++i) {
				d.step(fire(false));
			}
			REQUIRE(d.shots.size() == 2);
			d.step(reload());
			for (int i = 0; i < 120 && d.count(weapon_events::kShellLoaded) == 0; ++i) {
				d.step(fire(false));
			}
			REQUIRE(d.count(weapon_events::kShellLoaded) == 1);
			CHECK(d.st.current().magazine == 4);
			d.step(fire(true));
			for (int i = 0; i < 60; ++i) {
				d.step(fire(i == 0));
			}
			CHECK(d.count(weapon_events::kReloadFinished) == 1);
			CHECK(d.shots.size() == 3);
			CHECK(d.st.current().magazine == 3);
		}
	}

	TEST_CASE("sim.weapons.unit: spread and recoil arithmetic, the disc variants' moments") {
		const Dispersion disp{ 0.5f, 3.0f, 0.2f, 10.0f };
		CHECK(decay_spread(2.0f, disp, kTimeUnitsPerSecond / 10) == doctest::Approx(1.0f));
		CHECK(decay_spread(2.0f, disp, kTimeUnitsPerSecond) == 0.5f);
		CHECK(decay_spread(0.2f, disp, kTimeUnitsPerSecond) == 0.2f);
		CHECK(decay_spread(5.0f, disp, 0) == 3.0f);
		CHECK(spread_after_shot(2.9f, disp) == 3.0f);
		CHECK(spread_after_shot(1.0f, disp) == doctest::Approx(1.2f));
		Mechanics m;
		CHECK(enter_spread(0.1f, disp, m) == 0.5f);
		m.min_spread_rise = MinSpreadRise::AtDecreaseRate;
		CHECK(enter_spread(0.1f, disp, m) == 0.1f);

		const Vec3 aim{ 0.3f, 0.1f, 0.95f };
		for (const DiscDistribution disc : { DiscDistribution::UniformArea, DiscDistribution::UniformRadius }) {
			double sum_r = 0, sum_r2 = 0, worst = 0, len_err = 0;
			constexpr int kDraws = 100'000;
			for (int i = 0; i < kDraws; ++i) {
				const uint64_t seed = shot_seed(7, 3, uint32_t(i));
				const Vec3 v = sample_cone(aim, 2.0f, unit_float(shot_random(seed, RngPurpose::SpreadRadius)), unit_float(shot_random(seed, RngPurpose::SpreadAngle)), disc);
				const double a = angle_between(v, aim) / 2.0;
				sum_r += a;
				sum_r2 += a * a;
				worst = std::max(worst, a);
				len_err = std::max(len_err, std::fabs(len3(v) - 1.0));
			}
			const double mean_r = sum_r / kDraws, mean_r2 = sum_r2 / kDraws;
			MESSAGE((disc == DiscDistribution::UniformArea ? "uniform area" : "uniform radius") << ": E[r/R] " << mean_r << ", E[(r/R)^2] " << mean_r2);
			CHECK(worst <= 1.0 + 1e-4);
			CHECK(len_err <= 1e-6);
			if (disc == DiscDistribution::UniformArea) {
				CHECK(mean_r == doctest::Approx(2.0 / 3.0).epsilon(0.01));
				CHECK(mean_r2 == doctest::Approx(0.5).epsilon(0.01));
			} else {
				CHECK(mean_r == doctest::Approx(0.5).epsilon(0.01));
				CHECK(mean_r2 == doctest::Approx(1.0 / 3.0).epsilon(0.01));
			}
		}
		CHECK(sample_cone(aim, 0.0f, 0.5f, 0.5f, DiscDistribution::UniformArea)[2] == doctest::Approx(aim[2] / len3(aim)));

		const Recoil rec{ 0.4f, 1.0f, -0.3f, 0.2f, 0.5f, 6.0f };
		RecoilOffset o{};
		o = recoil_after_shot(o, rec, true, 2.5f, 0.0f);
		CHECK(o.pitch == doctest::Approx(1.0f));
		CHECK(o.yaw == doctest::Approx(-0.3f));
		for (int i = 0; i < 10; ++i) {
			o = recoil_after_shot(o, rec, false, 2.5f, 0.0f);
		}
		CHECK(o.pitch == 1.0f);
		CHECK(o.yaw == -0.5f);
		o = recoil_after_shot({}, rec, false, 2.5f, 1.0f);
		CHECK(o.yaw == doctest::Approx(0.2f));

		const std::shared_ptr<const WeaponTable> t = synthetic_table();
		const WeaponSimDef &a3 = t->defs[id_of(*t, kM16A3)];
		SwayContext ctx;
		Mechanics lin;
		const RecoilOffset lr = recover_recoil_tick({ 1.0f, -1.0f }, a3, ctx, false, lin);
		CHECK(lr.pitch == doctest::Approx(1.0f - 18.0f / 60.0f));
		CHECK(lr.yaw == doctest::Approx(-1.0f + 18.0f / 60.0f));
		const RecoilOffset lf = recover_recoil_tick({ 1.0f, 0.0f }, a3, ctx, true, lin);
		CHECK(lf.pitch == doctest::Approx(1.0f - 1.8f / 60.0f));
		CHECK(recover_recoil_tick({ 0.1f, 0.0f }, a3, ctx, false, lin).pitch == 0.0f);
		Mechanics ex;
		ex.recovery = RecoveryShape::Exponential;
		CHECK(recover_recoil_tick({ 1.0f, 0.0f }, a3, ctx, false, ex).pitch == doctest::Approx(std::exp(-18.0 / 60.0)));
		CHECK(recover_recoil_tick({ 1.0f, 0.0f }, a3, ctx, true, ex).pitch == doctest::Approx(std::exp(-1.8 / 60.0)));

		SwayContext crouch_sprint;
		crouch_sprint.stance = Stance::Crouch;
		crouch_sprint.move = SwayMove::Sprinting;
		CHECK(effective_dispersion(a3, crouch_sprint).min_angle == a3.sway[1][0].move[0].min_angle);
		SwayContext supp;
		supp.suppressed = true;
		CHECK(effective_dispersion(a3, supp).min_angle == doctest::Approx(a3.sway[0][0].move[0].min_angle * 1.5f));
		CHECK(effective_dispersion(a3, supp).max_angle == a3.sway[0][0].move[0].max_angle);
		CHECK(effective_recoil(a3, supp).horizontal_max == doctest::Approx(a3.sway[0][0].recoil.horizontal_max * 1.5f));

		const Vec3 n = aim_direction(0, 0, {});
		CHECK(n[2] == doctest::Approx(1.0f));
		CHECK(std::fabs(n[0]) < 1e-6f);
		const Vec3 up = aim_direction(0, kPitchLimit, { 10.0f, 0.0f });
		CHECK(up[1] == doctest::Approx(std::sin(89.99 * 3.14159265358979323846 / 180.0)).epsilon(1e-6));
	}

	TEST_CASE("sim.weapons.unit: the spread without a shot: stance, move, jump, zoom, decay, rise, and the shot evaluates the same function") {
		const std::shared_ptr<const WeaponTable> t = synthetic_table();
		WeaponSimDef d = t->defs[id_of(*t, kM16A3)];
		const float hip[3][2] = { { 2.5f, 3.0f }, { 2.0f, 2.5f }, { 1.5f, 2.0f } };
		for (size_t s = 0; s < 3; ++s) {
			SwayState &st = d.sway[s][0];
			st.move[size_t(SwayMove::Base)].min_angle = hip[s][0];
			st.move[size_t(SwayMove::Moving)].min_angle = hip[s][1];
			st.has_move[size_t(SwayMove::Moving)] = true;
		}
		d.sway[0][0].move[size_t(SwayMove::Jumping)].min_angle = 7.0f;
		d.sway[0][0].has_move[size_t(SwayMove::Jumping)] = true;
		d.sway[0][1].move[size_t(SwayMove::Base)].min_angle = 0.2f;
		const Mechanics immediate;
		Mechanics rise;
		rise.min_spread_rise = MinSpreadRise::AtDecreaseRate;
		const SimTime now = 600 * kTimeUnitsPerTick;

		MotorParams motor;
		for (PoseParams &pp : motor.poses) {
			pp.on_ground.velocity = 2.0f;
			pp.on_ground.backward = 0.5f;
			pp.on_ground.left = 0.75f;
			pp.on_ground.right = 0.75f;
			pp.throttle.points[0] = { 0.0f, 1.0f };
			pp.throttle.count = 1;
		}
		SoldierState still;
		still.flags = soldier_flags::kAlive;
		Command c;
		c.tick = 600;
		const auto spread_now = [&](const SoldierState &p_state, const Command &p_command, float p_stored, SimTime p_stored_time, const Mechanics &p_mech) {
			return spread_at(p_stored, p_stored_time, now, effective_dispersion(d, weapon_sway_context(motor, p_state, p_command)), p_mech);
		};
		CHECK(weapon_sway_context(motor, still, c).move == SwayMove::Base);
		CHECK(spread_now(still, c, 0.0f, 0, immediate) == 2.5f);

		SoldierState walking = still;
		walking.velocity = { 0.0f, 0.0f, 2.0f };
		CHECK(walk_speed(motor, walking) == 2.0f);
		CHECK(weapon_sway_context(motor, walking, c).move == SwayMove::Moving);
		CHECK(weapon_sway_context(motor, walking, c).move_fraction == 1.0f);
		CHECK(spread_now(walking, c, 2.5f, now - kTimeUnitsPerTick, immediate) == 3.0f);
		SoldierState half = still;
		half.velocity = { 0.0f, 0.0f, 1.0f };
		CHECK(weapon_sway_context(motor, half, c).move_fraction == 0.5f);
		CHECK(spread_now(half, c, 0.0f, 0, immediate) == 2.75f);
		SoldierState backing = still;
		backing.velocity = { 0.0f, 0.0f, -0.5f };
		CHECK(walk_speed(motor, backing) == 1.0f);
		CHECK(spread_now(backing, c, 0.0f, 0, immediate) == 2.75f);
		SoldierState sidestep = still;
		sidestep.velocity = { 0.75f, 0.0f, 0.0f };
		CHECK(walk_speed(motor, sidestep) == 1.5f);
		CHECK(spread_now(sidestep, c, 0.0f, 0, immediate) == 2.75f);
		SoldierState turned = still;
		turned.yaw = uint16_t(kYawSteps / 4);
		turned.velocity = { 2.0f, 0.0f, 0.0f };
		CHECK(walk_speed(motor, turned) == doctest::Approx(2.0f));
		CHECK(spread_now(turned, c, 0.0f, 0, immediate) == doctest::Approx(3.0f));
		SoldierState creeping = still;
		creeping.velocity = { 0.05f, 0.0f, 0.05f };
		const float creep_walk = 2.0f * (0.5f * 1.0f + 0.5f * 0.75f);
		CHECK(walk_speed(motor, creeping) == doctest::Approx(creep_walk));
		CHECK(weapon_sway_context(motor, creeping, c).move == SwayMove::Moving);
		CHECK(spread_now(creeping, c, 0.0f, 0, immediate) == doctest::Approx(2.5f + 0.5f * std::sqrt(0.005f) / creep_walk));
		CHECK(spread_at(0.0f, 0, now, effective_dispersion(d, weapon_sway_context(MotorParams{}, half, c)), immediate) == 3.0f);

		SoldierState airborne = still;
		airborne.mode = MoveMode::Jumping;
		CHECK(weapon_sway_context(motor, airborne, c).move == SwayMove::Jumping);
		CHECK(spread_now(airborne, c, 2.5f, now - kTimeUnitsPerTick, immediate) == 7.0f);
		SoldierState falling = still;
		falling.mode = MoveMode::Falling;
		CHECK(weapon_sway_context(motor, falling, c).move == SwayMove::Jumping);
		SoldierState sprint = walking;
		sprint.flags = uint16_t(soldier_flags::kAlive | soldier_flags::kSprinting);
		CHECK(weapon_sway_context(motor, sprint, c).move == SwayMove::Sprinting);

		SoldierState crouched = still;
		crouched.target_pose = Pose::Crouch;
		SoldierState prone = still;
		prone.target_pose = Pose::Prone;
		CHECK(weapon_sway_context(motor, crouched, c).stance == Stance::Crouch);
		CHECK(weapon_sway_context(motor, prone, c).stance == Stance::Prone);
		CHECK(spread_now(crouched, c, 0.0f, 0, immediate) == 2.0f);
		CHECK(spread_now(prone, c, 0.0f, 0, immediate) == 1.5f);
		SoldierState crouch_walk = crouched;
		crouch_walk.velocity = walking.velocity;
		CHECK(spread_now(crouch_walk, c, 0.0f, 0, immediate) == 2.5f);
		SoldierState prone_crawl = prone;
		prone_crawl.velocity = walking.velocity;
		CHECK(spread_now(prone_crawl, c, 0.0f, 0, immediate) == 2.0f);

		Command zoom = c;
		zoom.buttons = buttons::kZoom;
		CHECK(weapon_sway_context(motor, still, zoom).zoom == Zoom::Zoom);
		CHECK(spread_now(still, zoom, 0.0f, 0, immediate) == 0.2f);

		const float decrease = d.sway[0][0].move[0].decrease_per_second;
		CHECK(decrease == 15.0f);
		CHECK(spread_now(still, c, 6.0f, now, immediate) == 6.0f);
		CHECK(spread_now(still, c, 6.0f, now - kTimeUnitsPerSecond / 10, immediate) == doctest::Approx(6.0f - 0.1f * decrease));
		CHECK(spread_now(still, c, 6.0f, now - kTimeUnitsPerSecond / 5, immediate) == doctest::Approx(6.0f - 0.2f * decrease));
		CHECK(spread_now(still, c, 6.0f, now - kTimeUnitsPerSecond, immediate) == 2.5f);
		CHECK(spread_now(walking, c, 6.0f, now - kTimeUnitsPerSecond, immediate) == 3.0f);

		CHECK(spread_now(walking, c, 2.5f, now - kTimeUnitsPerSecond / 60, rise) == doctest::Approx(2.5f + decrease / 60.0f));
		CHECK(spread_now(walking, c, 2.5f, now - kTimeUnitsPerSecond, rise) == 3.0f);

		SwayContext quiet = weapon_sway_context(motor, still, c);
		CHECK_FALSE(suppressed_sway(quiet, 0.29f, immediate).suppressed);
		CHECK(suppressed_sway(quiet, 0.3f, immediate).suppressed);
		quiet.suppressed = true;
		CHECK(suppressed_sway(quiet, 0.0f, immediate).suppressed);

		Driver drv(t, { id_of(*t, kM16A3) });
		drv.ctx.sway = SwayContext{};
		drv.idle_until_ready();
		const WeaponSimDef &live = t->defs[id_of(*t, kM16A3)];
		const Dispersion live_disp = effective_dispersion(live, drv.ctx.sway);
		size_t checked = 0;
		for (int i = 0; i < 120; ++i) {
			const WeaponSlotState before = drv.st.current();
			const size_t had = drv.shots.size();
			drv.step(fire(i % 20 < 14, false));
			for (size_t k = had; k < drv.shots.size(); ++k) {
				const ShotEvent &e = drv.shots[k];
				const float want = k == had ? spread_at(before.spread, before.spread_time, e.time, live_disp, t->mechanics)
											: spread_at(spread_after_shot(drv.shots[k - 1].spread, live_disp), drv.shots[k - 1].time, e.time, live_disp, t->mechanics);
				CHECK(std::memcmp(&want, &e.spread, sizeof(float)) == 0);
				++checked;
			}
		}
		CHECK(checked > 15);
	}

	TEST_CASE("sim.weapons.unit: W7 determinism: replays, a mid-burst restore, seeds, two worlds") {
		const std::shared_ptr<const WeaponTable> t = synthetic_table();
		const uint16_t burst = id_of(*t, kM16A4), a3 = id_of(*t, kM16A3);

		auto script = [](uint32_t p_i) {
			WeaponInput in;
			in.zoom = (p_i / 50) % 2 == 0;
			in.fire = (p_i % 17) < 3;
			in.reload = p_i == 300;
			in.fire_mode_counter = uint8_t((p_i / 200) & 3);
			in.select_slot = p_i == 450 ? 1 : (p_i == 520 ? 0 : 0xFF);
			return in;
		};
		auto run = [&](uint64_t p_seed, std::vector<uint64_t> &r_hashes, std::vector<SoldierWeaponsState> *r_states) {
			Driver d(t, { burst, a3 });
			d.seed = p_seed;
			for (uint32_t i = 0; i < 600; ++i) {
				d.step(script(i));
				r_hashes.push_back(soldier_weapons_hash(d.st));
				if (r_states != nullptr) {
					r_states->push_back(d.st);
				}
			}
			return d.shots;
		};
		std::vector<uint64_t> h1, h2, h3;
		std::vector<SoldierWeaponsState> states;
		const std::vector<ShotEvent> s1 = run(1, h1, &states);
		const std::vector<ShotEvent> s2 = run(1, h2, nullptr);
		CHECK(h1 == h2);
		CHECK(shot_list_hash(s1) == shot_list_hash(s2));
		REQUIRE(s1.size() >= 40);

		size_t restore = 0;
		for (size_t i = 100; i < states.size(); ++i) {
			if (states[i].current().burst_left > 0) {
				restore = i;
				break;
			}
		}
		REQUIRE(restore > 0);
		{
			Driver d(t, { burst, a3 });
			d.st = states[restore];
			bool same = true;
			for (uint32_t i = uint32_t(restore) + 1; i < 600; ++i) {
				d.step(script(i));
				same = same && soldier_weapons_hash(d.st) == h1[i];
			}
			CHECK(same);
			std::vector<ShotEvent> tail;
			for (const ShotEvent &s : s1) {
				if (s.time >= tick_end(Tick(restore + 1))) {
					tail.push_back(s);
				}
			}
			CHECK(shot_list_hash(d.shots) == shot_list_hash(tail));
			MESSAGE("restored at tick " << restore + 1 << " (burst_left " << int(states[restore].current().burst_left) << "), " << d.shots.size() << " shots replayed");
		}

		{
			const std::shared_ptr<const WeaponTable> o = synthetic_table(true);
			auto hip = [&](uint64_t p_seed) {
				Driver d(o, { id_of(*o, kM16A3) });
				d.seed = p_seed;
				d.idle_until_ready();
				for (int i = 0; i < 600; ++i) {
					d.step(fire(true, false));
				}
				return d.shots;
			};
			const std::vector<ShotEvent> a = hip(1), b = hip(2);
			REQUIRE(a.size() == b.size());
			size_t changed = 0;
			bool times = true;
			for (size_t i = 0; i < a.size(); ++i) {
				changed += (a[i].direction != b[i].direction) ? 1 : 0;
				times = times && a[i].time == b[i].time;
			}
			const double share = double(changed) / double(a.size());
			MESSAGE("seed 1 vs 2: " << changed << " of " << a.size() << " hip-fire directions differ");
			CHECK(share >= 0.99);
			CHECK(times);
		}

		{
			const float walls[3] = { 30.0f, 120.0f, 400.0f };
			const std::vector<StaticTriangle> tris = range_wall_triangles(walls, 1.6f);
			core::Result<std::shared_ptr<const StaticWorldShapes>> shapes = build_static_world(std::span<const StaticTriangle>(tris));
			REQUIRE(shapes.ok());
			core::Result<std::unique_ptr<CollisionWorld>> w1 = CollisionWorld::create(*shapes);
			core::Result<std::unique_ptr<CollisionWorld>> w2 = CollisionWorld::create(*shapes);
			REQUIRE(w1.ok());
			REQUIRE(w2.ok());
			std::vector<HitTarget> targets;
			targets.push_back(capsule_target(7, { 0.2f, 1.0f, 25.0f }, { 0.2f, 1.6f, 25.0f }, 0.2f, HitZone::Chest));
			const StaticTargets provider(targets);
			auto shoot = [&](const CollisionWorld &p_world) {
				ProjectileWorld pw(t);
				for (const ShotEvent &s : s1) {
					ShotEvent e = s;
					e.origin = { 0, 1.6f, 0 };
					REQUIRE(pw.spawn(e).ok());
				}
				return fly(pw, p_world, provider, 1, 900);
			};
			const Flown f1 = shoot(**w1), f2 = shoot(**w2);
			CHECK(impact_list_hash(f1.impacts) == impact_list_hash(f2.impacts));
			CHECK(f1.damage.size() == f2.damage.size());
			CHECK(f1.impacts.size() >= s1.size());
		}
	}

	TEST_CASE("sim.weapons.unit: capsule geometry: hits, grazes, starts inside, ties") {
		ZoneCapsule c;
		c.a = { 0, 0, 10 };
		c.b = { 0, 1, 10 };
		c.radius = 0.25f;
		float f = 0;
		Vec3 n{};
		REQUIRE(segment_capsule({ 0, 0.5f, 0 }, { 0, 0.5f, 20 }, c, f, n));
		CHECK(f == doctest::Approx((10.0 - 0.25) / 20.0));
		CHECK(n[2] == doctest::Approx(-1.0f));

		REQUIRE(segment_capsule({ 0, 3, 10 }, { 0, -3, 10 }, c, f, n));
		CHECK(f == doctest::Approx((3.0 - 1.25) / 6.0));
		CHECK(n[1] == doctest::Approx(1.0f));

		CHECK_FALSE(segment_capsule({ 0.2501f, 0.5f, 0 }, { 0.2501f, 0.5f, 20 }, c, f, n));
		CHECK(segment_capsule({ 0.2499f, 0.5f, 0 }, { 0.2499f, 0.5f, 20 }, c, f, n));

		CHECK_FALSE(segment_capsule({ 0, 0.5f, 0 }, { 0, 0.5f, 9.7f }, c, f, n));
		CHECK_FALSE(segment_capsule({ 0, 0.5f, 9 }, { 0, 0.5f, 0 }, c, f, n));

		REQUIRE(segment_capsule({ 0, 0.5f, 10 }, { 0, 0.5f, 20 }, c, f, n));
		CHECK(f == 0.0f);
		CHECK(n[2] == doctest::Approx(-1.0f));

		ZoneCapsule s = c;
		s.b = s.a;
		REQUIRE(segment_capsule({ 0, 0, 0 }, { 0, 0, 20 }, s, f, n));
		CHECK(f == doctest::Approx(9.75 / 20.0));

		HitTarget t = capsule_target(3, { 0, 0, 10 }, { 0, 1, 10 }, 0.25f, HitZone::Body);
		t.capsule_count = 3;
		t.capsules[1] = t.capsules[0];
		t.capsules[1].zone = HitZone::Chest;
		t.capsules[1].capsule = 1;
		t.capsules[2] = t.capsules[0];
		t.capsules[2].a = { 0, 0, 9 };
		t.capsules[2].b = { 0, 1, 9 };
		t.capsules[2].zone = HitZone::Head;
		t.capsules[2].capsule = 2;
		t.bound_center = { 0, 0.5f, 9.5f };
		t.bound_radius = 2.0f;
		TargetHit hit;
		REQUIRE(segment_target({ 0, 0.5f, 0 }, { 0, 0.5f, 20 }, t, hit));
		CHECK(hit.zone == HitZone::Head);
		CHECK(hit.capsule == 2);
		CHECK(hit.target == 3);
		t.capsules[2].a = { 0, 5, 9 };
		t.capsules[2].b = { 0, 6, 9 };
		REQUIRE(segment_target({ 0, 0.5f, 0 }, { 0, 0.5f, 20 }, t, hit));
		CHECK(hit.zone == HitZone::Body);
		CHECK(hit.capsule == 0);
		CHECK_FALSE(segment_target({ 3, 0.5f, 0 }, { 3, 0.5f, 20 }, t, hit));
		Vec3 p{};
		CHECK(zone_aim_point(t, HitZone::Chest, p));
		CHECK(p[1] == doctest::Approx(0.5f));
		CHECK_FALSE(zone_aim_point(t, HitZone::Legs, p));
	}

	TEST_CASE("sim.weapons.unit: damage arithmetic, health, blasts, shots to kill") {
		const std::shared_ptr<const WeaponTable> t = synthetic_table();
		const WeaponSimDef &a3 = t->defs[id_of(*t, kM16A3)];
		const Mechanics m;
		CHECK(falloff_damage(a3.projectile, 0) == doctest::Approx(25.0));
		CHECK(falloff_damage(a3.projectile, 8) == doctest::Approx(25.0));
		CHECK(falloff_damage(a3.projectile, 29) == doctest::Approx(25.0 - 6.6 * 21.0 / 42.0));
		CHECK(falloff_damage(a3.projectile, 50) == doctest::Approx(18.4));
		CHECK(falloff_damage(a3.projectile, 5000) == doctest::Approx(18.4));
		CHECK(zone_damage(a3.projectile, HitZone::Head, 0, m) == doctest::Approx(50.0));
		Mechanics doubled;
		doubled.bullet_damage_modifier = 2.0f;
		CHECK(zone_damage(a3.projectile, HitZone::Chest, 0, doubled) == doctest::Approx(50.0));

		ExplosionSimDef x;
		x.present = true;
		x.blast_damage = 112;
		x.inner_radius = 1.5f;
		x.radius = 7;
		CHECK(blast_damage(x, 0, BlastFalloff::Linear) == 112.0);
		CHECK(blast_damage(x, 4.25, BlastFalloff::Linear) == doctest::Approx(56.0));
		CHECK(blast_damage(x, 7.0, BlastFalloff::Linear) == 0.0);
		CHECK(blast_damage(x, 70.0, BlastFalloff::Linear) == 0.0);
		x.radius = 1.0f;
		CHECK(blast_damage(x, 0.9, BlastFalloff::Linear) == 112.0);
		CHECK(blast_damage(x, 1.0, BlastFalloff::Linear) == 112.0);
		CHECK(blast_damage(x, 1.4, BlastFalloff::Linear) == 0.0);

		SoldierState s;
		s.flags = soldier_flags::kAlive;
		s.health = 100.0f;
		CHECK(apply_damage(s, 25.0f, m));
		CHECK(s.health == 75.0f);
		CHECK((s.events & soldier_events::kDamaged) != 0);
		CHECK_FALSE(apply_damage(s, 0.0f, m));
		CHECK_FALSE(apply_damage(s, std::numeric_limits<float>::quiet_NaN(), m));
		CHECK(apply_damage(s, 75.0f, m));
		CHECK(s.mode == MoveMode::Dead);
		CHECK((s.events & soldier_events::kDied) != 0);
		CHECK_FALSE(apply_damage(s, 10.0f, m));
		SoldierState p;
		p.flags = uint16_t(soldier_flags::kAlive | soldier_flags::kSpawnProtected);
		CHECK_FALSE(apply_damage(p, 10.0f, m));
		Mechanics open;
		open.spawn_protection_blocks = false;
		CHECK(apply_damage(p, 10.0f, open));

		const std::array<int32_t, 4> s870 = shots_to_kill(t->defs[id_of(*t, k870)], 5, 100, m);
		CHECK(s870[size_t(HitZone::Chest)] == 1);
		const std::array<int32_t, 4> nade = shots_to_kill(t->defs[id_of(*t, kM67)], 5, 100, m);
		CHECK(nade == std::array<int32_t, 4>{ 0, 0, 0, 0 });
		CHECK(one_shot_range(a3, HitZone::Head, 100, m) == -1.0);
		CHECK(std::isinf(one_shot_range(t->defs[id_of(*t, kM98B)], HitZone::Head, 100, m)));
	}

	TEST_CASE("sim.weapons.unit: W5 W9 flights on synthetic walls: bullets on capsules, bounce, fuse, blasts, occlusion, direct hits") {
		const std::shared_ptr<const WeaponTable> t = synthetic_table();
		const Mechanics &m = t->mechanics;
		const uint16_t a3 = id_of(*t, kM16A3), m67 = id_of(*t, kM67), rpg = id_of(*t, kRpg), knife = id_of(*t, kKnife);

		{
			const std::unique_ptr<CollisionWorld> w = wall_world({ 200.0f });
			const StaticTargets targets({ capsule_target(9, { 0, 1.3f, 29.0f }, { 0, 1.9f, 29.0f }, 0.2f, HitZone::Head) });
			ProjectileWorld pw(t);
			REQUIRE(pw.spawn(level_shot(a3, { 0, 1.6f, 0 })).ok());
			const Flown f = fly(pw, *w, targets, 1, 60);
			REQUIRE(f.damage.size() == 1);
			const DamageEvent &d = f.damage[0];
			CHECK(d.kind == DamageKind::Bullet);
			CHECK(d.target == 9);
			CHECK(d.zone == HitZone::Head);
			CHECK(d.distance == doctest::Approx(28.8).epsilon(1e-3));
			CHECK(d.amount == doctest::Approx(zone_damage(t->defs[a3].projectile, HitZone::Head, double(d.distance), m)).epsilon(1e-5));
			REQUIRE_FALSE(f.impacts.empty());
			CHECK(f.impacts[0].kind == ImpactKind::Target);
			CHECK(pw.live().empty());
		}

		{
			const std::unique_ptr<CollisionWorld> w = wall_world({ 200.0f });
			const StaticTargets near({ capsule_target(1, { 0, 1.3f, 1.2f }, { 0, 1.9f, 1.2f }, 0.2f, HitZone::Chest) });
			const StaticTargets far({ capsule_target(1, { 0, 1.3f, 2.5f }, { 0, 1.9f, 2.5f }, 0.2f, HitZone::Chest) });
			ProjectileWorld a(t), b(t);
			REQUIRE(a.spawn(level_shot(knife, { 0, 1.6f, 0 }, 0, 18'000'000)).ok());
			REQUIRE(b.spawn(level_shot(knife, { 0, 1.6f, 0 }, 0, 18'000'000)).ok());
			const Flown fa = fly(a, *w, near, 1, 30), fb = fly(b, *w, far, 1, 30);
			REQUIRE(fa.damage.size() == 1);
			CHECK(fa.damage[0].amount == doctest::Approx(50.0f));
			CHECK(fa.damage[0].time == 18'000'000);
			CHECK(fb.damage.empty());
		}

		{
			const std::unique_ptr<CollisionWorld> w = wall_world({ 5.0f });
			const StaticTargets none({});
			ProjectileWorld pw(t);
			const ShotEvent s = level_shot(m67, { 0, 1.6f, 0 }, 0, t->defs[m67].spawn_delay);
			REQUIRE(pw.spawn(s).ok());
			const Flown f = fly(pw, *w, none, 1, 300);
			const ImpactEvent *bounce = nullptr;
			for (const ImpactEvent &e : f.impacts) {
				if (e.kind == ImpactKind::Bounce && bounce == nullptr) {
					bounce = &e;
				}
			}
			REQUIRE(bounce != nullptr);
			CHECK(bounce->position[2] == doctest::Approx(5.0f).epsilon(1e-3));
			REQUIRE(f.detonations.size() == 1);
			CHECK(f.detonations[0].timeout);
			CHECK(f.detonations[0].time - s.spawn_time == 216'000'000);
			CHECK(f.detonations[0].position[2] < 5.0f);
			MESSAGE("M67 off a 5 m wall: detonated at (" << f.detonations[0].position[0] << ", " << f.detonations[0].position[1] << ", " << f.detonations[0].position[2]
														 << "), " << f.impacts.size() << " impacts");
		}

		{
			const std::unique_ptr<CollisionWorld> w = wall_world({ 60.0f });
			const StaticTargets targets({ capsule_target(1, { 2.0f, 1.3f, 58.5f }, { 2.0f, 1.9f, 58.5f }, 0.2f, HitZone::Chest),
					capsule_target(2, { 0.5f, 1.3f, 61.0f }, { 0.5f, 1.9f, 61.0f }, 0.2f, HitZone::Chest) });
			ProjectileWorld pw(t);
			REQUIRE(pw.spawn(level_shot(rpg, { 0, 1.6f, 0 })).ok());
			const Flown f = fly(pw, *w, targets, 1, 300);
			REQUIRE(f.detonations.size() == 1);
			CHECK_FALSE(f.detonations[0].dud);
			const ExplosionSimDef &x = t->defs[rpg].projectile.explosion;
			bool front = false, behind = false;
			for (const DamageEvent &d : f.damage) {
				CHECK(d.kind == DamageKind::Blast);
				if (d.target == 1) {
					front = true;
					CHECK(d.amount == doctest::Approx(blast_damage(x, double(d.distance), m.blast_falloff)).epsilon(1e-4));
					CHECK(d.distance > 1.0f);
					CHECK(d.distance < 3.0f);
				}
				behind = behind || d.target == 2;
			}
			CHECK(front);
			CHECK_FALSE(behind);

			const StaticTargets direct({ capsule_target(4, { 0, 1.3f, 20.0f }, { 0, 1.9f, 20.0f }, 0.25f, HitZone::Chest) });
			ProjectileWorld pd(t);
			REQUIRE(pd.spawn(level_shot(rpg, { 0, 1.6f, 0 })).ok());
			const Flown fd = fly(pd, *w, direct, 1, 300);
			const DamageEvent *hit = nullptr;
			for (const DamageEvent &d : fd.damage) {
				if (d.kind == DamageKind::Direct) {
					hit = &d;
				}
			}
			REQUIRE(hit != nullptr);
			CHECK(hit->amount == doctest::Approx(225.0f * 2.5f));
			REQUIRE(fd.detonations.size() == 1);
		}
	}

	TEST_CASE("sim.weapons.unit: W8 600 random ticks per def: the state invariants and the projectile bounds") {
		const std::shared_ptr<const WeaponTable> t = synthetic_table();
		const std::unique_ptr<CollisionWorld> w = wall_world({ 40.0f, 300.0f });
		const StaticTargets provider(random_run_targets());
		RandomRun all;
		for (uint16_t def = 0; def < t->defs.size(); ++def) {
			CAPTURE(t->defs[def].variant);
			const RandomRun r = random_run(t, def, uint16_t((def + 1) % t->defs.size()), 20261015u + def, *w, provider);
			CHECK(r.refused == 0);
			CHECK(r.dropped == 0);
			CHECK_MESSAGE(r.violations == 0, r.first_violation);
			std::printf("  %-36s %4zu shots, events %05x\n", t->defs[def].variant.c_str(), r.shots, unsigned(r.events));
			all.add(r);
		}
		std::printf("W8 random ticks: %zu defs x 600 ticks, %zu shots, worst %u shots / %u timed events per tick, peak %zu live, %zu budget-exhausted steps, "
					"%zu violations\n",
				t->defs.size(), all.shots, all.worst_shots, all.worst_timed, all.peak_live, all.exhausted, all.violations);
		CHECK(all.violations == 0);
		CHECK(all.worst_shots <= kMaxShotsPerTick);
		CHECK(all.worst_timed <= kMaxTimedEventsPerTick);
		CHECK(all.shots >= 200);
	}
}

TEST_SUITE("weapons-unit") {
	TEST_CASE("sim.weapons.unit: M2b ru the defibrillator: capacity -1 builds, 3 shots in 3 s, no reload, 0 damage") {
		Synthetic s;
		s.def = weapon("weapons/gadgets/defibrillator/u_defib", core::gameplay::FireLogic::Automatic, 60);
		s.def.deploy_time = 0.4f;
		core::gameplay::FireFunction &f = s.def.fire;
		f.magazine_capacity = -1;
		f.number_of_magazines = 1;
		f.reload_type = core::gameplay::ReloadType::Magazine;
		f.reload_time = 3.6f;
		f.reload_time_bullets_left = -1.0f;
		f.always_auto_reload = false;
		f.trace_frequency = 0;
		f.initial_speed = { 0, 0, 1.75f };
		for (core::gameplay::Dispersion &d : f.weapon_dispersion) {
			d = { 0, 0, 0, 5 };
		}
		core::gameplay::ProjectileDef &p = s.def.projectile;
		p.kind = core::gameplay::ProjectileKind::Bullet;
		p.gravity = 0;
		p.time_to_live = 1.0f;
		p.start_damage = 0;
		p.end_damage = 0;
		p.damage_falloff_start = 5;
		p.damage_falloff_end = 5;
		p.instant_hit = true;
		auto materials = std::make_shared<const SurfaceMaterialTable>();
		const WeaponSimDef d = sim_def(s, *materials);
		CHECK(d.unlimited_ammo);
		CHECK(d.magazine_capacity == 1);
		CHECK(d.projectile.instant_hit);
		CHECK(d.projectile.start_damage == 0.0f);
		const std::shared_ptr<const WeaponTable> t = table_of({ s });
		Driver dr(t, { 0 });
		dr.idle_until_ready();
		const int32_t magazine = dr.st.current().magazine;
		for (int i = 0; i < 180; ++i) {
			dr.step(fire(true, false));
		}
		std::printf("M2b ru defib: %zu shots in 180 ticks, magazine %d -> %d, events %05x\n", dr.shots.size(), magazine, dr.st.current().magazine,
				unsigned(dr.all_events));
		CHECK(dr.shots.size() == 3);
		CHECK(dr.st.current().magazine == magazine);
		CHECK((dr.all_events & (weapon_events::kReloadStarted | weapon_events::kReloadFinished)) == 0);
		for (const ShotEvent &e : dr.shots) {
			CHECK(e.def == 0);
		}
		if (dr.shots.size() >= 2) {
			CHECK(dr.shots[1].time - dr.shots[0].time == seconds_to_units(1.0));
		}
	}
}
