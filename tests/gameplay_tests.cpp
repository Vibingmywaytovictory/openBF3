// Gameplay tables (bf3_core/gameplay): SoldierDef, KitDef, ModeDef, VehicleDef against the retail
// install (plan §7.2-§7.4, research/topics/02-soldier-kits-modes.md), plus install-free unit cases.
// The install is read through InstallReader / Install / EbxLibrary only (read-only); without one the
// "gameplay" suite skips (exit 77).
#include <doctest/doctest.h>

#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/kit_def.h"
#include "bf3/core/gameplay/logic_world.h"
#include "bf3/core/gameplay/mode_def.h"
#include "bf3/core/gameplay/soldier_def.h"
#include "bf3/core/gameplay/vehicle_def.h"
#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"
#include "test_support.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace bf3::core;
using namespace bf3::core::gameplay;
using doctest::Approx;

namespace {

struct Mounted {
	InstallReader reader;
	std::shared_ptr<Install> install;
};

bool open_or_skip(Mounted &r_m) {
	if (!r_m.reader.locate({})) {
		bf3::test::skip_install_case(r_m.reader.describe().c_str());
		return false;
	}
	Result<std::shared_ptr<Install>> install = Install::open(r_m.reader);
	REQUIRE_MESSAGE(install.ok(), (install.ok() ? std::string() : install.error().message));
	r_m.install = *install;
	return true;
}

// The level's own superbundle (self-contained: gameplay/*, characters/*, settings, scoring).
std::unique_ptr<EbxLibrary> open_level(const Mounted &p_m, const char *p_level_id) {
	const std::optional<Level> level = p_m.install->level(p_level_id);
	REQUIRE_MESSAGE(level.has_value(), p_level_id);
	Result<std::unique_ptr<EbxLibrary>> lib = EbxLibrary::open(p_m.install, { level->superbundle });
	REQUIRE_MESSAGE(lib.ok(), (lib.ok() ? std::string() : lib.error().message));
	return std::move(*lib);
}

ModeDef mode(const EbxResolver &p_db, const char *p_level, const char *p_mode) {
	Result<ModeDef> m = read_mode_def(p_db, p_level, p_mode);
	REQUIRE_MESSAGE(m.ok(), (m.ok() ? std::string() : m.error().message));
	return std::move(*m);
}

VehicleDef vehicle(const EbxResolver &p_db, const char *p_blueprint) {
	Result<VehicleDef> v = read_vehicle_def(p_db, p_blueprint);
	REQUIRE_MESSAGE(v.ok(), (v.ok() ? std::string() : v.error().message));
	return std::move(*v);
}

size_t hq_count(const ModeDef &p_mode) {
	size_t n = 0;
	for (const CapturePointDef &c : p_mode.capture_points) {
		n += c.is_hq() ? 1 : 0;
	}
	return n;
}

// The first turret of a seat that turns about an axis (1 yaw, 0 pitch) at the given nesting.
const VehicleTurretDef *turret(const VehicleDef &p_v, int p_seat, int p_axis, bool p_root) {
	for (const VehicleTurretDef &t : p_v.turrets) {
		if (t.seat == p_seat && t.rotation_axis == p_axis && (t.parent < 0) == p_root) {
			return &t;
		}
	}
	return nullptr;
}

void check_mbt_health(const VehicleDef &p_v) {
	CHECK(p_v.body_mass == Approx(5000));
	CHECK(p_v.gravity_modifier == Approx(1.6));
	CHECK(p_v.max_zone_health() == Approx(1000));
	CHECK(p_v.front.min_damage_angle == Approx(81));
	CHECK(p_v.front.damage_angle_multiplier == Approx(1));
	CHECK(p_v.rear.min_damage_angle == Approx(36));
	CHECK(p_v.rear.damage_angle_multiplier == Approx(2.75));
	CHECK(p_v.left.min_damage_angle == Approx(45));
	CHECK(p_v.left.damage_angle_multiplier == Approx(1.8));
	CHECK(p_v.right.min_damage_angle == Approx(45));
	CHECK(p_v.right.damage_angle_multiplier == Approx(1.8));
	CHECK(p_v.rear.max_shield_health == Approx(175));
	CHECK(p_v.left.max_shield_health == Approx(175));
	CHECK(p_v.disabled_damage_threshold == Approx(500));
	CHECK(p_v.emergency_repair_health == Approx(510));
	CHECK(p_v.regeneration_rate == Approx(30));
	CHECK(p_v.regeneration_delay == Approx(12));
	REQUIRE(p_v.seats.size() == 2);
	CHECK(p_v.seats[0].seat_type == "EST_Driver");
	CHECK(p_v.seats[1].seat_type == "EST_Gunner");
}

} // namespace

