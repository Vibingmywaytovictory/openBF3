#pragma once
// Where weapons are resolved from, shared by `bf3dump weapon(s)` and the in-game inspector (plan §6
// M0 exit): the level superbundle's EbxLibrary (every MP level's main bundle carries the full weapon
// EBX), Win32/UI for the weapon-name SIDs, the level's material grid (with MP_Subway's container as
// the fallback on the delta levels) and the soldier hit capsules.
//
// WeaponSources is the low-level part (libraries opened lazily; bf3dump resolves a single weapon
// without parsing the whole level). WeaponCatalog builds on it for the game: it parses the whole
// level once, lists the variants and resolves any of them, thread-safe after open().

#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/material_grid.h"
#include "bf3/core/gameplay/weapon_def.h"
#include "bf3/core/result.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core {
class Install;
}

namespace bf3::core::ui {
class TextDatabase;
}

namespace bf3::core::gameplay {

// The level superbundle weapons are read from by default.
inline constexpr const char *kDefaultWeaponLevel = "Win32/Levels/MP_Subway/MP_Subway";

struct WeaponSources {
	std::shared_ptr<const Install> install;
	std::unique_ptr<EbxLibrary> level;
	std::unique_ptr<EbxLibrary> ui; // null when Win32/UI did not open (no SIDs then)
	std::unique_ptr<EbxLibrary> fallback; // container pairs for the delta (XP2-4) levels
	std::string level_partition; // "levels/mp_subway/mp_subway"
	std::string ui_error; // why `ui` is null
};

// Opens the level superbundle spec (a superbundle name, a comma list or @all-mp, see
// expand_superbundles) and Win32/UI, both lazily. Fails only when the level does not open.
Result<WeaponSources> open_weapon_sources(std::shared_ptr<const Install> p_install, std::string_view p_level = kDefaultWeaponLevel);

// The level's material grid; for a delta level whose container copy lacks pairs, again with
// MP_Subway's container as the fallback (opened into r_sources.fallback).
Result<MaterialGrid> load_level_grid(WeaponSources &r_sources);

// The partition to resolve for p_partition (lowercased): a SoldierWeaponBlueprint resolves through
// its base variant (for the unlock identifier and the name SID). Parses the whole level library when
// p_partition is a blueprint. Not thread-safe (load_all); call before any concurrent resolve.
std::string weapon_resolve_target(WeaponSources &r_sources, std::string_view p_partition);

class WeaponCatalog {
public:
	struct Options {
		std::string level = kDefaultWeaponLevel;
		// Optional text database for display names and the block's "name" line; kept alive here.
		std::shared_ptr<const ui::TextDatabase> text;
	};
	struct Report {
		std::string level; // superbundle spec
		size_t partitions = 0; // parsed partitions of the level library
		double open_seconds = 0;
		std::string grid_error; // empty when the grid loaded
		std::string capsules_error; // empty when the capsules loaded
	};

	// Opens the sources, parses the whole level library, loads the grid and capsules and lists the
	// variants. Fails only when the level does not open.
	static Result<WeaponCatalog> open(std::shared_ptr<const Install> p_install, const Options &p_options);

	WeaponCatalog(WeaponCatalog &&) = default;
	WeaponCatalog &operator=(WeaponCatalog &&) = default;

	const Report &report() const { return report_; }
	const WeaponSources &sources() const { return sources_; }
	const MaterialGrid *grid() const { return grid_ ? &*grid_ : nullptr; }
	const std::vector<HitCapsule> *capsules() const { return capsules_ ? &*capsules_ : nullptr; }
	const ui::TextDatabase *text() const { return text_.get(); }
	// Every SoldierWeaponUnlockAsset under weapons/, sorted.
	const std::vector<std::string> &variants() const { return variants_; }

	// What resolve() resolves for p_partition: the base variant of a blueprint, else p_partition
	// lowercased.
	std::string target_of(std::string_view p_partition) const;
	// Resolves a variant or blueprint with extra unlocks. Thread-safe.
	Result<WeaponDef> resolve(std::string_view p_partition, const std::vector<std::string> &p_unlocks = {}) const;
	// format_weapon with this catalog's grid, capsules and text database.
	std::string format(const WeaponDef &p_weapon) const;
	// weapon_display_name with this catalog's text database ("" without one or without a SID).
	std::string display_name(const WeaponDef &p_weapon) const;

private:
	WeaponCatalog() = default;

	Report report_;
	WeaponSources sources_;
	std::optional<MaterialGrid> grid_;
	std::optional<std::vector<HitCapsule>> capsules_;
	std::shared_ptr<const ui::TextDatabase> text_;
	std::vector<std::string> variants_;
};

} // namespace bf3::core::gameplay
