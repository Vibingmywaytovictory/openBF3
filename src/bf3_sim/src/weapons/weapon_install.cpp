#include "bf3/sim/weapons/weapon_install.h"

#include "bf3/core/gameplay/grid_pairs.h"
#include "bf3/core/install.h"
#include "bf3/core/thread_pool.h"
#include "bf3/core/util/elapsed.h"
#include "bf3/sim/weapons/damage.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <optional>

namespace bf3::sim::weapons {

namespace {

using bf3::core::seconds_since;
}

core::Result<InstallWeapons> open_install_weapons(std::shared_ptr<const core::Install> p_install, std::string_view p_level) {
	const auto start = std::chrono::steady_clock::now();
	InstallWeapons w;
	core::gameplay::WeaponCatalog::Options options;
	options.level = std::string(p_level);
	core::Result<core::gameplay::WeaponCatalog> catalog = core::gameplay::WeaponCatalog::open(std::move(p_install), options);
	if (!catalog) {
		return catalog.error();
	}
	w.catalog = std::make_unique<core::gameplay::WeaponCatalog>(std::move(*catalog));
	const core::gameplay::WeaponCatalog &c = *w.catalog;
	if (c.grid() == nullptr) {
		return core::make_error("open_install_weapons: the material grid did not load: " + c.report().grid_error);
	}
	if (c.capsules() == nullptr) {
		return core::make_error("open_install_weapons: the hit capsules did not load: " + c.report().capsules_error);
	}
	w.grid = std::make_shared<const core::gameplay::MaterialGrid>(*c.grid());
	w.capsules = *c.capsules();
	core::Result<std::vector<core::gameplay::MaterialPairRef>> pairs = core::gameplay::read_grid_pairs(*c.sources().level, w.grid->report().grid, *w.grid);
	if (!pairs) {
		return pairs.error();
	}
	w.grid_pairs = std::move(*pairs);
	core::Result<SurfaceMaterialTable> materials = SurfaceMaterialTable::make(w.grid, w.grid_pairs, w.capsules);
	if (!materials) {
		return materials.error();
	}
	w.materials = std::make_shared<const SurfaceMaterialTable>(std::move(*materials));
	core::Result<core::gameplay::SoldierDef> soldier = core::gameplay::read_soldier_def(*c.sources().level);
	if (!soldier) {
		return soldier.error();
	}
	w.soldier = std::move(*soldier);
	if (w.soldier.max_health > 0 && std::isfinite(w.soldier.max_health)) {
		w.max_health = w.soldier.max_health;
	}
	if (const core::gameplay::PoseDef *stand = w.soldier.pose(core::gameplay::SoldierPose::Stand); stand != nullptr && stand->eye_height > 0) {
		w.stand_eye_height = stand->eye_height;
	}
	for (const auto &[name, value] : w.soldier.sprint_settings) {
		if (name == "RecoverTime" && std::isfinite(value) && value >= 0 && value <= 60) {
			w.sprint_recover_seconds = float(value);
		}
	}
	w.seconds = seconds_since(start);
	return w;
}

WeaponTableOptions install_table_options(const InstallWeapons &p_weapons, const Mechanics &p_mechanics, bool p_infinite_magazine) {
	WeaponTableOptions o;
	o.mechanics = p_mechanics;
	o.infinite_magazine = p_infinite_magazine;
	o.sprint_recover_seconds = p_weapons.sprint_recover_seconds;
	return o;
}

core::Result<std::shared_ptr<const WeaponTable>> install_weapon_table(const InstallWeapons &p_weapons, std::span<const std::string_view> p_variants,
		const WeaponTableOptions &p_options) {
	if (p_weapons.catalog == nullptr || p_weapons.materials == nullptr) {
		return core::make_error("install_weapon_table: no catalog");
	}
	std::vector<core::gameplay::WeaponDef> defs;
	defs.reserve(p_variants.size());
	for (const std::string_view v : p_variants) {
		core::Result<core::gameplay::WeaponDef> d = p_weapons.catalog->resolve(v);
		if (!d) {
			return core::make_error(std::string(v) + ": " + d.error().message);
		}
		defs.push_back(std::move(*d));
	}
	return make_weapon_table(defs, p_weapons.materials, p_options);
}

}
