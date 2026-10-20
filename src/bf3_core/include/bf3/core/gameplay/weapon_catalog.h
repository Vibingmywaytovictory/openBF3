#pragma once

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

inline constexpr const char *kDefaultWeaponLevel = "Win32/Levels/MP_Subway/MP_Subway";

struct WeaponSources {
	std::shared_ptr<const Install> install;
	std::unique_ptr<EbxLibrary> level;
	std::unique_ptr<EbxLibrary> ui;
	std::unique_ptr<EbxLibrary> fallback;
	std::string level_partition;
	std::string ui_error;
};

Result<WeaponSources> open_weapon_sources(std::shared_ptr<const Install> p_install, std::string_view p_level = kDefaultWeaponLevel);

Result<MaterialGrid> load_level_grid(WeaponSources &r_sources);

std::string weapon_resolve_target(WeaponSources &r_sources, std::string_view p_partition);

class WeaponCatalog {
public:
	struct Options {
		std::string level = kDefaultWeaponLevel;

		std::shared_ptr<const ui::TextDatabase> text;
	};
	struct Report {
		std::string level;
		size_t partitions = 0;
		double open_seconds = 0;
		std::string grid_error;
		std::string capsules_error;
	};

	static Result<WeaponCatalog> open(std::shared_ptr<const Install> p_install, const Options &p_options);

	WeaponCatalog(WeaponCatalog &&) = default;
	WeaponCatalog &operator=(WeaponCatalog &&) = default;

	const Report &report() const { return report_; }
	const WeaponSources &sources() const { return sources_; }
	const MaterialGrid *grid() const { return grid_ ? &*grid_ : nullptr; }
	const std::vector<HitCapsule> *capsules() const { return capsules_ ? &*capsules_ : nullptr; }
	const ui::TextDatabase *text() const { return text_.get(); }

	const std::vector<std::string> &variants() const { return variants_; }

	std::string target_of(std::string_view p_partition) const;

	Result<WeaponDef> resolve(std::string_view p_partition, const std::vector<std::string> &p_unlocks = {}) const;

	std::string format(const WeaponDef &p_weapon) const;

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

}
