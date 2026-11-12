#include "test_support.h"
#include "weapons_support.h"

#include "bf3/sim/soldier_motor.h"
#include "bf3/sim/soldier_state.h"
#include "bf3/sim/vehicles/vehicle_damage.h"
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

constexpr double kSwayDeg = 3.14159265358979323846 / 180.0;

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
		m.apply_protection_threshold = true;
		CHECK(synthetic_table(false, m)->hash != t->hash);
		auto changed = std::make_shared<WeaponTable>(*t);
		changed->defs[0].gameplay_deviation_scale[1] += 0.5f;
		CHECK(weapon_table_hash(*changed) != t->hash);
		changed = std::make_shared<WeaponTable>(*t);
		changed->defs[0].deviation_scale[0] += 0.5f;
		CHECK(weapon_table_hash(*changed) != t->hash);
		changed = std::make_shared<WeaponTable>(*t);
		changed->defs[0].camera_spring_constant += 1.0f;
		CHECK(weapon_table_hash(*changed) != t->hash);
		changed = std::make_shared<WeaponTable>(*t);
		changed->defs[0].modes[0].interval_seconds = std::nextafter(changed->defs[0].modes[0].interval_seconds, 1.0f);
		CHECK(weapon_table_hash(*changed) != t->hash);
		changed = std::make_shared<WeaponTable>(*t);
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
		supp.suppression = 1.0f;
		const Dispersion sd = effective_dispersion(*lmg, supp);
		CHECK(sd.min_angle == doctest::Approx(lmg->sway[0][0].move[0].min_angle - 2.0f));
		CHECK(sd.decrease_per_second == doctest::Approx(lmg->sway[0][0].move[0].decrease_per_second - 10.0f));
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
			fast.def.fire.rate_of_fire = 27'000;
			const std::shared_ptr<const WeaponTable> t = table_of({ fast }, true);
			Driver d(t, { 0 });
			d.idle_until_ready();
			uint8_t most = 0;
			for (int i = 0; i < 60; ++i) {
				d.step(fire(true));
				most = std::max(most, d.out.shot_count);
			}
			CHECK(most == kMaxShotsPerTick);
			CHECK(d.refused == 0);
			REQUIRE(d.shots.size() > 400);
			float carry = sway_carry_at(d.shots[0].time);
			for (size_t i = 1; i < d.shots.size(); ++i) {
				const SwayWait w = sway_wait(d.shots[i - 1].time, carry, t->defs[0].modes[0].interval_seconds);
				REQUIRE(d.shots[i].time == w.time);
				REQUIRE(std::llabs(d.shots[i].time - d.shots[i - 1].time - 133'333) <= 1);
				carry = w.carry;
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

	TEST_CASE("sim.weapons.unit: Q2 Q3 Q6 R-A0 the sway generator, the polar draw, the kick, the recovery, the signs") {
		const uint32_t states[] = { 449972507u, 767167897u, 805624826u, 950006068u, 2121102460u, 1898997609u,
			1173878147u, 607081849u, 589913191u, 23435746u, 603991418u, 1950715051u };
		const double units[] = { 0.209535, 0.357240, 0.375148, 0.442381, 0.987715, 0.884290, 0.546630, 0.282695,
			0.274700, 0.010913, 0.281255, 0.908373 };
		uint32_t g = 5489u;
		for (size_t i = 0; i < std::size(states); ++i) {
			g = sway_rng_next(g);
			CHECK(g == states[i]);
			CHECK(double(sway_rng_unit(g)) == doctest::Approx(units[i]).epsilon(5e-6));
		}
		CHECK(sway_rng_seeded(0) == 1u);
		CHECK(sway_rng_seeded(0x80000005u) == 5u);
		CHECK(sway_rng_next(0) == sway_rng_next(1));

		const std::shared_ptr<const WeaponTable> t = synthetic_table();
		const WeaponSimDef &m16 = t->defs[id_of(*t, kM16A3)];
		SwayContext ads;
		ads.zoom = Zoom::Zoom;
		struct Draw {
			uint32_t seed;
			double pitch;
			double yaw;
			double kick_yaw;
			double kick_up;
		};
		const Draw draws[] = { { 12345u, 1.736661, 0.811906, -0.301420, 0.646805 },
			{ 1u, -1.736272, 0.487140, -0.338388, 0.601027 }, { 7u, -0.041543, -0.115986, 0.031283, 0.632191 } };
		for (const Draw &d : draws) {
			GunSwayState s;
			s.dispersion = 2.5f;
			sway_fire_start(s);
			sway_shot(s, d.seed);
			CHECK(double(s.deviation_pitch) / kSwayDeg == doctest::Approx(d.pitch).epsilon(3e-6));
			CHECK(double(s.deviation_yaw) / kSwayDeg == doctest::Approx(d.yaw).epsilon(3e-6));
			const GunSwayState again = [&] {
				GunSwayState c = s;
				sway_shot(c, d.seed);
				return c;
			}();
			CHECK(again.deviation_pitch == s.deviation_pitch);
			CHECK(again.rng == s.rng);
			CHECK(again.shots == 2);
			sway_update(s, m16, ads);
			CHECK(double(s.recoil_yaw) / kSwayDeg == doctest::Approx(d.kick_yaw).epsilon(3e-6));
			CHECK(-double(s.recoil_pitch) / kSwayDeg == doctest::Approx(d.kick_up).epsilon(3e-6));
			CHECK(s.dispersion == doctest::Approx(2.6f));
		}

		GunSwayState one;
		one.dispersion = m16.sway[0][1].move[0].min_angle;
		sway_fire_start(one);
		sway_shot(one, 12345u);
		sway_shot(one, 12345u);
		sway_update(one, m16, ads);
		CHECK(one.dispersion == doctest::Approx(0.3f));
		CHECK(-double(one.recoil_pitch) / kSwayDeg == doctest::Approx(0.258722).epsilon(3e-6));
		sway_fire_stop(one);
		int updates = 0;
		while (one.recoil_pitch != 0.0f && updates < 30) {
			sway_update(one, m16, ads);
			++updates;
		}
		CHECK(updates == 4);
		CHECK(one.recoil_yaw == 0.0f);

		GunSwayState kick;
		sway_fire_start(kick);
		sway_shot(kick, 12345u);
		sway_update(kick, m16, ads);
		sway_shot(kick, 12346u);
		sway_update(kick, m16, ads);
		const double second_up = -double(kick.recoil_pitch) / kSwayDeg - 0.646805;
		CHECK(second_up >= 0.9 * 0.26 - 1e-5);
		CHECK(second_up <= 0.26 + 1e-5);
		GunSwayState clamp;
		clamp.recoil_pitch = float(-9.9 * kSwayDeg);
		sway_shot(clamp, 99u);
		sway_update(clamp, m16, ads);
		CHECK(-double(clamp.recoil_pitch) / kSwayDeg == doctest::Approx(10.0).epsilon(1e-6));

		GunSwayState held = kick;
		const float before = held.recoil_pitch;
		sway_update(held, m16, ads);
		GunSwayState released = kick;
		sway_fire_stop(released);
		sway_update(released, m16, ads);
		const double t1 = double(kSwaySeconds);
		CHECK(double(held.recoil_pitch - before) == doctest::Approx(18.0 * t1 * t1 * 0.01 * 0.1).epsilon(1e-2));
		CHECK(double(released.recoil_pitch - before) == doctest::Approx(18.0 * t1 * t1 * 0.01).epsilon(1e-2));

		GunSwayState env;
		env.dispersion = 6.95f;
		sway_fire_start(env);
		Dispersion top{ 2.5f, 7.0f, 0.1f, 15.0f };
		WeaponSimDef wide = m16;
		wide.sway[0][1].move[0] = top;
		sway_shot(env, 5u);
		sway_update(env, wide, ads);
		CHECK(env.dispersion == doctest::Approx(7.05f));
		sway_shot(env, 6u);
		sway_update(env, wide, ads);
		CHECK(env.dispersion == doctest::Approx(7.05f));
		sway_fire_stop(env);
		for (int i = 0; i < 9; ++i) {
			sway_update(env, wide, ads);
		}
		CHECK(env.dispersion == doctest::Approx(2.55f));
		sway_update(env, wide, ads);
		CHECK(env.dispersion == 2.5f);
		GunSwayState low;
		low.dispersion = 1.0f;
		sway_fire_start(low);
		sway_update(low, wide, ads);
		CHECK(low.dispersion == 1.0f);

		WeaponSimDef blend = m16;
		blend.sway[0][0].move[size_t(SwayMove::Base)] = { 2.5f, 7.0f, 0.1f, 15.0f };
		blend.sway[0][0].move[size_t(SwayMove::Moving)] = { 3.0f, 8.0f, 0.2f, 12.0f };
		blend.sway[0][0].has_move[size_t(SwayMove::Moving)] = true;
		SwayContext walk;
		walk.move = SwayMove::Moving;
		walk.move_fraction = 0.5f;
		const Dispersion half = effective_dispersion(blend, walk);
		CHECK(half.min_angle == doctest::Approx(2.75f));
		CHECK(half.max_angle == 8.0f);
		CHECK(half.increase_per_shot == 0.2f);
		CHECK(half.decrease_per_second == 12.0f);
		SwayContext sprint;
		sprint.move = SwayMove::Sprinting;
		sprint.stance = Stance::Crouch;
		sprint.zoom = Zoom::Zoom;
		REQUIRE(sway_record(m16, sprint, kSwayAirHold));
		CHECK_FALSE(sway_record(m16, sprint, kSwayAirHold)->modifiers);
		CHECK(sway_record(m16, sprint, kSwayAirHold)->dispersion.min_angle ==
				m16.sway[0][0].move[size_t(SwayMove::Sprinting)].min_angle);
		SwayContext crouch;
		crouch.stance = Stance::Crouch;
		CHECK_FALSE(sway_record(m16, crouch, 0.2f));
		CHECK(sway_record(m16, SwayContext{}, 0.2f)->dispersion.min_angle ==
				m16.sway[0][0].move[size_t(SwayMove::Jumping)].min_angle);
		SwayContext supp;
		supp.suppression = 1.0f;
		CHECK(effective_dispersion(m16, supp).min_angle == doctest::Approx(m16.sway[0][0].move[0].min_angle + 1.5f));
		CHECK(effective_dispersion(m16, supp).max_angle == m16.sway[0][0].move[0].max_angle);
		CHECK(effective_dispersion(m16, supp).increase_per_shot ==
				doctest::Approx(m16.sway[0][0].move[0].increase_per_shot + 0.1f));
		supp.suppression = 0.5f;
		CHECK(effective_dispersion(m16, supp).min_angle == doctest::Approx(m16.sway[0][0].move[0].min_angle + 0.75f));

		const Vec3 right = sway_rotate({ 0, 0, 1 }, 0.0f, float(kSwayDeg));
		CHECK(double(right[0]) == doctest::Approx(0.017452).epsilon(1e-4));
		CHECK(std::fabs(right[1]) < 1e-7f);
		CHECK(double(right[2]) == doctest::Approx(0.999848).epsilon(1e-6));
		const Vec3 up = sway_rotate({ 0, 0, 1 }, -float(kSwayDeg), 0.0f);
		CHECK(std::fabs(up[0]) < 1e-7f);
		CHECK(double(up[1]) == doctest::Approx(0.017452).epsilon(1e-4));

		const auto ticks = [](float p_rpm, int p_shots) {
			std::vector<uint32_t> out;
			SimTime at = 0;
			float carry = kSwaySeconds;
			for (int i = 0; i < p_shots; ++i) {
				out.push_back(sway_tick(at));
				const SwayWait w = sway_wait(at, carry, 60.0f / p_rpm);
				at = w.time;
				carry = w.carry;
			}
			out.push_back(sway_tick(at));
			return out;
		};
		CHECK(ticks(800.0f, 10) == std::vector<uint32_t>{ 0, 2, 4, 6, 8, 11, 13, 15, 17, 20, 22 });
		CHECK(ticks(600.0f, 2) == std::vector<uint32_t>{ 0, 2, 5 });
		CHECK(ticks(900.0f, 3) == std::vector<uint32_t>{ 0, 2, 4, 6 });
		CHECK(ticks(750.0f, 10) == std::vector<uint32_t>{ 0, 2, 4, 7, 9, 11, 14, 16, 19, 21, 23 });
		CHECK(ticks(1000.0f, 10) == std::vector<uint32_t>{ 0, 1, 3, 5, 7, 8, 10, 12, 14, 16, 17 });
		CHECK(sway_wait(0, kSwaySeconds, 0.0f).time == 0);
		CHECK(sway_carry_at(sway_tick_start(7)) == kSwaySeconds);
	}

	TEST_CASE("sim.weapons.unit: T1 / R-A0 TV4 the M16A4 10-round ADS burst at 30 Hz, shot ticks, seeds, draws, kicks "
			  "and the recovery") {
		const std::shared_ptr<const WeaponTable> t = synthetic_table();
		const uint16_t id = id_of(*t, kM16A3);
		const WeaponSimDef &def = t->defs[id];
		constexpr uint32_t kPress = 12345;
		const Tick press_tick = Tick(2 * kPress + 1);
		REQUIRE(sway_tick(tick_start(press_tick)) == kPress);
		SoldierWeaponsState st;
		const uint16_t loadout[] = { id };
		REQUIRE(spawn_soldier_weapons(st, *t, loadout, 0, press_tick - 600).ok());
		WeaponContext ctx;
		ctx.eye_start = { 0, 1.6f, 0 };
		ctx.eye_end = ctx.eye_start;
		ctx.sway.zoom = Zoom::Zoom;
		struct Row {
			uint32_t tick;
			double d;
			double yaw;
			double pitch;
			double cam_pitch;
			double cam_yaw;
		};
		const Row rows[] = { { 0, 0.2, +0.064952, +0.138933, -0.646805, -0.301420 },
			{ 2, 0.3, +0.006883, -0.062467, -0.891203, -0.478471 },
			{ 4, 0.4, -0.159277, +0.206580, -1.148423, -0.532298 },
			{ 6, 0.5, +0.044238, -0.017001, -1.392464, -0.462902 },
			{ 8, 0.6, -0.317243, -0.057779, -1.649328, -0.770281 },
			{ 11, 0.7, -0.413354, +0.264971, -1.894840, -0.888242 },
			{ 13, 0.8, +0.115222, -0.002647, -2.131758, -0.887562 },
			{ 15, 0.9, -0.457170, -0.264250, -2.381498, -1.263658 },
			{ 17, 1.0, +0.014071, +0.025779, -2.618059, -1.516531 },
			{ 20, 1.1, +0.201819, +0.067098, -2.869270, -1.579984 } };
		struct After {
			uint32_t tick;
			double d;
			double cam_pitch;
			double cam_yaw;
		};
		const After after[] = { { 22, 1.2, -2.868124, -1.578838 }, { 23, 0.7, -2.822288, -1.533002 },
			{ 24, 0.2, -2.719155, -1.429869 }, { 27, 0.2, -1.836800, -0.547514 }, { 28, 0.2, -1.275302, 0.0 },
			{ 29, 0.2, -0.541916, 0.0 }, { 30, 0.2, 0.0, 0.0 } };
		const auto rotate = [](const std::array<double, 3> &p_v, double p_pitch, double p_yaw) {
			const double x = p_v[0] * std::cos(p_yaw) + p_v[2] * std::sin(p_yaw);
			const double z = -p_v[0] * std::sin(p_yaw) + p_v[2] * std::cos(p_yaw);
			return std::array<double, 3>{ x, p_v[1] * std::cos(p_pitch) - z * std::sin(p_pitch),
				p_v[1] * std::sin(p_pitch) + z * std::cos(p_pitch) };
		};
		std::vector<ShotEvent> shots;
		std::vector<GunSwayState> at_shot;
		std::vector<GunSwayState> kicked(31);
		std::vector<bool> seen(31, false);
		WeaponTickOutput out;
		for (Tick tick = st.tick + 1; tick <= press_tick + 70; ++tick) {
			Command c;
			c.tick = tick;
			WeaponInput in;
			in.zoom = true;
			in.fire = tick >= press_tick && shots.size() < 10;
			c.buttons = with_weapon_input(0, in);
			step_soldier_weapons(*t, st, c, ctx, 1, out);
			if (tick < press_tick) {
				CHECK(out.shot_count == 0);
				continue;
			}
			const uint32_t k = sway_tick(tick_start(tick)) - kPress;
			if (starts_sway_tick(tick) && k <= 30) {
				kicked[k] = st.current().sway;
				seen[k] = true;
			}
			for (uint8_t i = 0; i < out.shot_count; ++i) {
				shots.push_back(out.shots[i]);
				at_shot.push_back(st.current().sway);
			}
		}
		REQUIRE(shots.size() == 10);
		CHECK(st.current().magazine == def.magazine_capacity - 10);
		for (size_t i = 0; i < shots.size(); ++i) {
			const Row &r = rows[i];
			const ShotEvent &e = shots[i];
			CAPTURE(i);
			CHECK(sway_tick(e.time) == kPress + r.tick);
			CHECK(double(e.spread) == doctest::Approx(r.d).epsilon(1e-6));
			CHECK(std::fabs(double(at_shot[i].deviation_yaw) / kSwayDeg - r.yaw) < 2e-6);
			CHECK(std::fabs(double(at_shot[i].deviation_pitch) / kSwayDeg - r.pitch) < 2e-6);
			const std::array<double, 3> want = rotate(rotate({ 0, 0, 1 }, r.pitch * kSwayDeg, r.yaw * kSwayDeg),
					double(at_shot[i].recoil_pitch), double(at_shot[i].recoil_yaw));
			for (size_t a = 0; a < 3; ++a) {
				CHECK(std::fabs(double(e.direction[a]) - want[a]) < 2e-6);
			}
			const RecoilOffset kick = camera_recoil(def, kicked[r.tick + 1], Zoom::Zoom);
			CHECK(seen[r.tick + 1]);
			CHECK(std::fabs(double(kick.pitch) + r.cam_pitch) < 2e-6);
			CHECK(std::fabs(double(kick.yaw) - r.cam_yaw) < 2e-6);
			CHECK(kicked[r.tick + 1].shots == (i == 0 ? 1 : 2));
		}
		for (const After &a : after) {
			CAPTURE(a.tick);
			REQUIRE(seen[a.tick]);
			const RecoilOffset cam = camera_recoil(def, kicked[a.tick], Zoom::Zoom);
			CHECK(double(kicked[a.tick].dispersion) == doctest::Approx(a.d).epsilon(1e-6));
			CHECK(std::fabs(double(cam.pitch) + a.cam_pitch) < 2e-6);
			CHECK(std::fabs(double(cam.yaw) - a.cam_yaw) < 2e-6);
		}
		CHECK((kicked[23].flags & sway_flags::kTriggerHeld) == 0);
	}

	TEST_CASE("sim.weapons.unit: Q5 the camera spring runs only unzoomed and follows the recovered accumulator; R4-A2 "
			  "bullets carry the gameplay-scaled recoil") {
		const std::shared_ptr<const WeaponTable> t = synthetic_table();
		WeaponSimDef d = t->defs[id_of(*t, kM16A3)];
		d.camera_spring_constant = 250.0f;
		d.camera_spring_damping = 0.35f;
		d.camera_spring_min_threshold_angle = 1.0f;
		d.deviation_scale = { 0.5f, 1.0f };
		d.gameplay_deviation_scale = { 0.25f, 1.0f };
		SwayContext hip;
		GunSwayState s;
		s.recoil_pitch = float(-1.1 * kSwayDeg);
		s.since_shot = 100.0f;
		sway_update(s, d, hip);
		CHECK(s.recoil_pitch == 0.0f);
		CHECK(s.spring_pitch == 0.0f);
		GunSwayState big;
		big.recoil_pitch = float(-2.5 * kSwayDeg);
		big.recoil_yaw = float(0.5 * kSwayDeg);
		sway_fire_start(big);
		const double dt = double(kSwaySeconds);
		sway_update(big, d, hip);
		const double v = double(big.recoil_pitch);
		const double w = 250.0 * v * dt * (1.0 - 0.35);
		CHECK(double(big.spring_pitch_rate) == doctest::Approx(w).epsilon(1e-6));
		CHECK(double(big.spring_pitch) == doctest::Approx(w * dt).epsilon(1e-6));
		CHECK(big.spring_yaw == 0.0f);
		const RecoilOffset cam = camera_recoil(d, big, Zoom::NoZoom);
		CHECK(double(cam.pitch) == doctest::Approx(-w * dt * 0.5 / kSwayDeg).epsilon(1e-5));
		const RecoilOffset gameplay = gameplay_recoil(d, big, Zoom::NoZoom);
		CHECK(double(gameplay.pitch) == doctest::Approx(-v * 0.25 / kSwayDeg).epsilon(1e-6));
		SwayContext zoomed;
		zoomed.zoom = Zoom::Zoom;
		GunSwayState z = big;
		sway_update(z, d, zoomed);
		CHECK(z.spring_pitch == big.spring_pitch);
		CHECK(z.spring_pitch_rate == big.spring_pitch_rate);
		CHECK(camera_recoil(d, z, Zoom::Zoom).pitch == doctest::Approx(-double(z.recoil_pitch) / kSwayDeg));
		const Vec3 bullet = shoot_direction(0, 0, d, big, Zoom::NoZoom, false);
		CHECK(double(bullet[1]) == doctest::Approx(std::sin(-v * 0.25)).epsilon(1e-4));
		const Vec3 turned = shoot_direction(uint16_t(kYawSteps / 4), 0, d, GunSwayState{}, Zoom::NoZoom, false);
		CHECK(double(turned[0]) == doctest::Approx(1.0).epsilon(1e-6));
	}

	TEST_CASE("sim.weapons.unit: Q2 B3 the sway record from the soldier: sprint, air, moving above 0.5 m/s, stance and "
			  "zoom") {
		MotorParams motor;
		for (PoseParams &pp : motor.poses) {
			pp.on_ground.velocity = 2.0f;
			pp.on_ground.backward = 2.0f;
			pp.on_ground.left = 2.0f;
			pp.on_ground.right = 2.0f;
			pp.throttle.points[0] = { 0.0f, 1.0f };
			pp.throttle.count = 1;
		}
		SoldierState still;
		still.flags = soldier_flags::kAlive;
		Command c;
		c.tick = 600;
		CHECK(weapon_sway_context(motor, still, c).move == SwayMove::Base);
		SoldierState creeping = still;
		creeping.velocity = { 0.0f, 0.0f, 0.5f };
		CHECK(weapon_sway_context(motor, creeping, c).move == SwayMove::Base);
		SoldierState walking = still;
		walking.velocity = { 0.0f, 0.0f, 1.0f };
		CHECK(weapon_sway_context(motor, walking, c).move == SwayMove::Moving);
		CHECK(weapon_sway_context(motor, walking, c).move_fraction == 0.5f);
		SoldierState airborne = still;
		airborne.mode = MoveMode::Falling;
		CHECK(weapon_sway_context(motor, airborne, c).move == SwayMove::Jumping);
		SoldierState sprint = walking;
		sprint.flags = uint16_t(soldier_flags::kAlive | soldier_flags::kSprinting);
		CHECK(weapon_sway_context(motor, sprint, c).move == SwayMove::Sprinting);
		SoldierState prone = still;
		prone.target_pose = Pose::Prone;
		CHECK(weapon_sway_context(motor, prone, c).stance == Stance::Prone);
		Command zoom = c;
		zoom.buttons = buttons::kZoom;
		CHECK(weapon_sway_context(motor, still, zoom).zoom == Zoom::Zoom);

		CHECK(make_weapon_context(motor, still, still, c).sway.suppression == 0.0f);
		CHECK(make_weapon_context(motor, still, still, c, 0.641f).sway.suppression == 0.641f);
		CHECK(make_weapon_context(motor, still, still, c, 3.0f).sway.suppression == 1.0f);
		CHECK(make_weapon_context(motor, still, still, c, std::nanf("")).sway.suppression == 0.0f);

		const std::shared_ptr<const WeaponTable> t = synthetic_table();
		WeaponSimDef d = t->defs[id_of(*t, kM16A3)];
		d.sway[0][0].move[size_t(SwayMove::Jumping)].min_angle = 7.0f;
		d.sway[0][0].move[size_t(SwayMove::Jumping)].max_angle = 8.0f;
		GunSwayState s;
		SwayContext air;
		air.move = SwayMove::Jumping;
		sway_update(s, d, air);
		CHECK(s.since_air == 0.0f);
		CHECK(s.dispersion == d.sway[0][0].move[size_t(SwayMove::Jumping)].min_angle);
		SwayContext ground;
		for (int i = 0; i < 14; ++i) {
			sway_update(s, d, ground);
		}
		CHECK(s.since_air < kSwayAirHold);
		CHECK(s.dispersion == d.sway[0][0].move[size_t(SwayMove::Jumping)].min_angle);
		sway_update(s, d, ground);
		CHECK(s.since_air == kSwayAirHold);
		CHECK(s.dispersion < d.sway[0][0].move[size_t(SwayMove::Jumping)].min_angle);
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
			MESSAGE("seed 1 vs 2: "
					<< changed << " of " << a.size()
					<< " hip-fire directions differ (R-A0: the sway is seeded with the 30 Hz tick number)");
			CHECK(share == 0.0);
			std::set<std::array<float, 3>> distinct;
			for (const ShotEvent &e : a) {
				distinct.insert({ e.direction[0], e.direction[1], e.direction[2] });
			}
			CHECK(distinct.size() == a.size());
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
		CHECK(blast_damage(x, 1.4, BlastFalloff::Linear) == 112.0);
		CHECK(blast_damage(x, 1.6, BlastFalloff::Linear) == 0.0);

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

	TEST_CASE("sim.weapons.unit: R-G4 explosion damage: full inside InnerBlastRadius, linear to BlastRadius, "
			  "ShockwaveDamage as a floor out to the gather radius, the material relation's threshold as a cutoff") {
		ExplosionSimDef x;
		x.present = true;
		x.blast_damage = 100;
		x.inner_radius = 1;
		x.radius = 5;
		x.shockwave_damage = 10;
		x.shockwave_radius = 8;
		CHECK(x.gather_radius() == 8.0f);
		const double want[][2] = { { 0.5, 100 }, { 1, 100 }, { 3, 50 }, { 4.5, 12.5 }, { 4.8, 10 }, { 7, 10 },
			{ 8.5, 0 } };
		for (const auto &[d, v] : want) {
			CAPTURE(d);
			CHECK(blast_damage(x, d, BlastFalloff::Linear) == doctest::Approx(v));
		}
		CHECK(vehicles::material_relation_damage(float(blast_damage(x, 3, BlastFalloff::Linear)), 0.5f, 5.0f) ==
				doctest::Approx(25.0f));
		CHECK(vehicles::material_relation_damage(float(blast_damage(x, 4.8, BlastFalloff::Linear)), 0.5f, 5.0f) ==
				doctest::Approx(5.0f));
		CHECK(vehicles::material_relation_damage(float(blast_damage(x, 4.8, BlastFalloff::Linear)), 0.4f, 5.0f) ==
				0.0f);
		ExplosionSimDef none = x;
		none.radius = 0;
		none.shockwave_radius = 0;
		CHECK(blast_damage(none, 0.5, BlastFalloff::Linear) == 0.0);
		ExplosionSimDef only = x;
		only.radius = 0;
		CHECK(blast_damage(only, 0.5, BlastFalloff::Linear) == doctest::Approx(100.0));
		CHECK(blast_damage(only, 2, BlastFalloff::Linear) == doctest::Approx(10.0));

		const std::shared_ptr<const WeaponTable> t = synthetic_table();
		const uint16_t rpg = id_of(*t, kRpg);
		std::shared_ptr<WeaponTable> floored = std::make_shared<WeaponTable>(*t);
		ExplosionSimDef &fx = floored->defs[rpg].projectile.explosion;
		fx.shockwave_damage = 1.0f;
		fx.shockwave_radius = 7.0f;
		fx.zone_threshold[size_t(HitZone::Chest)] = 3.0f;
		const std::unique_ptr<CollisionWorld> w = wall_world({ 60.0f });
		const StaticTargets targets(
				{ capsule_target(4, { 5.5f, 1.3f, 58.0f }, { 5.5f, 1.9f, 58.0f }, 0.2f, HitZone::Body),
						capsule_target(5, { -5.5f, 1.3f, 58.0f }, { -5.5f, 1.9f, 58.0f }, 0.2f, HitZone::Chest),
						capsule_target(6, { 0.0f, 1.3f, 50.0f }, { 0.0f, 1.9f, 50.0f }, 0.2f, HitZone::Body) });
		ProjectileWorld pw(floored);
		REQUIRE(pw.spawn(level_shot(rpg, { 0, 1.6f, 0 })).ok());
		const Flown f = fly(pw, *w, targets, 1, 300);
		REQUIRE(f.detonations.size() == 1);
		size_t floors = 0;
		for (const DamageEvent &d : f.damage) {
			CAPTURE(d.target);
			CHECK(d.kind == DamageKind::Blast);
			CHECK(d.target != 6u);
			CHECK(d.distance > fx.radius);
			CHECK(d.target != 5u);
			CHECK(d.base == doctest::Approx(1.0f));
			CHECK(d.amount == doctest::Approx(fx.zone_multiplier[size_t(HitZone::Body)]));
			++floors;
		}
		CHECK(floors == 1);
	}

	TEST_CASE("sim.weapons.unit: R2-G4 missile update per tick: thrust to MaxSpeed, gravity only with "
			  "ApplyGravityWhenGuided while powered, Drag per update after burnout, ignition and burnout times, "
			  "TimeToArm picks the Explosion or the DudExplosion") {
		const std::shared_ptr<const WeaponTable> base = synthetic_table();
		const uint16_t rpg = id_of(*base, kRpg);
		const std::unique_ptr<CollisionWorld> open = wall_world({ 4000.0f });
		const StaticTargets none({});
		const auto missile = [&](const std::function<void(ProjectileSimDef &)> &p_set) {
			std::shared_ptr<WeaponTable> t = std::make_shared<WeaponTable>(*base);
			ProjectileSimDef &p = t->defs[rpg].projectile;
			p.launch_offset = {};
			p.gravity = 0;
			p.gravity_when_guided = false;
			p.engine_ignition = 0;
			p.engine_time_to_live = 40 * kTimeUnitsPerSecond;
			p.engine_acceleration = 0;
			p.drag = 0;
			p_set(p);
			return std::shared_ptr<const WeaponTable>(t);
		};
		const auto speed_after = [&](const std::shared_ptr<const WeaponTable> &p_t, Tick p_ticks,
										 Vec3 *r_velocity = nullptr) {
			ProjectileWorld pw(p_t);
			REQUIRE(pw.spawn(level_shot(rpg, { 0, 1.6f, 0 })).ok());
			ProjectileStepOutput out;
			for (Tick k = 1; k <= 2u * p_ticks; ++k) {
				out.clear();
				pw.step(k, *open, none, out);
			}
			REQUIRE(pw.live().size() == 1);
			const DVec3 v = pw.live()[0].velocity;
			if (r_velocity != nullptr) {
				*r_velocity = { float(v[0]), float(v[1]), float(v[2]) };
			}
			return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
		};
		const auto thrust = [&](float p_v0) {
			return missile([&](ProjectileSimDef &p) {
				p.launch_velocity = { 0, 0, p_v0 };
				p.engine_acceleration = 100;
				p.max_speed = 200;
			});
		};
		CHECK(speed_after(thrust(50), 1) == doctest::Approx(50.0 + 100.0 / 30.0));
		CHECK(speed_after(thrust(199), 1) == doctest::Approx(200.0));
		Vec3 v{};
		const std::shared_ptr<const WeaponTable> burnt = missile([](ProjectileSimDef &p) {
			p.launch_velocity = { 0, 0, 100 };
			p.max_speed = 200;
			p.engine_time_to_live = 0;
			p.drag = 0.5f;
			p.gravity = -9.81f;
		});
		speed_after(burnt, 1, &v);
		CHECK(v[2] == doctest::Approx(99.5f));
		CHECK(v[1] == doctest::Approx(-9.81f / 30.0f));
		const auto powered = [&](bool p_flag) {
			return missile([&](ProjectileSimDef &p) {
				p.launch_velocity = { 0, 0, 40 };
				p.max_speed = 100;
				p.gravity = -1.5f;
				p.gravity_when_guided = p_flag;
			});
		};
		speed_after(powered(false), 3, &v);
		CHECK(v[1] == 0.0f);
		speed_after(powered(true), 3, &v);
		CHECK(v[1] == doctest::Approx(-1.5f * 3.0f / 30.0f));

		const std::shared_ptr<const WeaponTable> timed = missile([](ProjectileSimDef &p) {
			p.launch_velocity = { 0, 0, 10 };
			p.engine_acceleration = 30;
			p.max_speed = 1000;
			p.engine_ignition = seconds_to_units(0.1);
			p.engine_time_to_live = seconds_to_units(2.0);
		});
		CHECK(speed_after(timed, 2) == doctest::Approx(10.0));
		CHECK(speed_after(timed, 3) == doctest::Approx(11.0));
		CHECK(speed_after(timed, 63) == doctest::Approx(10.0 + 61.0));
		CHECK(speed_after(timed, 64) == doctest::Approx(10.0 + 61.0));

		const auto armed_at = [&](float p_wall) {
			const std::shared_ptr<const WeaponTable> t = missile([](ProjectileSimDef &p) {
				p.launch_velocity = { 0, 0, 100 };
				p.max_speed = 100;
				p.time_to_arm = seconds_to_units(0.25);
			});
			const std::unique_ptr<CollisionWorld> w = wall_world({ p_wall });
			ProjectileWorld pw(t);
			REQUIRE(pw.spawn(level_shot(rpg, { 0, 1.6f, 0 })).ok());
			const Flown f = fly(pw, *w, none, 1, 30);
			REQUIRE(f.detonations.size() == 1);
			CHECK(f.detonations[0].position[2] == doctest::Approx(p_wall).epsilon(1e-3));
			return !f.detonations[0].dud;
		};
		CHECK_FALSE(armed_at(19.5f));
		CHECK(armed_at(30.5f));
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

	TEST_CASE("sim.weapons.unit: a bullet first stepped nine ticks late hits where one stepped every tick hits") {
		const std::shared_ptr<const WeaponTable> t = synthetic_table();
		const std::unique_ptr<CollisionWorld> w = wall_world({ 60.0f });
		const StaticTargets none({});
		const ShotEvent s = level_shot(id_of(*t, kM16A3), { 0, 1.6f, 0 }, 300'000);
		ProjectileWorld on_time(t), late(t);
		REQUIRE(on_time.spawn(s).ok());
		REQUIRE(late.spawn(s).ok());
		const Flown a = fly(on_time, *w, none, 1, 60), b = fly(late, *w, none, 10, 60);
		REQUIRE(a.impacts.size() == 1);
		REQUIRE(b.impacts.size() == a.impacts.size());
		MESSAGE("every tick: y " << a.impacts[0].position[1] << " at " << a.impacts[0].time << "; late: y "
								 << b.impacts[0].position[1] << " at " << b.impacts[0].time);
		CHECK(b.impacts[0].kind == a.impacts[0].kind);
		CHECK(b.impacts[0].time == a.impacts[0].time);
		CHECK(b.impacts[0].position == a.impacts[0].position);
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
		for (int i = 0; i < 170; ++i) {
			dr.step(fire(true, false));
		}
		std::printf("M2b ru defib: %zu shots in 170 ticks, magazine %d -> %d, events %05x\n", dr.shots.size(), magazine,
				dr.st.current().magazine, unsigned(dr.all_events));
		CHECK(dr.shots.size() == 3);
		CHECK(dr.st.current().magazine == magazine);
		CHECK((dr.all_events & (weapon_events::kReloadStarted | weapon_events::kReloadFinished)) == 0);
		for (const ShotEvent &e : dr.shots) {
			CHECK(e.def == 0);
		}
		if (dr.shots.size() >= 2) {
			CHECK(dr.shots[1].time ==
					sway_wait(dr.shots[0].time, sway_carry_at(dr.shots[0].time),
							dr.table->defs[0].modes[0].interval_seconds)
							.time);
			CHECK(std::llabs(dr.shots[1].time - dr.shots[0].time - seconds_to_units(1.0)) < kTimeUnitsPerTick);
		}
	}
}
