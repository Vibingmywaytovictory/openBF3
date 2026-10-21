#include "test_support.h"

#include "bf3/core/gameplay/weapon_catalog.h"
#include "bf3/core/gameplay/weapon_format.h"
#include "bf3/core/gameplay/weapon_resolver.h"
#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/ui/text_db.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace bf3::core;
using namespace bf3::core::gameplay;

namespace {

using Bytes = std::vector<uint8_t>;

void put32(Bytes &r_bytes, size_t p_at, uint32_t p_value) {
	for (int i = 0; i < 4; ++i) {
		r_bytes[p_at + i] = uint8_t(p_value >> (8 * i));
	}
}

ui::TextDatabase make_text_db(std::vector<std::pair<std::string, std::string>> p_entries) {
	std::sort(p_entries.begin(), p_entries.end(), [](const auto &a, const auto &b) { return ui::text_hash(a.first) < ui::text_hash(b.first); });
	const size_t table_at = 0x14 + 16;
	const size_t strings_at = table_at + 8 * p_entries.size();
	Bytes b(strings_at, 0);
	std::memcpy(b.data() + 0x14, "Global", 6);
	size_t offset = 0;
	for (size_t i = 0; i < p_entries.size(); ++i) {
		put32(b, table_at + 8 * i, ui::text_hash(p_entries[i].first));
		put32(b, table_at + 8 * i + 4, uint32_t(offset));
		b.insert(b.end(), p_entries[i].second.begin(), p_entries[i].second.end());
		b.push_back(0);
		offset += p_entries[i].second.size() + 1;
	}
	put32(b, 0, ui::kTextBinaryMagic);
	put32(b, 4, uint32_t(b.size() - 8));
	put32(b, 8, uint32_t(p_entries.size()));
	put32(b, 12, uint32_t(table_at - 8));
	put32(b, 16, uint32_t(strings_at - 8));
	Bytes h(12 + 2 * 256, 0);
	put32(h, 0, ui::kTextHistogramMagic);
	put32(h, 4, uint32_t(h.size() - 8));
	Result<ui::TextDatabase> db = ui::TextDatabase::parse(b, h);
	REQUIRE_MESSAGE(db.ok(), (db.ok() ? std::string() : db.error().message));
	return std::move(*db);
}

std::string without_name_line(const std::string &p_block) {
	const size_t at = p_block.find("\nname         ");
	if (at == std::string::npos) {
		return p_block;
	}
	const size_t end = p_block.find('\n', at + 1);
	return p_block.substr(0, at) + p_block.substr(end);
}

}

TEST_SUITE("inspect-unit") {
	TEST_CASE("format_weapon adds the localised name only with a text database") {
		WeaponDef w;
		w.variant = "weapons/test/u_test";
		w.blueprint = "weapons/test/test";
		w.name_sid = "ID_P_WNAME_TEST";
		w.name_sid_hash = loc_sid_hash(w.name_sid);
		w.fire.rate_of_fire = 800;
		const ui::TextDatabase db = make_text_db({ { "ID_P_WNAME_TEST", "Test Rifle" }, { "ID_P_WNAME_OTHER", "Other" } });

		const std::string plain = format_weapon(w, nullptr, nullptr);
		const std::string named = format_weapon(w, nullptr, nullptr, &db);
		CHECK(plain.find("\nname         ") == std::string::npos);
		CHECK(named.find("\nname sid     ID_P_WNAME_TEST  (hash ") != std::string::npos);
		CHECK(named.find("\nname sid     ID_P_WNAME_TEST  (hash ") + 1 < named.find("\nname         Test Rifle\n"));
		CHECK(without_name_line(named) == plain);
		CHECK(weapon_display_name(w, &db) == "Test Rifle");
		CHECK(weapon_display_name(w, nullptr).empty());

		w.name_sid = "ID_P_WNAME_ABSENT";
		CHECK(weapon_display_name(w, &db) == "ID_P_WNAME_ABSENT");
		CHECK(format_weapon(w, nullptr, nullptr, &db).find("\nname         ID_P_WNAME_ABSENT  (not in the text database)\n") != std::string::npos);

		w.name_sid.clear();
		CHECK(weapon_display_name(w, &db).empty());
		CHECK(format_weapon(w, nullptr, nullptr, &db).find("\nname         -\n") != std::string::npos);
	}

	TEST_CASE("number helpers keep bf3dump's text") {
		CHECK(format_number(800) == "800");
		CHECK(format_number(0.075f) == "0.075");
		CHECK(format_number(std::nan("")) == "-");
		CHECK(format_fixed(75, 1) == "75.0");
		FireFunction f;
		f.logic = FireLogic::Burst;
		f.extra_modes = { FireLogic::Single };
		CHECK(format_fire_modes(f) == "fltBurstFire + [fltSingleFire]");
	}
}