TEST_SUITE("gameplay_unit") {
	TEST_CASE("gameplay: interface constants parse by their type word") {
		std::optional<PropertyValue> v = PropertyValue::parse("Int32 250");
		REQUIRE(v.has_value());
		CHECK(v->kind == PropertyValue::Kind::Int);
		CHECK(v->as_i64() == 250);
		v = PropertyValue::parse("  Float32 1.5 ");
		REQUIRE(v.has_value());
		CHECK(v->kind == PropertyValue::Kind::Float);
		CHECK(*v->as_f64() == Approx(1.5));
		v = PropertyValue::parse("Boolean True");
		REQUIRE(v.has_value());
		CHECK(v->as_bool() == true);
		v = PropertyValue::parse("TeamId Team2");
		REQUIRE(v.has_value());
		CHECK(v->as_team() == 2);
		v = PropertyValue::parse("CString \"ID_LOC_A\"");
		REQUIRE(v.has_value());
		CHECK(v->as_str() == std::optional<std::string_view>("ID_LOC_A"));
		v = PropertyValue::parse("Vec3 1 2 3");
		REQUIRE(v.has_value());
		REQUIRE(v->as_vec3().has_value());
		CHECK((*v->as_vec3())[2] == Approx(3));
		// Malformed or unknown: kept as an untyped constant, never a number.
		v = PropertyValue::parse("Int32 12abc");
		REQUIRE(v.has_value());
		CHECK(v->kind == PropertyValue::Kind::Constant);
		CHECK(!v->as_i64().has_value());
		v = PropertyValue::parse("Vec3 1 2");
		REQUIRE(v.has_value());
		CHECK(v->kind == PropertyValue::Kind::Constant);
		CHECK(!PropertyValue::parse("   ").has_value());
	}

	TEST_CASE("gameplay: piecewise-linear curves clamp outside their range") {
		const std::vector<CurvePoint> fall = { { 0, 0 }, { 13.9f, 0 }, { 14, 20 }, { 22, 40 }, { 30, 100 } };
		CHECK(eval_curve({}, 5) == 0);
		CHECK(eval_curve(fall, -3) == Approx(0));
		CHECK(eval_curve(fall, 10) == Approx(0));
		CHECK(eval_curve(fall, 18) == Approx(30));
		CHECK(eval_curve(fall, 26) == Approx(70));
		CHECK(eval_curve(fall, 99) == Approx(100));
	}

	TEST_CASE("gameplay: mode aliases") {
		CHECK(canonical_mode_id("CQS") == "ConquestSmall0");
		CHECK(canonical_mode_id("cql") == "ConquestLarge0");
		CHECK(canonical_mode_id("RL") == "RushLarge0");
		CHECK(canonical_mode_id("SQDM") == "SquadDeathMatch0");
		CHECK(canonical_mode_id("TDM") == "TeamDeathMatch0");
		CHECK(canonical_mode_id("Domination0") == "Domination0");
		CHECK(soldier_pose_from_name("CharacterPoseType_Prone") == SoldierPose::Prone);
		CHECK(!soldier_pose_from_name("CharacterPoseType_Lie").has_value());
	}
}

