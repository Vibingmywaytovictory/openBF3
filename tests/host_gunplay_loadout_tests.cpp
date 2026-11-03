#include <doctest/doctest.h>

#include "gunplay_fixture.h"
#include "tdm_fixture.h"

#include "bf3/host/gunplay.h"
#include "bf3/host/kits.h"
#include "bf3/host/loadout.h"

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
}
