#include <doctest/doctest.h>

#include "bf3/core/cache/disk_cache.h"
#include "bf3/host/gunplay.h"
#include "bf3/host/level_vehicles.h"
#include "bf3/host/tdm_brain.h"
#include "bf3/sim/weapons/surface_materials.h"

#include "bot_weapon_templates.h"
#include "tdm_fixture.h"
#include "vehicles/vehicle_hits.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

using namespace bf3;
namespace tt = bf3::test::tdm;

namespace {

using Strengths = std::array<int32_t, ai::kCombatStrengthTypeCount>;

constexpr std::string_view kAk = "weapons/ak74m/u_ak74m";
constexpr std::string_view kM16 = "weapons/m16a4/u_m16a4";
constexpr std::string_view kRpg = "weapons/rpg7/u_rpg7";
constexpr std::string_view kC4 = "weapons/gadgets/c4/u_c4";
constexpr std::string_view kM320 = "weapons/gadgets/m320/u_m320_he";
constexpr std::string_view kGrenade = "weapons/m67/u_m67";

const ai::WeaponTemplate &named(const host::WeaponTemplates &p_templates, std::string_view p_name) {
	const auto found = p_templates.find(p_name);
	REQUIRE_MESSAGE(found != p_templates.end(), std::string(p_name));
	return found->second;
}

uint16_t def_of(const host::GunplayLevel &p_level, std::string_view p_variant) {
	for (size_t d = 0; d < p_level.defs.size(); ++d) {
		if (p_level.defs[d].variant == p_variant) {
			return uint16_t(d);
		}
	}
	FAIL("no def " << std::string(p_variant));
	return sim::weapons::kNoWeapon;
}

std::filesystem::path scratch_dir(std::string_view p_tag) {
	return std::filesystem::temp_directory_path() /
			("openbf3-" + std::string(p_tag) + "-" +
					std::to_string(uint64_t(std::chrono::steady_clock::now().time_since_epoch().count())));
}

void print_template(std::string_view p_name, const ai::WeaponTemplate &p_template) {
	const Strengths &s = p_template.strengths;
	std::printf("weapon templates: %s: strengths %d %d %d %d %d %d, range %.1f..%.1f m, explosion %.1f m%s\n",
			std::string(p_name).c_str(), s[0], s[1], s[2], s[3], s[4], s[5], double(p_template.min_range),
			double(p_template.max_range), double(p_template.explosion_radius), p_template.is_thrown ? ", thrown" : "");
}

}

