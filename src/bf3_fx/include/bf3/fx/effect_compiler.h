#pragma once
// The effect compiler (docs/design/m2a-vfx1.md §6.1-§6.2, §10; plan §3.14 "compile every EmitterDocument /
// EffectBlueprint from EBX into a flat EffectDef"). Reads through any EbxResolver (a level's EbxDb, an EbxLibrary
// over a level superbundle or @all-mp); never opens a file, never writes. Godot-free; thread-safe across items when
// the resolver is (EbxLibrary and a read-only EbxDb are).
//
// What it reads (verified with bf3dump types/dump over @all-mp, 2026-10-15; docs §4):
//   EffectBlueprint.Object -> EffectEntityData {Components[], MaxInstanceCount, ResetInstanceWhenStarted,
//       CullDistance, StartDelay, HighEndPc/MediumPc/LowEndPc}; Components hold only EmitterEntityData (4,353),
//       SoundEffectEntityData (319) and VisualEnvironmentEffectEntityData (88), all internal to the blueprint
//       partition; EmitterEntityData.Emitter is an external reference to an EmitterDocument partition.
//   EmitterDocument {RootProcessor, TemplateData -> EmitterTemplateData}; ProcessorData.NextProcessor and .Pre are
//       internal references; Pre names one EvaluatorData leaf.
//   BaseEmitterData (121, Enable false on all): processors come from EmitterAsset's chain when the document's own
//       reachable chain holds nothing else (76: the unreachable "orphan" copy in the partition equals the base chain
//       instance for instance), else from the document's own chain (45: a full flattened copy with overrides);
//       the template always from the document (docs §6.2, a decision the census made, not an inference).
//   EmitterData (174): EmitterAssets[] child documents, compiled into the same EffectDef.
//
// Errors, never crashes: an unknown processor or evaluator type, an unknown enumerator, a broken or cyclic chain, a
// duplicate type, a non-finite or out-of-range number, a count past the fx_types.h bounds or an exhausted
// WorkBudget fail the item with "<partition>: <reason>". Disabled processors (Enable false: BaseEmitterData 121,
// TurbulanceData 7, UpdateRotationData 5) are skipped and counted.

#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/work_budget.h"
#include "bf3/core/result.h"
#include "bf3/fx/effect_def.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::fx {

// Retail effects need a few thousand units (one per instance examined, array element scanned, reference followed);
// hostile graphs end on this budget (docs §10).
inline constexpr size_t kDefaultCompileWorkLimit = size_t(1) << 20;

struct CompileStats {
	uint32_t emitters = 0; // emitter defs produced (components' emitters, children, bases resolved)
	uint32_t processors = 0; // present slots
	uint32_t disabled_processors = 0;
	uint32_t evaluators = 0;
	uint32_t derived_from_base = 0; // emitters whose processors came from a BaseEmitterData's base
	uint32_t parameterised_evaluators = 0; // EmitterParameter other than None
	uint32_t sounds = 0;
	uint32_t visual_environments = 0;
};

struct CompileContext {
	const core::EbxResolver *db = nullptr;
	core::gameplay::WorkBudget budget{ kDefaultCompileWorkLimit };
	CompileStats stats;
	// Every partition the compile looked up, lowercase, sorted and distinct (the cache's dependency list,
	// effect_cache.h). Filled by the compiler itself: it wraps `db` in a recording resolver (the weapon_blob.cpp
	// RecordingResolver pattern) that records every partition() / partition_by_name() / name_of() lookup, found or
	// absent, so no call site does manual bookkeeping and an absent base or child document that later appears in a
	// patch changes the key.
	std::vector<std::string> sources;
	// Names of types and enumerators the tables do not know (the census gate requires both empty).
	std::vector<std::string> unknown_types;
	std::vector<std::string> unknown_enumerators;
};

// One EmitterDocument partition into one emitter def (its children are compiled too and returned after it, the
// def's `children` indexing that vector: element 0 is the requested emitter).
core::Result<std::vector<EmitterDef>> compile_emitter(std::string_view p_document, CompileContext &r_ctx);

