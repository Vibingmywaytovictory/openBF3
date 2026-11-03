#include <doctest/doctest.h>

#include "gunplay_fixture.h"
#include "tdm_fixture.h"

#include "bf3/host/gunplay.h"
#include "bf3/host/kits.h"
#include "bf3/host/loadout.h"
#include "bf3/host/squads.h"
#include "bf3/net/protocol.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace bf3;
namespace sw = bf3::sim::weapons;
namespace tdm = bf3::test::tdm;

namespace {

uint32_t accessory_id(const host::LoadoutItem &p_item, size_t &r_part, const std::string &p_unlock) {
	for (size_t a = 0; a < host::kLoadoutAccessories; ++a) {
		for (size_t i = 0; i < p_item.accessory_unlocks[a].size(); ++i) {
			if (p_item.accessory_unlocks[a][i] == p_unlock) {
				r_part = a;
				return p_item.accessories[a][i];
			}
		}
	}
	return 0;
}

std::unique_ptr<host::Gunplay> make_gunplay(const tdm::TdmLevel &p_level) {
	host::GunplayConfig config;
	config.mechanics_cfg = OPENBF3_MECHANICS_CFG;
	core::Result<std::unique_ptr<host::Gunplay>> g = host::Gunplay::create(*p_level.shared, config);
	REQUIRE_MESSAGE(g.ok(), (g.ok() ? std::string() : g.error().message));
	return std::move(*g);
}

host::LoadoutChoice pick(const host::LoadoutItem &p_item, const std::vector<std::string> &p_accessories) {
	host::LoadoutChoice c;
	c.items[host::kLoadoutAccessoryWeapon] = p_item.identifier;
	for (const std::string &a : p_accessories) {
		size_t part = 0;
		const uint32_t id = accessory_id(p_item, part, a);
		REQUIRE_MESSAGE(id != 0, a);
		c.accessories[part] = id;
	}
	return c;
}

uint32_t specialization_id(const host::GunplayLevel &p_level, const std::string &p_name) {
	for (const core::gameplay::KitSpecialization &e : p_level.specializations.entries) {
		if (e.unlock == "persistence/unlocks/soldiers/specializations/" + p_name) {
			return e.identifier;
		}
	}
	return 0;
}

host::LoadoutChoice specialization(uint32_t p_identifier) {
	host::LoadoutChoice c;
	c.specialization = p_identifier;
	return c;
}

int32_t reserve_of(const host::Gunplay &p_gunplay, uint8_t p_slot, core::gameplay::WeaponSlot p_weapon) {
	return p_gunplay.weapons(p_slot).slots[size_t(p_weapon)].reserve;
}

sw::DamageEvent blast(uint8_t p_victim, sw::DamageKind p_kind) {
	sw::DamageEvent e;
	e.target = p_victim;
	e.kind = p_kind;
	e.amount = 40.0f;
	return e;
}
}