TEST_SUITE("inspect") {
	TEST_CASE("WeaponCatalog over MP_Subway: M16A3 and M16A4 burst with their localised names") {
		bf3::test::Mounted m;
		if (!bf3::test::open_or_skip(m)) {
			return;
		}
		Result<ui::TextDatabase> text = ui::open_text_database(m.install);
		REQUIRE_MESSAGE(text.ok(), (text.ok() ? std::string() : text.error().message));

		WeaponCatalog::Options options;
		options.text = std::make_shared<const ui::TextDatabase>(std::move(*text));
		Result<WeaponCatalog> catalog = WeaponCatalog::open(m.install, options);
		REQUIRE_MESSAGE(catalog.ok(), (catalog.ok() ? std::string() : catalog.error().message));
		const WeaponCatalog &c = *catalog;
		CHECK(c.report().grid_error.empty());
		CHECK(c.report().capsules_error.empty());
		REQUIRE(c.grid() != nullptr);
		REQUIRE(c.capsules() != nullptr);
		CHECK(c.variants().size() == 172);

		CHECK(c.target_of("weapons/m16a4/m16a4") == "weapons/m16a4/u_m16a4");
		CHECK(c.target_of("Weapons/M16A4/U_M16_Burst") == "weapons/m16a4/u_m16_burst");
		Result<WeaponDef> a3 = c.resolve("weapons/m16a4/m16a4");
		REQUIRE(a3.ok());
		CHECK(a3->variant == "weapons/m16a4/u_m16a4");
		CHECK(a3->name_sid == "ID_P_WNAME_M16A4");
		CHECK(c.display_name(*a3) == "M16A3");
		CHECK(a3->fire.rate_of_fire == 800.0f);
		const std::string block = c.format(*a3);
		CHECK(block == format_weapon(*a3, c.grid(), c.capsules(), c.text()));
		CHECK(block.find("\nname         M16A3\n") != std::string::npos);
		CHECK(block.find("\nbody         head 2  body 1  chest 1  legs 0.91  (") != std::string::npos);
		CHECK(without_name_line(block) == format_weapon(*a3, c.grid(), c.capsules()));

		Result<WeaponDef> a4 = c.resolve("weapons/m16a4/u_m16_burst");
		REQUIRE(a4.ok());
		CHECK(a4->name_sid == "ID_P_WNAME_M16");
		CHECK(c.display_name(*a4) == "M16A4");
		CHECK(a4->fire.bullets_per_burst == 3);
		CHECK(c.format(*a4).find("\nname         M16A4\n") != std::string::npos);

		Result<WeaponDef> att = c.resolve("weapons/m16a4/u_m16a4", { "u_m16a4_sound_suppressor" });
		REQUIRE(att.ok());
		CHECK(att->silenced);

		size_t errors = 0;
		for (const std::string &v : c.variants()) {
			errors += c.resolve(v).ok() ? 0 : 1;
		}
		CHECK(errors == 0);
		CHECK_FALSE(c.resolve("weapons/nope/u_nope").ok());

		Result<WeaponSources> sources = open_weapon_sources(m.install);
		REQUIRE(sources.ok());
		CHECK(sources->ui != nullptr);
		CHECK(sources->level_partition == "levels/mp_subway/mp_subway");
		CHECK(weapon_resolve_target(*sources, "weapons/m16a4/m16a4") == "weapons/m16a4/u_m16a4");
		Result<MaterialGrid> grid = load_level_grid(*sources);
		REQUIRE(grid.ok());
		CHECK(grid->report().unresolved == 0);
	}
}
