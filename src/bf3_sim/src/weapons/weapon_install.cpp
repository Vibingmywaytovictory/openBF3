// The install side of the weapon simulation for the tools and data tests (weapon_install.h).
#include "bf3/sim/weapons/weapon_install.h"

#include "bf3/core/gameplay/grid_pairs.h"
#include "bf3/core/install.h"
#include "bf3/core/thread_pool.h"
#include "bf3/sim/weapons/damage.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <optional>

namespace bf3::sim::weapons {

namespace {

double seconds_since(std::chrono::steady_clock::time_point p_start) {
	return std::chrono::duration<double>(std::chrono::steady_clock::now() - p_start).count();
}

} // namespace

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

core::Result<WeaponCheckInputs> install_weapon_check_inputs(const InstallWeapons &p_weapons, const Mechanics &p_mechanics) {
	WeaponCheckInputs in;
	core::Result<std::shared_ptr<const WeaponTable>> table = install_weapon_table(p_weapons, weapon_check_variants(), install_table_options(p_weapons, p_mechanics));
	if (!table) {
		return table.error();
	}
	core::Result<std::shared_ptr<const WeaponTable>> oracle =
			install_weapon_table(p_weapons, weapon_check_variants(), install_table_options(p_weapons, p_mechanics, true));
	if (!oracle) {
		return oracle.error();
	}
	in.table = *table;
	in.oracle = *oracle;
	in.health = p_weapons.max_health;
	in.eye_height = p_weapons.stand_eye_height;
	return in;
}

bool w4_qualifies(const core::gameplay::WeaponDef &p_def) {
	return p_def.projectile.kind == core::gameplay::ProjectileKind::Bullet && p_def.projectile.start_damage > 0 && !p_def.projectile.instant_hit &&
			p_def.weapon_class != "wcUgl" && p_def.weapon_class != "wcPowerTool";
}

W4Table build_w4_table(const InstallWeapons &p_weapons, const Mechanics &p_mechanics, unsigned p_threads) {
	const auto start = std::chrono::steady_clock::now();
	W4Table t;
	if (p_weapons.catalog == nullptr) {
		return t;
	}
	const std::vector<std::string> &variants = p_weapons.catalog->variants();
	t.variants = variants.size();
	struct Slot {
		bool qualifies = false;
		W4Row row;
	};
	const std::vector<Slot> slots = core::parallel_map<Slot>(
			variants.size(),
			[&](size_t i) {
				Slot s;
				s.row.variant = variants[i];
				core::Result<core::gameplay::WeaponDef> d = p_weapons.catalog->resolve(variants[i]);
				if (!d) {
					// A variant that does not resolve is counted as an error only if it would qualify; it cannot be
					// judged, so it qualifies (a resolve failure is a reader bug the gate must see).
					s.qualifies = true;
					s.row.error = d.error().message;
					return s;
				}
				if (!w4_qualifies(*d)) {
					return s;
				}
				s.qualifies = true;
				s.row.weapon_class = d->weapon_class;
				core::Result<WeaponSimDef> sd = make_weapon_sim_def(*d, *p_weapons.materials);
				if (!sd) {
					s.row.error = sd.error().message;
					return s;
				}
				W4Row &r = s.row;
				r.rpm = sim_effective_rpm(*sd);
				r.muzzle_speed = sd->projectile.muzzle_speed;
				r.start_damage = sd->projectile.start_damage;
				r.end_damage = sd->projectile.end_damage;
				r.falloff_start = sd->projectile.falloff_start;
				r.falloff_end = sd->projectile.falloff_end;
				r.pellets = sd->pellets;
				for (size_t z = 0; z < kHitZoneCount; ++z) {
					r.zone[z] = sd->projectile.zone[z].present ? sd->projectile.zone[z].protection_multiplier : 1.0f;
					r.one_shot_range[z] = one_shot_range(*sd, HitZone(z), p_weapons.max_health, p_mechanics);
				}
				for (size_t k = 0; k < kW4Distances.size(); ++k) {
					r.shots_to_kill[k] = shots_to_kill(*sd, kW4Distances[k], p_weapons.max_health, p_mechanics);
				}
				return s;
			},
			p_threads);
	for (const Slot &s : slots) {
		if (!s.qualifies) {
			continue;
		}
		++t.qualifying;
		t.errors += s.row.error.empty() ? 0 : 1;
		t.rows.push_back(s.row);
	}
	t.seconds = seconds_since(start);
	return t;
}

std::string format_w4_tsv(const W4Table &p_table) {
	static const char *const kZones[kHitZoneCount] = { "head", "body", "chest", "legs" };
	std::string out = "variant\tclass\trpm\tv0\tdamage\tfalloff\tpellets\tx_head\tx_body\tx_chest\tx_legs";
	for (size_t z = 0; z < kHitZoneCount; ++z) {
		for (const double d : kW4Distances) {
			char buf[32];
			std::snprintf(buf, sizeof(buf), "\tstk_%s_%g", kZones[z], d);
			out += buf;
		}
	}
	for (size_t z = 0; z < kHitZoneCount; ++z) {
		out += std::string("\tohk_") + kZones[z];
	}
	out += "\terror\n";
	for (const W4Row &r : p_table.rows) {
		char buf[256];
		std::snprintf(buf, sizeof(buf), "%s\t%s\t%.2f\t%g\t%g-%g\t%g-%g\t%d\t%g\t%g\t%g\t%g", r.variant.c_str(), r.weapon_class.c_str(), r.rpm,
				double(r.muzzle_speed), double(r.start_damage), double(r.end_damage), double(r.falloff_start), double(r.falloff_end), r.pellets,
				double(r.zone[0]), double(r.zone[1]), double(r.zone[2]), double(r.zone[3]));
		out += buf;
		for (size_t z = 0; z < kHitZoneCount; ++z) {
			for (size_t k = 0; k < kW4Distances.size(); ++k) {
				out += "\t" + std::to_string(r.shots_to_kill[k][z]);
			}
		}
		for (size_t z = 0; z < kHitZoneCount; ++z) {
			const double o = r.one_shot_range[z];
			if (std::isinf(o)) {
				out += "\tinf";
			} else if (o < 0) {
				out += "\t-";
			} else {
				std::snprintf(buf, sizeof(buf), "\t%.3f", o);
				out += buf;
			}
		}
		out += "\t" + (r.error.empty() ? std::string("-") : r.error) + "\n";
	}
	return out;
}

} // namespace bf3::sim::weapons