// One EffectBlueprint partition into one EffectDef (emitters deduplicated by (document name, role), effect_def.h).
core::Result<EffectDef> compile_effect(std::string_view p_blueprint, CompileContext &r_ctx);

// One lone EmitterDocument wrapped into a one-component EffectDef, for tools and goldens (`bf3dump fx simulate
// --emitter`, gate G4): name = the document, identity transforms, one Emitter component with SpawnProbability 1,
// StartDelay 0, all tiers, MaxInstanceCount 1 with reset, emitters = compile_emitter's result. Never cached.
core::Result<EffectDef> compile_emitter_effect(std::string_view p_document, CompileContext &r_ctx);

// The one catalogue name that is p_name or ends with "/" + p_name (ASCII case-insensitive), so a tool can take
// "fx_muzz_generic_rifle_1p" for "fx/weapons/muzzleflashes/generic/fx_muzz_generic_rifle_1p". No match, or more
// than one, is an error that lists up to 8 candidates. EffectLibrary::get and the compiler take full names only.
core::Result<std::string> find_partition(std::span<const std::string> p_catalogue, std::string_view p_name);

// When an effect is over (docs §6.5, gate G7), from the def alone, in real seconds after the start: spawn_end = the
// latest moment any emitter can still spawn = effect StartDelay + component StartDelay + window / TimeScale (a
// child: its parent's spawn_end + its own window / TimeScale); end = the latest such moment plus the longest
// possible particle life / TimeScale, where the life bound is UpdateAge Lifetime times an upper bound of its Pre
// (Random max(Min, Max); Polynomial max(MinClamp, MaxClamp) * |ScaleValue|; any other evaluator 1); an emitter
// without an UpdateAge slot ends at its spawn_end (its particles die with the window). `finite` is false when any
// reachable emitter is continuous (Lifetime 0) or repeats (RepeatParticleSpawning).
struct EffectExtent {
	bool finite = true;
	float spawn_end = 0;
	float end = 0;
};
EffectExtent effect_extent(const EffectDef &p_def);

// ---- census (gate G1; `bf3dump fx census`, ctest fx.install) -----------------------------------------------

struct CensusReport {
	size_t blueprints = 0; // EffectBlueprint partitions found
	size_t blueprints_compiled = 0;
	size_t emitter_documents = 0; // EmitterDocument partitions found
	size_t emitter_documents_compiled = 0;
	size_t emitter_entities = 0; // EmitterEntityData over all compiled effects
	std::array<size_t, kProcessorTypeCount> processor_emitters{}; // emitters using each type
	std::array<size_t, kEvaluatorTypeCount> evaluator_emitters{};
	std::vector<std::string> unknown_types; // distinct, sorted
	std::vector<std::string> unknown_enumerators;
	std::vector<std::string> errors; // the first 32 "<partition>: <message>"
	size_t dual_role_documents = 0; // documents one effect uses both as a component emitter and as a child
	size_t effects_with_transform = 0; // EffectEntityData.Transform not identity (retail 21)
	size_t component_pools = 0; // EmitterEntityData with MaxInstanceCount != 1 or reset false (retail 4 + 8)
	size_t flipbooks_normalised = 0; // TextureRef frame count or columns stored 0 -> 1 (retail 1)
	size_t max_work_units = 0; // most units one item spent (retail headroom)
	double seconds = 0;
};

// Compiles every EffectBlueprint and every EmitterDocument the library catalogues, on p_threads lanes
// (bf3::core::parallel_for; 0 = all cores). Fails only when the library cannot be enumerated.
core::Result<CensusReport> run_census(const core::EbxLibrary &p_library, unsigned p_threads = 0);

// ---- canonical text (goldens; `bf3dump fx compile`) --------------------------------------------------------

// Every field of a def in a fixed order, floats as %.9g, one line per slot/evaluator/component. Stable across
// runs, threads and cache hits (fx.install compares a cold and a warm def through it).
std::string format_effect_def(const EffectDef &p_def);
std::string format_emitter_def(const EmitterDef &p_def);

} // namespace bf3::fx