TEST_SUITE("gameplay") {
	TEST_CASE("gameplay: SoldierDef (plan §7.2)") {
		Mounted m;
		if (!open_or_skip(m)) {
			return;
		}
		const std::unique_ptr<EbxLibrary> lib = open_level(m, "MP_001");
		Result<SoldierDef> read = read_soldier_def(*lib);
		REQUIRE_MESSAGE(read.ok(), (read.ok() ? std::string() : read.error().message));
		const SoldierDef &s = *read;
		CHECK(s.max_health == Approx(100));
		CHECK(s.low_health_threshold == Approx(30));
		CHECK(s.regeneration_delay == Approx(10));
		CHECK(s.regeneration_rate == Approx(5));
		CHECK(s.immortal_time_after_spawn == Approx(2));
		CHECK(s.man_down_time == Approx(13));
		CHECK(s.post_revive_health == Approx(20));
		CHECK(s.time_for_corpse == Approx(15));
		CHECK(s.mass == Approx(100));
		CHECK(s.radius == Approx(0.35));
		CHECK(s.max_ascend_angle == Approx(50));
		CHECK(s.water_depth_limit == Approx(1.4));

		const PoseDef *stand = s.pose(SoldierPose::Stand);
		const PoseDef *crouch = s.pose(SoldierPose::Crouch);
		const PoseDef *prone = s.pose(SoldierPose::Prone);
		REQUIRE(stand != nullptr);
		REQUIRE(crouch != nullptr);
		REQUIRE(prone != nullptr);
		CHECK(stand->height == Approx(1.8));
		CHECK(crouch->height == Approx(1.15));
		CHECK(prone->height == Approx(0.3));
		CHECK(stand->eye_height == Approx(1.6));
		CHECK(crouch->eye_height == Approx(1.0));
		CHECK(prone->eye_height == Approx(0.3));
		CHECK(stand->step_height == Approx(0.3));
		CHECK(stand->transition_to[1].value_or(-1) == Approx(0.3));
		CHECK(stand->transition_to[2].value_or(-1) == Approx(0.7));
		CHECK(crouch->transition_to[0].value_or(-1) == Approx(0.3));
		CHECK(crouch->transition_to[2].value_or(-1) == Approx(0.5));
		CHECK(prone->transition_to[0].value_or(-1) == Approx(0.7));
		CHECK(prone->transition_to[1].value_or(-1) == Approx(1.05));

		CHECK(s.ground_speed(SoldierPose::Stand).value_or(-1) == Approx(4));
		CHECK(s.ground_speed(SoldierPose::Crouch).value_or(-1) == Approx(2));
		CHECK(s.ground_speed(SoldierPose::Prone).value_or(-1) == Approx(0.9));
		CHECK(s.sprint_speed().value_or(-1) == Approx(6.5));
		CHECK(s.jump_height().value_or(-1) == Approx(0.6));
		const MovementStateDef *ground = s.state("OnGroundStateData");
		REQUIRE(ground != nullptr);
		REQUIRE(ground->pose(SoldierPose::Stand) != nullptr);
		CHECK(ground->pose(SoldierPose::Stand)->backward == Approx(0.75));
		CHECK(ground->pose(SoldierPose::Stand)->left == Approx(0.75));
		const MovementStateDef *swim = s.state("SwimmingStateData");
		const MovementStateDef *climb = s.state("ClimbingStateData");
		const MovementStateDef *chute = s.state("ParachuteStateData");
		REQUIRE(swim != nullptr);
		REQUIRE(climb != nullptr);
		REQUIRE(chute != nullptr);
		REQUIRE(swim->pose(SoldierPose::Stand) != nullptr);
		REQUIRE(climb->pose(SoldierPose::Stand) != nullptr);
		REQUIRE(chute->pose(SoldierPose::Stand) != nullptr);
		CHECK(swim->pose(SoldierPose::Stand)->velocity == Approx(3));
		CHECK(climb->pose(SoldierPose::Stand)->velocity == Approx(1.32));
		CHECK(chute->pose(SoldierPose::Stand)->velocity == Approx(9));
		CHECK(chute->scalar("TerminalVelocity").value_or(-1) == Approx(5));

		CHECK(s.fall_damage(13.9f) == Approx(0));
		CHECK(s.fall_damage(14) == Approx(20));
		CHECK(s.fall_damage(22) == Approx(40));
		CHECK(s.fall_damage(30) == Approx(100));
		CHECK(s.fall_damage(40) == Approx(100));

		struct Spec {
			const char *id;
			double value;
			const char *field;
		};
		const Spec specs[] = {
			{ "SprintBoostL1", 1.1, "SprintMultiplier" },
			{ "AmmoBoostL1", 1.5, "AmmoClipIncreaseMultiplier" },
			{ "ExplosiveBoostL1", 2.0, "ExplosiveIncreaseMultiplier" },
			{ "ExplosiveResistL1", 0.85, "ExplosionDamageModifier" },
			{ "SuppressionResistL1", 0.5, "SuppressionResistModifier" },
			{ "SuppressionBoostL1", 1.5, "SuppressionBoostModifier" },
			{ "GrenadeBoostL1", 1.0, "GrenadeIncrease" },
			{ "UnderslungBoostL1", 3.0, "UnderslungGrenadeIncrease" },
			{ "HealSpeedBoostL1", 0.8, "RegenerationDelayModifier" },
		};
		for (const Spec &spec : specs) {
			CAPTURE(spec.id);
			const SpecializationDef *d = s.specialization(spec.id);
			REQUIRE(d != nullptr);
			CHECK(d->value == Approx(spec.value));
			CHECK(d->target_field == spec.field);
			CHECK(!d->squad);
		}
		const SpecializationDef *squad = s.specialization("SprintBoostL2");
		REQUIRE(squad != nullptr);
		CHECK(squad->squad);
		CHECK(squad->value == Approx(1.1));
		CHECK(s.specializations.size() == 18);
	}

	TEST_CASE("gameplay: ModeDef MP_001 and scoring (plan §7.3)") {
		Mounted m;
		if (!open_or_skip(m)) {
			return;
		}
		const std::unique_ptr<EbxLibrary> lib = open_level(m, "MP_001");
		const std::vector<std::string> modes = level_modes(*lib, "MP_001");
		CHECK(modes.size() == 7);

		const ModeDef cqs = mode(*lib, "MP_001", "CQS");
		CHECK(cqs.mode_id == "ConquestSmall0");
		for (const int team : { 1, 2 }) {
			CAPTURE(team);
			REQUIRE(cqs.tickets(team) != nullptr);
			CHECK(cqs.tickets(team)->initial_tickets == 250);
		}
		CHECK(cqs.flag_area_value().value_or(-1) == Approx(51));
		CHECK(cqs.flag_count() == 3);
		CHECK(hq_count(cqs) == 2);
		REQUIRE(cqs.size.has_value());
		CHECK(cqs.size->player_count == 32);
		CHECK(cqs.size->squad_size() == 4);
		REQUIRE(cqs.teams.size() >= 2);
		CHECK(cqs.teams.front().team <= cqs.teams.back().team);
		REQUIRE(cqs.team(1) != nullptr);
		CHECK(cqs.team(1)->faction == "FactionUS");
		CHECK(cqs.team(1)->kits.size() == 4);

		const ModeDef cql = mode(*lib, "MP_001", "CQL");
		REQUIRE(cql.tickets(1) != nullptr);
		CHECK(cql.tickets(1)->initial_tickets == 400);
		CHECK(cql.flag_area_value().value_or(-1) == Approx(34));
		CHECK(cql.flag_count() == 5);
		REQUIRE(cql.size.has_value());
		CHECK(cql.size->player_count == 64);

		const ModeDef rush = mode(*lib, "MP_001", "RL");
		const LifeCounterDef *attackers = nullptr;
		const LifeCounterDef *defenders = nullptr;
		for (const LifeCounterDef &l : rush.life_counters) {
			(l.attacker ? attackers : defenders) = &l;
		}
		REQUIRE(attackers != nullptr);
		REQUIRE(defenders != nullptr);
		CHECK(attackers->starting_lives == 75);
		CHECK(defenders->base_count == 4);
		// 10 crates are placed (a 5th base included); which ones run is decided by logic at runtime.
		REQUIRE(rush.mcoms.size() >= 8);
		for (const McomDef &c : rush.mcoms) {
			CHECK(c.arm_time.value_or(-1) == Approx(2.5));
			CHECK(c.disarm_time.value_or(-1) == Approx(5));
			CHECK(c.radius.value_or(-1) == Approx(1.5));
			CHECK(c.fuse.value_or(-1) == Approx(25));
		}
		REQUIRE(rush.size.has_value());
		CHECK(rush.size->player_count == 32);
		CHECK(rush.size->rounds_per_map == 2);

		const ModeDef squad_rush = mode(*lib, "MP_001", "SR");
		bool sr_attackers = false;
		for (const LifeCounterDef &l : squad_rush.life_counters) {
			if (l.attacker) {
				sr_attackers = true;
				CHECK(l.starting_lives == 20);
			}
		}
		CHECK(sr_attackers);
		REQUIRE(!squad_rush.mcoms.empty());
		CHECK(squad_rush.mcoms.front().arm_time.value_or(-1) == Approx(5));
		REQUIRE(squad_rush.size.has_value());
		CHECK(squad_rush.size->player_count == 8);

		const ModeDef tdm = mode(*lib, "MP_001", "TDM");
		for (const int team : { 1, 2 }) {
			CAPTURE(team);
			REQUIRE(tdm.kills(team) != nullptr);
			CHECK(tdm.kills(team)->max_kills == 100);
		}

		const ModeDef sqdm = mode(*lib, "MP_001", "SQDM");
		for (const int team : { 1, 2, 3, 4 }) {
			CAPTURE(team);
			REQUIRE(sqdm.kills(team) != nullptr);
			CHECK(sqdm.kills(team)->max_kills == 50);
		}
		REQUIRE(sqdm.size.has_value());
		CHECK(sqdm.size->player_count == 16);

		Result<ScoringTable> scoring = read_scoring(*lib);
		REQUIRE_MESSAGE(scoring.ok(), (scoring.ok() ? std::string() : scoring.error().message));
		CHECK(scoring->score("ID_SCORE_KILL").value_or(-1) == Approx(100));
		CHECK(scoring->score("ID_SCORE_CAPTURE_FLAG").value_or(-1) == Approx(250));
		CHECK(scoring->score("ID_SCORE_NEUTRALIZE_FLAG").value_or(-1) == Approx(200));
		CHECK(scoring->score("ID_SCORE_REVIVE").value_or(-1) == Approx(100));
		CHECK(scoring->score("ID_SCORE_HEADSHOT").value_or(-1) == Approx(10));
		CHECK(!scoring->score("ID_SCORE_NO_SUCH").has_value());
	}

	TEST_CASE("gameplay: ModeDef Metro, Caspian Border and Palace Domination (plan §7.3)") {
		Mounted m;
		if (!open_or_skip(m)) {
			return;
		}
		{
			const std::unique_ptr<EbxLibrary> lib = open_level(m, "MP_Subway");
			const ModeDef cqs = mode(*lib, "MP_Subway", "CQS");
			REQUIRE(cqs.tickets(1) != nullptr);
			CHECK(cqs.tickets(1)->initial_tickets == 350);
			CHECK(cqs.flag_area_value().value_or(-1) == Approx(55));
			CHECK(cqs.flag_count() == 3);
			const ModeDef rush = mode(*lib, "MP_Subway", "RL");
			bool attackers = false;
			for (const LifeCounterDef &l : rush.life_counters) {
				if (l.attacker) {
					attackers = true;
					CHECK(l.starting_lives == 100);
				}
			}
			CHECK(attackers);
			REQUIRE(!rush.mcoms.empty());
			CHECK(rush.mcoms.front().fuse.value_or(-1) == Approx(25));
			const ModeDef tdm = mode(*lib, "MP_Subway", "TDM");
			REQUIRE(tdm.kills(1) != nullptr);
			CHECK(tdm.kills(1)->max_kills == 100);
		}
		{
			const std::unique_ptr<EbxLibrary> lib = open_level(m, "XP2_Palace");
			const ModeDef dom = mode(*lib, "XP2_Palace", "Domination0");
			REQUIRE(dom.tickets(1) != nullptr);
			CHECK(dom.tickets(1)->initial_tickets == 150);
			CHECK(dom.flag_count() == 3);
			REQUIRE(dom.size.has_value());
			CHECK(dom.size->player_count == 16);
		}
		{
			const std::unique_ptr<EbxLibrary> lib = open_level(m, "MP_007");
			const ModeDef cql = mode(*lib, "MP_007", "CQL");
			REQUIRE(cql.tickets(1) != nullptr);
			CHECK(cql.tickets(1)->initial_tickets == 300);
			CHECK(cql.flag_area_value().value_or(-1) == Approx(34));
		}
	}

	TEST_CASE("gameplay: KitDef per team (plan §3.4)") {
		Mounted m;
		if (!open_or_skip(m)) {
			return;
		}
		const std::unique_ptr<EbxLibrary> lib = open_level(m, "MP_001");
		const ModeDef cqs = mode(*lib, "MP_001", "CQS");
		struct Expect {
			int team;
			const char *kit;
			const char *primary;
			const char *sidearm;
		};
		const Expect expected[] = {
			{ 1, "gameplay/kits/usassault", "weapons/m16a4/m16a4", "weapons/m9/m9" },
			{ 1, "gameplay/kits/usengineer", "weapons/m4a1/m4a1", "weapons/m9/m9" },
			{ 1, "gameplay/kits/ussupport", "weapons/m27iar/m27iar", "weapons/m9/m9" },
			{ 1, "gameplay/kits/usrecon", "weapons/mk11/mk11", "weapons/m9/m9" },
			{ 2, "gameplay/kits/ruassault", "weapons/ak74m/ak74", "weapons/mp443/mp443" },
			{ 2, "gameplay/kits/ruengineer", "weapons/aks74u/aks74u", "weapons/mp443/mp443" },
		};
		for (const Expect &e : expected) {
			CAPTURE(e.kit);
			const TeamDef *team = cqs.team(e.team);
			REQUIRE(team != nullptr);
			bool listed = false;
			for (const std::string &k : team->kits) {
				listed = listed || k == e.kit;
			}
			CHECK(listed);
			Result<KitDef> kit = read_kit_def(*lib, e.kit);
			REQUIRE_MESSAGE(kit.ok(), (kit.ok() ? std::string() : kit.error().message));
			const KitUnlock *primary = kit->default_unlock(WeaponSlot::Primary);
			const KitUnlock *sidearm = kit->default_unlock(WeaponSlot::Sidearm);
			const KitUnlock *grenade = kit->default_unlock(WeaponSlot::Grenade);
			const KitUnlock *knife = kit->default_unlock(WeaponSlot::Knife);
			REQUIRE(primary != nullptr);
			REQUIRE(sidearm != nullptr);
			REQUIRE(grenade != nullptr);
			REQUIRE(knife != nullptr);
			CHECK(primary->weapon == e.primary);
			CHECK(sidearm->weapon == e.sidearm);
			CHECK(grenade->weapon == "weapons/m67/m67");
			CHECK(!knife->weapon.empty());
			CHECK(kit->default_unlock(WeaponSlot::Gadget2) != nullptr);
		}
	}

	TEST_CASE("gameplay: VehicleDef (plan §7.4)") {
		Mounted m;
		if (!open_or_skip(m)) {
			return;
		}
		const std::unique_ptr<EbxLibrary> lib = open_level(m, "MP_007");

		const VehicleDef m1 = vehicle(*lib, "vehicles/m1a2/m1abrams");
		check_mbt_health(m1);
		const VehicleTurretDef *ring = turret(m1, 0, 1, true);
		const VehicleTurretDef *gun = turret(m1, 0, 0, false);
		const VehicleTurretDef *hmg = turret(m1, 1, 1, false);
		REQUIRE(ring != nullptr);
		REQUIRE(gun != nullptr);
		REQUIRE(hmg != nullptr);
		CHECK(ring->world_space_lock_efficiency == Approx(0.0001));
		CHECK(gun->world_space_lock_efficiency == Approx(0.85));
		CHECK(hmg->world_space_lock_efficiency == Approx(1.0));
		CHECK(gun->constrained);
		CHECK(gun->min_angle == Approx(-20));
		CHECK(gun->max_angle == Approx(0));
		REQUIRE(m1.weapons.size() >= 2);
		CHECK(m1.weapons[0].seat == 0);
		CHECK(m1.weapons[0].classification == "WCPrimary");

		const VehicleDef t90 = vehicle(*lib, "vehicles/t90/t90");
		check_mbt_health(t90);
		const VehicleTurretDef *t90_ring = turret(t90, 0, 1, true);
		const VehicleTurretDef *t90_gun = turret(t90, 0, 0, false);
		REQUIRE(t90_ring != nullptr);
		REQUIRE(t90_gun != nullptr);
		CHECK(t90_ring->world_space_lock_efficiency == Approx(0.0));
		CHECK(t90_gun->world_space_lock_efficiency == Approx(1.0));
		CHECK(t90_gun->min_angle == Approx(-20));
		CHECK(t90_gun->max_angle == Approx(4));

		for (const char *heli : { "vehicles/ah1z/ah1z", "vehicles/mi28/mi28" }) {
			CAPTURE(heli);
			const VehicleDef v = vehicle(*lib, heli);
			CHECK(v.body_mass == Approx(5000));
			CHECK(v.gravity_modifier == Approx(1.0));
			CHECK(v.max_zone_health() == Approx(500));
			CHECK(v.disabled_damage_threshold == Approx(170));
			CHECK(v.emergency_repair_health == Approx(185));
		}
		for (const char *jet : { "vehicles/f18-f/f18", "vehicles/su-35bm-e/su-35bm-e" }) {
			CAPTURE(jet);
			const VehicleDef v = vehicle(*lib, jet);
			CHECK(v.body_mass == Approx(10000));
			CHECK(v.gravity_modifier == Approx(1.0));
			CHECK(v.max_zone_health() == Approx(500));
			CHECK(v.regeneration_rate == Approx(18));
			CHECK(v.regeneration_delay == Approx(10));
			REQUIRE(v.driver() != nullptr);
		}
		const VehicleDef humvee = vehicle(*lib, "vehicles/humveearmored/humveearmored");
		CHECK(humvee.body_mass == Approx(1500));
		CHECK(humvee.gravity_modifier == Approx(1.85));
		CHECK(humvee.seats.size() == 4);
		for (const char *buggy : { "vehicles/vdv_buggy/vdv_buggy", "vehicles/growleritv/growleritv" }) {
			CAPTURE(buggy);
			const VehicleDef v = vehicle(*lib, buggy);
			CHECK(v.body_mass == Approx(800));
			CHECK(v.gravity_modifier == Approx(1.65));
		}
		for (const char *aa : { "vehicles/centurion_c-ram/centurion_c-ram", "vehicles/pantsir/pantsir-s1" }) {
			CAPTURE(aa);
			const VehicleDef v = vehicle(*lib, aa);
			CHECK(v.armor_multiplier == Approx(99999));
		}
		CHECK(!read_vehicle_def(*lib, "vehicles/no/such_vehicle").ok());

		const std::unique_ptr<EbxLibrary> kharg = open_level(m, "MP_018");
		const VehicleDef rhib = vehicle(*kharg, "vehicles/rhib/rhib");
		CHECK(rhib.body_mass == Approx(1000));
		CHECK(rhib.gravity_modifier == Approx(1.25));
	}
}
