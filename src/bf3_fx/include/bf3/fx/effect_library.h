#pragma once

#include "bf3/core/cache/disk_cache.h"
#include "bf3/core/result.h"
#include "bf3/fx/effect_cache.h"
#include "bf3/fx/effect_def.h"
#include "bf3/fx/emitter_atlas.h"
#include "bf3/fx/impact_table.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::fx {

struct AtlasBinding {
	const AtlasEntry *texture = nullptr;
	const AtlasEntry *normal_map = nullptr;
};

class EffectLibrary {
public:
	struct Sources {
		FxSources fx;
		core::Cache *cache = nullptr;
		const EmitterAtlas *atlas = nullptr;
		const ImpactTable *impacts = nullptr;
	};
	explicit EffectLibrary(const Sources &p_sources);
	~EffectLibrary();
	EffectLibrary(const EffectLibrary &) = delete;
	EffectLibrary &operator=(const EffectLibrary &) = delete;

	core::Result<std::shared_ptr<const EffectDef>> get(std::string_view p_blueprint);

	size_t preload(const std::vector<std::string> &p_blueprints, unsigned p_threads = 0);

	std::span<const AtlasBinding> bindings(const EffectDef &p_def) const;
	const ImpactTable *impacts() const;
	core::ConvertTotals &totals();

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

}
