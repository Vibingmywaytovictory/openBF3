#pragma once

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

inline constexpr size_t kDefaultCompileWorkLimit = size_t(1) << 20;

struct CompileStats {
	uint32_t emitters = 0;
	uint32_t processors = 0;
	uint32_t disabled_processors = 0;
	uint32_t evaluators = 0;
	uint32_t derived_from_base = 0;
	uint32_t parameterised_evaluators = 0;
	uint32_t sounds = 0;
	uint32_t visual_environments = 0;
	uint32_t flipbooks_normalised = 0;
};

struct CompileContext {
	const core::EbxResolver *db = nullptr;
	core::gameplay::WorkBudget budget{ kDefaultCompileWorkLimit };
	CompileStats stats;

	std::vector<std::string> sources;

	std::vector<std::string> unknown_types;
	std::vector<std::string> unknown_enumerators;
};

core::Result<std::vector<EmitterDef>> compile_emitter(std::string_view p_document, CompileContext &r_ctx);

core::Result<EffectDef> compile_effect(std::string_view p_blueprint, CompileContext &r_ctx);

core::Result<EffectDef> compile_emitter_effect(std::string_view p_document, CompileContext &r_ctx);

core::Result<std::string> find_partition(std::span<const std::string> p_catalogue, std::string_view p_name);

struct EffectExtent {
	bool finite = true;
	float spawn_end = 0;
	float end = 0;
};
EffectExtent effect_extent(const EffectDef &p_def);

struct CensusReport {
	size_t blueprints = 0;
	size_t blueprints_compiled = 0;
	size_t emitter_documents = 0;
	size_t emitter_documents_compiled = 0;
	size_t emitter_entities = 0;
	std::array<size_t, kProcessorTypeCount> processor_emitters{};
	std::array<size_t, kEvaluatorTypeCount> evaluator_emitters{};
	std::vector<std::string> unknown_types;
	std::vector<std::string> unknown_enumerators;
	std::vector<std::string> errors;
	size_t dual_role_documents = 0;
	size_t effects_with_transform = 0;
	size_t component_pools = 0;
	size_t flipbooks_normalised = 0;
	size_t max_work_units = 0;
	double seconds = 0;
};

core::Result<CensusReport> run_census(const core::EbxLibrary &p_library, unsigned p_threads = 0);

std::string format_effect_def(const EffectDef &p_def);
std::string format_emitter_def(const EmitterDef &p_def);

}