TEST_SUITE("host-gunplay-loadout") {
	TEST_CASE("loadout: a soldier spawned with picks gets the modified magazine and spread, an unpicked one the plain "
			  "def") {
		tdm::TdmLevel *level = tdm::load_tdm_level_once();
		if (level == nullptr) {
			return;
		}
		const host::GunplayLevel &gl = *level->shared->gunplay;
		size_t variants = 0, weapons = 0;
		for (const std::vector<sw::WeaponSimDef> &v : gl.variants) {
			variants += v.size();
			weapons += v.empty() ? 0u : 1u;
		}
		std::printf("loadout variants: %zu primaries with variants, %zu variant defs, %zu resolves, kits and loadouts "
					"%.2f s\n",
				weapons, variants, gl.variant_resolves, gl.seconds_kits);
		CHECK(weapons > 0);
		std::unique_ptr<host::Gunplay> g = make_gunplay(*level);
		const sw::WeaponTable &table = g->table();

		const uint8_t support = uint8_t(host::KitRole::Support);
		const host::LoadoutKit *us_support = gl.loadouts.kit(1, support);
		REQUIRE(us_support != nullptr);
		const host::LoadoutItem *m27 = us_support->find(0, us_support->defaults[0]);
		REQUIRE(m27 != nullptr);
		CHECK(m27->unlock == "weapons/m27iar/u_m27iar");
		REQUIRE(g->select_loadout(0, 1, support, pick(*m27, { "weapons/m27iar/u_m27iar_extendedmag" })));
		g->on_spawn(0, support, 1, 100);
		REQUIRE(g->armed(0));
		const sw::WeaponSlotState &extended = g->weapons(0).slots[0];
		CHECK(extended.def == m27->def);
		CHECK(extended.magazine == 61);
		CHECK(extended.reserve == 3 * 61);
		CHECK(extended.variant != 0);
		CHECK(g->loadout(0).variants[0] == extended.variant);
		CHECK(host::loadout_gear_variant(gl.loadouts, m27->def, g->loadout(0).choice.accessories) == extended.variant);

		const uint8_t assault = uint8_t(host::KitRole::Assault);
		const host::LoadoutKit *us_assault = gl.loadouts.kit(1, assault);
		REQUIRE(us_assault != nullptr);
		const host::LoadoutItem *m16 = us_assault->find(0, us_assault->defaults[0]);
		REQUIRE(m16 != nullptr);
		REQUIRE(g->select_loadout(
				1, 1, assault, pick(*m16, { "weapons/m16a4/u_m16a4_heavybarrel", "weapons/m16a4/u_m16a4_foregrip" })));
		g->on_spawn(1, assault, 1, 100);
		REQUIRE(g->armed(1));
		const sw::WeaponSlotState &barrel = g->weapons(1).slots[0];
		REQUIRE(barrel.variant != 0);
		const sw::WeaponSimDef &plain = table.defs[m16->def];
		const sw::WeaponSimDef &used = table.def(barrel.def, barrel.variant);
		CHECK(sw::weapon_sim_def_diff(plain, used) ==
				std::vector<std::string>{ "sway", "projectile damage", "projectile suppression" });
		CHECK(barrel.magazine == plain.magazine_capacity);
		const float ads_min = plain.sway[0][1].move[0].min_angle;
		CHECK(used.sway[0][1].move[0].min_angle == doctest::Approx(ads_min * 0.25f * 1.5f));
		std::printf("M16A4 heavy barrel + foregrip: variant %u, standing ADS min spread %.4f -> %.4f deg, falloff end "
					"%.0f -> %.0f m\n",
				unsigned(barrel.variant), double(ads_min), double(used.sway[0][1].move[0].min_angle),
				double(plain.projectile.falloff_end), double(used.projectile.falloff_end));

		g->on_spawn(2, support, 1, 100);
		REQUIRE(g->armed(2));
		const sw::WeaponSlotState &unpicked = g->weapons(2).slots[0];
		CHECK(unpicked.def == m27->def);
		CHECK(unpicked.magazine == table.defs[m27->def].magazine_capacity);
		CHECK(unpicked.magazine == 46);
		CHECK(unpicked.variant == 0);
		size_t unpicked_variants = 0;
		for (const auto &team : gl.loadouts.kits) {
			for (const host::LoadoutKit &k : team) {
				for (const host::LoadoutItem &item : k.slots[host::kLoadoutAccessoryWeapon]) {
					unpicked_variants += host::loadout_variant(item, {}) != 0 ? 1u : 0u;
				}
			}
		}
		CHECK(unpicked_variants == 0);
		tdm::check_install_unchanged(*level);
	}

	TEST_CASE("specializations: Ammo raises the reserve magazines, Grenades adds a grenade, Flak lowers explosive "
			  "damage") {
		tdm::TdmLevel *level = tdm::load_tdm_level_once();
		if (level == nullptr) {
			return;
		}
		const host::GunplayLevel &gl = *level->shared->gunplay;
		using core::gameplay::SpecializationTarget;
		using core::gameplay::WeaponSlot;
		std::printf("specializations: %zu kit entries\n", gl.specializations.entries.size());
		CHECK(gl.specializations.entries.size() == 15);
		const uint32_t ammo = specialization_id(gl, "ammoboostl1");
		const uint32_t grenades = specialization_id(gl, "grenadeboostl1");
		const uint32_t flak = specialization_id(gl, "explosiveresistl1");
		REQUIRE(ammo != 0);
		REQUIRE(grenades != 0);
		REQUIRE(flak != 0);
		CHECK(gl.specializations.find(ammo)->effects ==
				std::vector<std::pair<SpecializationTarget, float>>{ { SpecializationTarget::AmmoClips, 1.5f } });
		CHECK(gl.specializations.find(flak)->effects ==
				std::vector<std::pair<SpecializationTarget, float>>{
						{ SpecializationTarget::ExplosionDamage, 0.85f } });
		CHECK(gl.specializations.find(grenades)->effects ==
				std::vector<std::pair<SpecializationTarget, float>>{
						{ SpecializationTarget::Grenades, 1.0f }, { SpecializationTarget::UnderslungGrenades, 3.0f } });
		std::unique_ptr<host::Gunplay> g = make_gunplay(*level);
		const sw::WeaponTable &table = g->table();
		const uint8_t support = uint8_t(host::KitRole::Support);

		g->on_spawn(0, support, 1, 100);
		REQUIRE(g->select_loadout(1, 1, support, specialization(ammo)));
		g->on_spawn(1, support, 1, 100);
		REQUIRE(g->select_loadout(2, 1, support, specialization(grenades)));
		g->on_spawn(2, support, 1, 100);
		REQUIRE(g->select_loadout(3, 1, support, specialization(flak)));
		g->on_spawn(3, support, 1, 100);
		for (uint8_t s = 0; s < 4; ++s) {
			REQUIRE(g->armed(s));
		}
		for (const WeaponSlot w : { WeaponSlot::Primary, WeaponSlot::Sidearm }) {
			const sw::WeaponSlotState &x = g->weapons(0).slots[size_t(w)];
			REQUIRE(x.def != sw::kNoWeapon);
			const sw::WeaponSimDef &d = table.def(x.def, x.variant);
			CHECK(reserve_of(*g, 0, w) == d.magazines * d.magazine_capacity);
			const int32_t boosted = int32_t(std::round(d.magazines * 1.5)) * d.magazine_capacity;
			CHECK(reserve_of(*g, 1, w) == boosted);
			CHECK(reserve_of(*g, 2, w) == reserve_of(*g, 0, w));
			std::printf("Ammo, slot %u: reserve %d -> %d\n", unsigned(w), reserve_of(*g, 0, w), reserve_of(*g, 1, w));
		}
		const sw::WeaponSlotState &grenade = g->weapons(0).slots[size_t(WeaponSlot::Grenade)];
		REQUIRE(grenade.def != sw::kNoWeapon);
		const int32_t capacity = table.def(grenade.def, grenade.variant).magazine_capacity;
		CHECK(reserve_of(*g, 2, WeaponSlot::Grenade) == reserve_of(*g, 0, WeaponSlot::Grenade) + capacity);
		CHECK(reserve_of(*g, 1, WeaponSlot::Grenade) == reserve_of(*g, 0, WeaponSlot::Grenade));
		std::printf("Grenades: %d in hand, reserve %d -> %d\n", grenade.magazine,
				reserve_of(*g, 0, WeaponSlot::Grenade), reserve_of(*g, 2, WeaponSlot::Grenade));

		CHECK(g->received_damage(3, blast(3, sw::DamageKind::Blast)) == doctest::Approx(40.0f * 0.85f));
		CHECK(g->received_damage(3, blast(3, sw::DamageKind::DudBlast)) == doctest::Approx(40.0f * 0.85f));
		CHECK(g->received_damage(3, blast(3, sw::DamageKind::Bullet)) == 40.0f);
		CHECK(g->received_damage(3, blast(3, sw::DamageKind::Direct)) == 40.0f);
		CHECK(g->received_damage(0, blast(0, sw::DamageKind::Blast)) == 40.0f);
		CHECK(g->received_damage(1, blast(1, sw::DamageKind::Blast)) == 40.0f);
		tdm::check_install_unchanged(*level);
	}

	TEST_CASE("specializations: a squad variant boosts its squad's members, a personal one only its picker") {
		tdm::TdmLevel *level = tdm::load_tdm_level_once();
		if (level == nullptr) {
			return;
		}
		const host::GunplayLevel &gl = *level->shared->gunplay;
		using core::gameplay::WeaponSlot;
		const uint32_t squad_ammo = specialization_id(gl, "ammoboostl2");
		const uint32_t squad_flak = specialization_id(gl, "explosiveresistl2");
		const uint32_t ammo = specialization_id(gl, "ammoboostl1");
		REQUIRE(squad_ammo != 0);
		REQUIRE(squad_flak != 0);
		REQUIRE(ammo != 0);
		CHECK(gl.specializations.find(squad_ammo)->squad);
		CHECK(gl.specializations.find(squad_flak)->squad);
		CHECK_FALSE(gl.specializations.find(ammo)->squad);
		std::unique_ptr<host::Gunplay> g = make_gunplay(*level);
		host::SquadTable squads;
		squads.configure(net::kMaxSlots, host::kDefaultSquadSize);
		REQUIRE(squads.assign(0, 1) == 1);
		REQUIRE(squads.assign(1, 1) == 1);
		REQUIRE(squads.create(2, 1) == host::SquadRefusal::None);
		REQUIRE(squads.join(3, 1, squads.squad_of(2)) == host::SquadRefusal::None);
		CHECK(squads.squad_of(2) != squads.squad_of(0));
		g->set_squads(&squads);
		const uint8_t support = uint8_t(host::KitRole::Support);

		g->on_spawn(5, support, 1, 100);
		const int32_t plain = reserve_of(*g, 5, WeaponSlot::Primary);
		REQUIRE(g->select_loadout(0, 1, support, specialization(squad_ammo)));
		g->on_spawn(0, support, 1, 100);
		g->on_spawn(1, support, 1, 100);
		REQUIRE(g->select_loadout(2, 1, support, specialization(ammo)));
		g->on_spawn(2, support, 1, 100);
		g->on_spawn(3, support, 1, 100);
		g->on_spawn(4, support, 1, 100);
		const sw::WeaponSlotState &x = g->weapons(5).slots[size_t(WeaponSlot::Primary)];
		const sw::WeaponSimDef &d = g->table().def(x.def, x.variant);
		const int32_t boosted = int32_t(std::round(d.magazines * 1.5)) * d.magazine_capacity;
		REQUIRE(boosted > plain);
		CHECK(reserve_of(*g, 0, WeaponSlot::Primary) == boosted);
		CHECK(reserve_of(*g, 1, WeaponSlot::Primary) == boosted);
		CHECK(reserve_of(*g, 2, WeaponSlot::Primary) == boosted);
		CHECK(reserve_of(*g, 3, WeaponSlot::Primary) == plain);
		CHECK(reserve_of(*g, 4, WeaponSlot::Primary) == plain);
		std::printf("squad Ammo: picker and mate reserve %d, other squad %d, no squad %d\n",
				reserve_of(*g, 1, WeaponSlot::Primary), reserve_of(*g, 3, WeaponSlot::Primary),
				reserve_of(*g, 4, WeaponSlot::Primary));

		REQUIRE(g->select_loadout(3, 1, support, specialization(squad_flak)));
		g->on_spawn(3, support, 1, 200);
		CHECK(g->received_damage(2, blast(2, sw::DamageKind::Blast)) == doctest::Approx(40.0f * 0.85f));
		CHECK(g->received_damage(3, blast(3, sw::DamageKind::Blast)) == doctest::Approx(40.0f * 0.85f));
		CHECK(g->received_damage(0, blast(0, sw::DamageKind::Blast)) == 40.0f);
		CHECK(g->received_damage(4, blast(4, sw::DamageKind::Blast)) == 40.0f);
		g->clear(3);
		CHECK(g->received_damage(2, blast(2, sw::DamageKind::Blast)) == 40.0f);
		tdm::check_install_unchanged(*level);
	}
}
