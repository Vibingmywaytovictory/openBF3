#include "bf3/core/gameplay/weapon_catalog.h"

#include "bf3/core/gameplay/weapon_format.h"
#include "bf3/core/gameplay/weapon_resolver.h"
#include "bf3/core/ids.h"
#include "bf3/core/install.h"
#include "bf3/core/ui/text_db.h"

#include <chrono>

namespace bf3::core::gameplay {

namespace {

Result<std::unique_ptr<EbxLibrary>> open_spec(const std::shared_ptr<const Install> &p_install, std::string_view p_spec) {
	Result<std::unique_ptr<EbxLibrary>> lib = EbxLibrary::open(p_install, expand_superbundles(*p_install, p_spec));
	if (!lib) {
		return error_at(std::string(p_spec), lib.error());
	}
	return lib;
}

}

Result<WeaponSources> open_weapon_sources(std::shared_ptr<const Install> p_install, std::string_view p_level) {
	if (!p_install) {
		return make_error("weapon sources: no install");
	}
	WeaponSources s;
	s.install = p_install;
	Result<std::unique_ptr<EbxLibrary>> level = open_spec(p_install, p_level);
	if (!level) {
		return level.error();
	}
	s.level = std::move(*level);
	Result<std::unique_ptr<EbxLibrary>> ui = open_spec(p_install, "Win32/UI");
	if (ui) {
		s.ui = std::move(*ui);
	} else {
		s.ui_error = ui.error().message;
	}
	std::string lp = to_lower_ascii(p_level);
	if (lp.rfind("win32/", 0) == 0) {
		lp = lp.substr(6);
	}
	s.level_partition = lp;
	return s;
}

Result<MaterialGrid> load_level_grid(WeaponSources &r_sources) {
	const std::optional<std::string> name = MaterialGrid::grid_of_level(*r_sources.level, r_sources.level_partition);
	if (!name) {
		return make_error(r_sources.level_partition + ": no RuntimeMaterialGrid");
	}
	Result<MaterialGrid> grid = MaterialGrid::load(*r_sources.level, *name);
	if (grid && grid->report().unresolved_in_level > 0) {
		if (!r_sources.fallback) {
			Result<std::unique_ptr<EbxLibrary>> fb = open_spec(r_sources.install, kDefaultWeaponLevel);
			if (fb) {
				r_sources.fallback = std::move(*fb);
			}
		}
		if (r_sources.fallback) {
			const EbxResolver *fb[] = { r_sources.fallback.get() };
			grid = MaterialGrid::load(*r_sources.level, *name, fb);
		}
	}
	return grid;
}

std::string weapon_resolve_target(WeaponSources &r_sources, std::string_view p_partition) {
	const std::string partition = to_lower_ascii(p_partition);
	const EbxPartition *root = r_sources.level->partition_by_name(partition);
	if (root != nullptr && root->primary().is_a("SoldierWeaponBlueprint")) {
		if (!r_sources.level->bulk_loaded()) {
			r_sources.level->load_all();
		}
		const std::string base = base_variant_of(*r_sources.level, partition);
		if (!base.empty()) {
			return base;
		}
	}
	return partition;
}

Result<WeaponCatalog> WeaponCatalog::open(std::shared_ptr<const Install> p_install, const Options &p_options) {
	const auto start = std::chrono::steady_clock::now();
	Result<WeaponSources> sources = open_weapon_sources(std::move(p_install), p_options.level);
	if (!sources) {
		return sources.error();
	}
	WeaponCatalog c;
	c.sources_ = std::move(*sources);
	c.text_ = p_options.text;
	c.report_.level = p_options.level;
	const EbxLibrary::LoadReport load = c.sources_.level->load_all();
	c.report_.partitions = load.partitions;
	c.variants_ = list_weapon_variants(*c.sources_.level);
	Result<MaterialGrid> grid = load_level_grid(c.sources_);
	if (grid) {
		c.grid_ = std::move(*grid);
	} else {
		c.report_.grid_error = grid.error().message;
	}
	Result<std::vector<HitCapsule>> capsules = read_hit_capsules(*c.sources_.level);
	if (capsules) {
		c.capsules_ = std::move(*capsules);
	} else {
		c.report_.capsules_error = capsules.error().message;
	}
	c.report_.open_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	return c;
}

std::string WeaponCatalog::target_of(std::string_view p_partition) const {

	const std::string partition = to_lower_ascii(p_partition);
	const EbxPartition *root = sources_.level->partition_by_name(partition);
	if (root != nullptr && root->primary().is_a("SoldierWeaponBlueprint")) {
		const std::string base = base_variant_of(*sources_.level, partition);
		if (!base.empty()) {
			return base;
		}
	}
	return partition;
}

Result<WeaponDef> WeaponCatalog::resolve(std::string_view p_partition, const std::vector<std::string> &p_unlocks) const {
	WeaponResolveOptions options;
	options.unlocks = p_unlocks;
	options.ui = sources_.ui.get();
	return resolve_weapon(*sources_.level, target_of(p_partition), options);
}

std::string WeaponCatalog::format(const WeaponDef &p_weapon) const {
	return format_weapon(p_weapon, grid(), capsules(), text_.get());
}

std::string WeaponCatalog::display_name(const WeaponDef &p_weapon) const {
	return weapon_display_name(p_weapon, text_.get());
}

}
