#pragma once
// The effects of one level session (docs/design/m2a-vfx1.md §7.1): compiled EffectDefs by blueprint name, compiled on
// first use through the cache (effect_cache.h), shared read-only with FxWorld instances, plus the level's emitter
// atlas and impact table. The seam VFX2 (renderer: atlas bindings per emitter) and VFX3 (triggers: get by name,
// impact lookup) build on. Godot-free; every method is thread-safe.

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

// Where an emitter's texture and normal map sit in this level's atlas (nullptr: the name is not in the atlas; VFX2
// draws such an emitter with the atlas's `systems/emitter/defaulttexture` entry and counts it).
struct AtlasBinding {
	const AtlasEntry *texture = nullptr;
	const AtlasEntry *normal_map = nullptr;
};

class EffectLibrary {
public:
	struct Sources {
		FxSources fx; // the level's resolver and partition digests
		core::Cache *cache = nullptr; // may be null (compile uncached)
		const EmitterAtlas *atlas = nullptr; // may be null (no bindings)
		const ImpactTable *impacts = nullptr; // may be null
	};
	explicit EffectLibrary(const Sources &p_sources);
	~EffectLibrary();
	EffectLibrary(const EffectLibrary &) = delete;
	EffectLibrary &operator=(const EffectLibrary &) = delete;

	// The blueprint's def, compiled (or read from the cache) once; later calls return the same pointer. A failed
	// compile is remembered and returned again (the error is logged once by the caller). p_blueprint is the full
	// partition name (tools resolve a short name first with find_partition, effect_compiler.h).
	core::Result<std::shared_ptr<const EffectDef>> get(std::string_view p_blueprint);
	// Compiles a list on p_threads lanes (load time; bf3::core::parallel_for). Returns the number that failed.
	size_t preload(const std::vector<std::string> &p_blueprints, unsigned p_threads = 0);
	// p_def must come from get(); one binding per p_def->emitters entry.
	std::span<const AtlasBinding> bindings(const EffectDef &p_def) const;
	const ImpactTable *impacts() const;
	core::ConvertTotals &totals();

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace bf3::fx