TEST_SUITE("host-bots-templates") {
	TEST_CASE("weapon templates: MP_007 ConquestSmall0: every weapon of the level gets a template generated from the "
			  "install's weapon and damage data, kills, revives, heals or repairs per minute per combat strength "
			  "type, ranges from the projectile, cached by level and install") {
		tt::TdmLevel *level = tt::load_conquest_bots_level_once();
		if (level == nullptr) {
			return;
		}
		const host::GunplayLevel &gl = *level->shared->gunplay;
		const host::LevelVehicles *vehicles = level->shared->vehicles.get();
		REQUIRE(vehicles != nullptr);
		const host::WeaponTemplates &loaded = level->bots->ai_weapons;
		core::Result<std::shared_ptr<const sim::weapons::WeaponTable>> table =
				host::make_match_table(gl, sim::weapons::Mechanics{}, false, false);
		REQUIRE(table.ok());
		for (const sim::weapons::WeaponSimDef &d : (*table)->defs) {
			CHECK_MESSAGE(loaded.find(d.variant) != loaded.end(), d.variant);
		}
		for (const auto &[name, w] : loaded) {
			CHECK(w.deviation == ai::WeaponTemplate{}.deviation);
			CHECK(w.deviation_correction_time == ai::WeaponTemplate{}.deviation_correction_time);
		}

		for (const auto &team : gl.kits.loadouts) {
			for (const host::KitLoadout &kit : team) {
				std::string carried;
				for (const uint8_t s : host::kGadgetSlots) {
					carried += " " + (kit.defs[s] < gl.defs.size() ? gl.defs[kit.defs[s]].variant : std::string("-"));
				}
				std::printf("weapon templates: team %u %s gadgets:%s\n", unsigned(kit.team),
						host::kit_role_name(host::KitRole(kit.kit)), carried.c_str());
			}
		}
		size_t heavy = 0, light = 0;
		for (const host::LevelVehicleKind &k : vehicles->kinds) {
			const ai::CombatStrengthType t = host::vehicle_strength_type(k);
			heavy += t == ai::CombatStrengthType::HeavyArmour ? 1u : 0u;
			light += t == ai::CombatStrengthType::LightArmour ? 1u : 0u;
			std::printf("weapon templates: %s is %s\n", k.blueprint.c_str(),
					t == ai::CombatStrengthType::HeavyArmour ? "HeavyArmour (tracked)" : "LightArmour (wheeled)");
		}
		CHECK(heavy == 2u);
		CHECK(light == 2u);

		const ai::WeaponTemplate &ak = named(loaded, kAk);
		const ai::WeaponTemplate &m16 = named(loaded, kM16);
		const ai::WeaponTemplate &rpg = named(loaded, kRpg);
		const ai::WeaponTemplate &c4 = named(loaded, kC4);
		const ai::WeaponTemplate &m320 = named(loaded, kM320);
		const ai::WeaponTemplate &grenade = named(loaded, kGrenade);
		for (const auto &[name, w] : { std::pair{ kAk, &ak }, std::pair{ kM16, &m16 }, std::pair{ kRpg, &rpg },
					 std::pair{ kC4, &c4 }, std::pair{ kM320, &m320 }, std::pair{ kGrenade, &grenade } }) {
			print_template(name, *w);
		}
		CHECK(ak.strengths == Strengths{ 78, 0, 0, 0, 0, 0 });
		CHECK(ak.min_range == 0.0f);
		CHECK(ak.max_range == doctest::Approx(900.0f));
		CHECK(ak.explosion_radius == 0.0f);
		CHECK(m16.strengths == Strengths{ 99, 0, 0, 0, 0, 0 });
		CHECK(rpg.strengths == Strengths{ 11, 7, 2, 0, 0, 0 });
		CHECK(rpg.min_range == doctest::Approx(20.0f));
		CHECK(rpg.max_range == doctest::Approx(1000.0f));
		CHECK(rpg.explosion_radius == doctest::Approx(4.0f));
		CHECK(c4.strengths == Strengths{ 90, 90, 54, 0, 0, 0 });
		CHECK(c4.max_range == doctest::Approx(4.0f * 4.0f / sim::weapons::kGadgetGravity).epsilon(0.02));
		CHECK(c4.explosion_radius == doctest::Approx(5.0f));
		CHECK(m320.strengths == Strengths{ 19, 12, 1, 0, 0, 0 });
		CHECK(m320.min_range == doctest::Approx(22.5f).epsilon(0.01));
		CHECK(m320.max_range == doctest::Approx(45.0f * 45.0f / 7.2f).epsilon(0.01));
		CHECK(grenade.is_thrown);
		CHECK_FALSE(m320.is_thrown);
		CHECK_FALSE(ak.is_thrown);
		CHECK(ak.strength(ai::CombatStrengthType::Infantry) > rpg.strength(ai::CombatStrengthType::Infantry));
		CHECK(ak.strength(ai::CombatStrengthType::HeavyArmour) == 0);
		CHECK(rpg.strength(ai::CombatStrengthType::HeavyArmour) > 0);

		core::Result<host::WeaponTemplates> derived = host::derive_weapon_templates(gl, vehicles);
		REQUIRE(derived.ok());
		CHECK(*derived == loaded);
		core::Result<host::WeaponTemplates> reread =
				host::read_weapon_templates(host::format_weapon_templates(loaded), "reread");
		REQUIRE(reread.ok());
		CHECK(*reread == loaded);

		const std::filesystem::path root = scratch_dir("ai-weapons");
		{
			core::Cache cache;
			core::CacheOptions co;
			co.root = root;
			co.guard = level->reader->write_guard();
			std::string detail;
			REQUIRE_MESSAGE(cache.open(std::move(co), &detail) == core::CacheStatus::Ok, detail);
			core::Sha256Digest install{};
			install.bytes[0] = 1;
			core::Sha256Digest other = install;
			other.bytes[0] = 2;
			const auto cached = [&](const core::Sha256Digest &p_install, std::string_view p_level) {
				core::Result<host::CachedWeaponTemplates> got =
						host::cached_weapon_templates(&cache, p_install, p_level, "ConquestSmall0", gl, vehicles);
				REQUIRE_MESSAGE(got.ok(), (got.ok() ? std::string() : got.error().message));
				return *got;
			};
			const host::CachedWeaponTemplates first = cached(install, "MP_007");
			const host::CachedWeaponTemplates again = cached(install, "MP_007");
			const host::CachedWeaponTemplates elsewhere = cached(other, "MP_007");
			const host::CachedWeaponTemplates level_two = cached(install, "MP_001");
			CHECK(first.outcome == core::CacheOutcome::Converted);
			CHECK(again.outcome == core::CacheOutcome::Hit);
			CHECK(elsewhere.outcome == core::CacheOutcome::Converted);
			CHECK(level_two.outcome == core::CacheOutcome::Converted);
			CHECK(first.templates == loaded);
			CHECK(again.templates == loaded);
			std::printf("weapon templates: %zu templates, %zu bytes; cache %s, %s\n", loaded.size(),
					host::format_weapon_templates(loaded).size(), core::cache_outcome_name(first.outcome),
					core::cache_outcome_name(again.outcome));
		}
		std::error_code ec;
		std::filesystem::remove_all(root, ec);
	}

	TEST_CASE("weapon templates: MP_007 ConquestSmall0: a hit on a vehicle is rated as the server damages it, "
			  "through VehicleDamageSource under the weapon's def, variants included") {
		tt::TdmLevel *level = tt::load_conquest_bots_level_once();
		if (level == nullptr) {
			return;
		}
		namespace sw = sim::weapons;
		const host::GunplayLevel &gl = *level->shared->gunplay;
		core::Result<host::TemplateSource> source = host::template_source(gl, level->shared->vehicles.get());
		REQUIRE(source.ok());
		const sw::WeaponTable &t = *source->table;
		host::VehicleDamageSource server;
		server.configure(t, gl.defs);
		size_t own_property = 0, rated = 0;
		const auto check = [&](uint16_t p_def, const sw::WeaponSimDef &p_weapon) {
			const sw::ProjectileSimDef &p = p_weapon.projectile;
			if (p.kind == core::gameplay::ProjectileKind::None) {
				return;
			}
			const bool missile = p.kind == core::gameplay::ProjectileKind::Missile;
			sw::DamageEvent direct;
			direct.def = p_def;
			direct.kind = missile || p.explosion.present ? sw::DamageKind::Direct : sw::DamageKind::Bullet;
			direct.base = !sw::fires_round(p) ? 0.0f : missile ? p.direct_damage : p.start_damage;
			sw::DamageEvent blast;
			blast.def = p_def;
			blast.kind = sw::DamageKind::Blast;
			blast.base = p.explosion.blast_damage;
			own_property += p.property != t.defs[p_def].projectile.property ? 1u : 0u;
			for (const host::StrengthTarget &target : source->targets) {
				if (target.soldier) {
					continue;
				}
				const float hit = server.amount(direct, target.property) * float(std::max(p_weapon.pellets, 1)) +
						(p.explosion.present ? server.amount(blast, target.property) : 0.0f);
				CHECK_MESSAGE(host::hit_damage(*source, p_def, p_weapon, target) ==
								doctest::Approx(hit / target.armor).epsilon(0.001),
						p_weapon.variant);
				++rated;
			}
		};
		for (size_t d = 0; d < t.defs.size() && d < size_t(sw::kNoWeapon); ++d) {
			check(uint16_t(d), t.defs[d]);
			if (d < t.variants.size()) {
				for (const sw::WeaponSimDef &v : t.variants[d]) {
					check(uint16_t(d), v);
				}
			}
		}
		std::printf("weapon templates: %zu vehicle hits rated, %zu variants with a projectile property of their own\n",
				rated, own_property);
		CHECK(own_property > 0u);
	}

	TEST_CASE("weapon templates: MP_007 ConquestSmall0: choseWeapon over an engineer's kit picks the rifle at a "
			  "soldier and the RPG-7 at a tank or a jeep from the generated templates alone; the defibrillator, the "
			  "medkit and the repair tool are chosen by type") {
		tt::TdmLevel *level = tt::load_conquest_bots_level_once();
		if (level == nullptr) {
			return;
		}
		const host::GunplayLevel &gl = *level->shared->gunplay;
		const host::WeaponTemplates &loaded = level->bots->ai_weapons;
		const std::array<std::string_view, 7> kit{ kAk, "weapons/m9/u_m9", kRpg,
			"weapons/gadgets/repairtool/u_repairtool", kGrenade, "weapons/knife/u_knife",
			"weapons/gadgets/medicbag/u_medkit" };
		const auto choices = [&](ai::CombatStrengthType p_type) {
			std::array<ai::WeaponChoice, 8> out{};
			for (size_t s = 0; s < kit.size(); ++s) {
				const uint16_t d = def_of(gl, kit[s]);
				out[s] = host::template_choice(
						named(loaded, kit[s]), p_type, host::weapon_type_of(gl, gl.kits.defib_def, d), 5);
			}
			out[7] = host::template_choice(named(loaded, "weapons/gadgets/defibrillator/u_defib"), p_type,
					host::weapon_type_of(gl, gl.kits.defib_def, def_of(gl, "weapons/gadgets/defibrillator/u_defib")),
					1);
			return out;
		};
		using ai::CombatStrengthType;
		const auto infantry = choices(CombatStrengthType::Infantry);
		const auto heavy = choices(CombatStrengthType::HeavyArmour);
		const auto light = choices(CombatStrengthType::LightArmour);
		CHECK(ai::chose_weapon(infantry, -1, 50.0f, false, false, false, false) == 0);
		CHECK(ai::chose_weapon(infantry, 0, 1.0f, false, false, false, false) == 0);
		CHECK(ai::chose_weapon(infantry, 5, 1.0f, false, false, false, false) == 5);
		CHECK(ai::chose_weapon(heavy, -1, 50.0f, false, false, false, false) == 2);
		CHECK(ai::chose_weapon(heavy, 0, 10.0f, false, false, false, false) == 2);
		CHECK(ai::chose_weapon(light, -1, 50.0f, false, false, false, false) == 2);
		CHECK(ai::chose_weapon(infantry, -1, 0.0f, false, false, true, false) == 7);
		CHECK(ai::chose_weapon(infantry, -1, 0.0f, false, true, false, false) == 6);
		CHECK(ai::chose_weapon(heavy, -1, 0.0f, false, true, false, false) == 3);
		std::array<ai::WeaponChoice, 8> empty = infantry;
		empty[0].ammo = 0;
		CHECK(ai::chose_weapon(empty, 0, 50.0f, false, false, false, false) == 1);
	}

	TEST_CASE("weapon templates: MP_007 ConquestSmall0: a hand-written Weapons/*.ai template beside the AI scripts "
			  "replaces the generated one of its name through load_bot_ai; the rest stay generated") {
		tt::TdmLevel *level = tt::load_conquest_bots_level_once();
		if (level == nullptr) {
			return;
		}
		const host::WeaponTemplates &generated = level->bots->ai_weapons;
		const std::filesystem::path shipped = OPENBF3_AI_DIR;
		const std::filesystem::path root = scratch_dir("ai-weapons-hand");
		std::filesystem::create_directories(root / ai::kWeaponTemplatesDir);
		std::filesystem::copy_file(shipped / ai::kAiDefaultScript, root / ai::kAiDefaultScript);
		std::filesystem::copy_file(shipped / ai::kAiBehavioursScript, root / ai::kAiBehavioursScript);
		std::ofstream(root / ai::kWeaponTemplatesDir / "rpg7.ai", std::ios::binary)
				<< "weaponTemplate.create " << kRpg << "\nweaponTemplate.minRange 5\n"
				<< "weaponTemplate.setStrength HeavyArmour 9\n";
		host::TdmBotConfig config;
		const core::Status st = host::load_bot_ai(root, "MP_007", level->bots->ai_areas, config, generated);
		REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
		const ai::WeaponTemplate &rpg = named(config.weapon_templates, kRpg);
		CHECK(rpg.strengths == Strengths{ 0, 0, 9, 0, 0, 0 });
		CHECK(rpg.min_range == 5.0f);
		CHECK(rpg.max_range == 0.0f);
		CHECK(named(config.weapon_templates, kAk) == named(generated, kAk));
		CHECK(config.weapon_templates.size() == generated.size());

		host::TdmBotConfig none;
		std::filesystem::remove_all(root / ai::kWeaponTemplatesDir);
		REQUIRE(host::load_bot_ai(root, "MP_007", level->bots->ai_areas, none, generated).ok());
		CHECK(none.weapon_templates == generated);
		std::error_code ec;
		std::filesystem::remove_all(root, ec);
	}

	TEST_CASE("weapon templates: MP_007 ConquestSmall0: the bots' per-def table takes a config's template by name in "
			  "any case, generates the missing ones, and maps each hull material to its kind's class") {
		tt::TdmLevel *level = tt::load_conquest_bots_level_once();
		if (level == nullptr) {
			return;
		}
		const host::GunplayLevel &gl = *level->shared->gunplay;
		const host::LevelVehicles *vehicles = level->shared->vehicles.get();
		const host::WeaponTemplates &loaded = level->bots->ai_weapons;
		core::Result<std::shared_ptr<const sim::weapons::WeaponTable>> table =
				host::make_match_table(gl, sim::weapons::Mechanics{}, false, false);
		REQUIRE(table.ok());
		uint16_t ak = sim::weapons::kNoWeapon, rpg = sim::weapons::kNoWeapon;
		for (size_t d = 0; d < (*table)->defs.size(); ++d) {
			ak = (*table)->defs[d].variant == kAk ? uint16_t(d) : ak;
			rpg = (*table)->defs[d].variant == kRpg ? uint16_t(d) : rpg;
		}
		REQUIRE(ak != sim::weapons::kNoWeapon);
		REQUIRE(rpg != sim::weapons::kNoWeapon);

		host::WeaponTemplates hand = loaded;
		ai::WeaponTemplate unarmed = named(loaded, kRpg);
		unarmed.strengths[size_t(ai::CombatStrengthType::HeavyArmour)] = 0;
		hand.erase(hand.find(kRpg));
		hand.emplace("WEAPONS/RPG7/U_RPG7", unarmed);
		core::Result<std::shared_ptr<const host::BotWeaponTemplates>> by_hand =
				host::BotWeaponTemplates::create(**table, gl, hand, vehicles);
		REQUIRE(by_hand.ok());
		CHECK((*by_hand)->of(rpg, 0) == unarmed);
		CHECK((*by_hand)->of(ak, 0) == named(loaded, kAk));

		core::Result<std::shared_ptr<const host::BotWeaponTemplates>> generated =
				host::BotWeaponTemplates::create(**table, gl, {}, vehicles);
		REQUIRE(generated.ok());
		CHECK((*generated)->of(rpg, 0) == named(loaded, kRpg));
		CHECK((*generated)->of(ak, 0) == named(loaded, kAk));
		CHECK((*generated)->of(sim::weapons::kNoWeapon, 0) == ai::WeaponTemplate{});
		for (const host::LevelVehicleKind &k : vehicles->kinds) {
			const sim::weapons::SurfacePair pair = gl.materials->pair_of(k.material);
			REQUIRE(pair.resolved);
			CHECK((*generated)->vehicle_type(pair.property) == host::vehicle_strength_type(k));
		}
	}

	TEST_CASE("weapon templates: MP_007 ConquestSmall0: a bot fires at a vehicle only what it can fire (a gun, a "
			  "launcher, the C4) and at a soldier a gun or launcher; a thrown grenade or another pack is left out, "
			  "so an engineer 25 m from a jeep takes the RPG-7, not the grenade the templates rate higher") {
		tt::TdmLevel *level = tt::load_conquest_bots_level_once();
		if (level == nullptr) {
			return;
		}
		const host::GunplayLevel &gl = *level->shared->gunplay;
		const host::WeaponTemplates &loaded = level->bots->ai_weapons;
		const auto fires = [&](std::string_view p_variant, bool p_vehicle) {
			return host::fires_at(gl, def_of(gl, p_variant), named(loaded, p_variant), p_vehicle);
		};
		for (const bool vehicle : { false, true }) {
			CHECK(fires(kAk, vehicle));
			CHECK(fires(kRpg, vehicle));
			CHECK(fires(kM320, vehicle));
			CHECK_FALSE(fires(kGrenade, vehicle));
			CHECK_FALSE(fires("weapons/gadgets/claymore/u_claymore", vehicle));
			CHECK_FALSE(fires("weapons/gadgets/m15/u_m15", vehicle));
			CHECK_FALSE(fires("weapons/gadgets/medicbag/u_medkit", vehicle));
		}
		CHECK(fires(kC4, true));
		CHECK_FALSE(fires(kC4, false));

		const std::array<std::string_view, 3> kit{ kAk, kRpg, kGrenade };
		std::array<ai::WeaponChoice, 3> rated{}, fired{};
		for (size_t s = 0; s < kit.size(); ++s) {
			const uint16_t d = def_of(gl, kit[s]);
			rated[s] = host::template_choice(named(loaded, kit[s]), ai::CombatStrengthType::LightArmour,
					host::weapon_type_of(gl, gl.kits.defib_def, d), 3);
			fired[s] = rated[s];
			fired[s].strength = host::fires_at(gl, d, named(loaded, kit[s]), true) ? rated[s].strength : 0;
		}
		CHECK(ai::chose_weapon(rated, -1, 25.0f, false, false, false, false) == 2);
		CHECK(ai::chose_weapon(fired, -1, 25.0f, false, false, false, false) == 1);
	}
}
