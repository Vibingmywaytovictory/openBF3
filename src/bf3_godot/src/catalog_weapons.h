#pragma once

#include "bf3/core/cache/disk_cache.h"
#include "bf3/render/viewmodel_set.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace bf3::godot_side {

struct CatalogViewmodel {
	uint8_t team = 1;
	uint16_t def = 0;
	std::string unlock;
	std::unique_ptr<render::ViewmodelSet> set;
	std::string error;
	double seconds = 0.0;
};

struct CatalogWeaponsStats {
	uint32_t viewmodel_requests = 0;
	uint32_t viewmodel_loaded = 0;
	uint32_t viewmodel_failed = 0;
	uint32_t pending = 0;
	double busy_seconds = 0.0;
};

using CatalogCacheSource = std::function<std::shared_ptr<core::Cache>()>;

class CatalogWeapons {
public:
	CatalogWeapons(std::unique_ptr<render::ViewmodelSources> p_sources, CatalogCacheSource p_cache);
	~CatalogWeapons();
	CatalogWeapons(const CatalogWeapons &) = delete;
	CatalogWeapons &operator=(const CatalogWeapons &) = delete;

	bool request_viewmodel(uint8_t p_team, uint16_t p_def, const std::string &p_unlock);
	bool viewmodel_requested(uint8_t p_team, uint16_t p_def) const;
	std::vector<CatalogViewmodel> take_viewmodels();
	CatalogWeaponsStats stats() const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

}
