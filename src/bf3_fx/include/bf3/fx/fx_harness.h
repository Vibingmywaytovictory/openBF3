#pragma once

#include "bf3/core/result.h"
#include "bf3/fx/effect_def.h"
#include "bf3/fx/emitter_atlas.h"
#include "bf3/fx/impact_table.h"
#include "bf3/fx/simulator.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::fx {

inline constexpr float kVariableDtMin = 1.0f / 240.0f;
inline constexpr float kVariableDtMax = 1.0f / 20.0f;
float fx_variable_dt(uint64_t p_seed, uint64_t p_frame);

class LanePool {
public:
	explicit LanePool(unsigned p_lanes);
	~LanePool();
	LanePool(const LanePool &) = delete;
	LanePool &operator=(const LanePool &) = delete;
	unsigned lanes() const;

	void run(FxWorld &r_world, size_t p_items, size_t p_chunk);

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

void fx_step_frame(FxWorld &r_world, float p_dt, LanePool *p_pool = nullptr, size_t p_chunk = 1);

uint64_t fx_frame_digest(const FxWorld &p_world);

struct HarnessRun {
	uint64_t seed = 1;
	float seconds = 1.0f;
	float dt = 1.0f / 60.0f;
	bool variable = false;
	uint8_t tier = kTierHigh;
};

using FrameHook = std::function<void(const FxWorld &p_world, EffectHandle p_handle, uint64_t p_frame, double p_time, float p_dt)>;

core::Result<EffectHandle> fx_run_start(FxWorld &r_world, const std::shared_ptr<const EffectDef> &p_def, uint64_t p_index, const HarnessRun &p_run,
		const FrameHook &p_frame, LanePool *p_pool = nullptr, size_t p_chunk = 1);

struct HarnessReport {
	bool pass = true;
	std::vector<std::string> lines;
	std::vector<std::string> failures;
	FxStats stats;
};

HarnessReport fx_simulate_starts(const std::shared_ptr<const EffectDef> &p_def, uint32_t p_starts, const HarnessRun &p_run);

HarnessReport fx_golden_muzzle1p(const std::shared_ptr<const EffectDef> &p_def, uint32_t p_starts, const HarnessRun &p_run);

HarnessReport fx_golden_oilfire(const std::shared_ptr<const EffectDef> &p_def, const HarnessRun &p_run);

struct ImpactCounts {
	size_t rows = 0;
	size_t effect_relations = 0;
	size_t decal_relations = 0;
	size_t default_row_indices = 0;
	size_t generic_effect_indices = 0;
	size_t no_effect_indices = 0;
	size_t columns_without_effect_relation = 0;
	std::set<std::string> distinct_effects;
	bool default_cells_generic = true;
};
ImpactCounts fx_impact_counts(const ImpactTable &p_table, uint8_t p_projectile);

HarnessReport fx_golden_impacts(const ImpactTable &p_table, std::string_view p_level, uint8_t p_projectile);

HarnessReport fx_golden_atlas(const EmitterAtlas &p_atlas, std::string_view p_level, const std::set<std::string> &p_used_names, size_t p_documents);

struct CheckOptions {
	float seconds = 10.0f;
	uint64_t seed = 1;
	unsigned threads = 0;
	bool determinism = true;
	unsigned determinism_lanes = 8;
};
struct EffectCheck {
	std::string name;
	std::vector<std::string> failures;
	bool unchecked_termination = false;
	bool finite = false;
	uint32_t max_live = 0;
	uint64_t emitter_starts_dropped = 0;
};
struct CheckReport {
	size_t effects = 0;
	size_t failed_effects = 0;
	size_t finite_effects = 0;
	size_t unchecked_termination = 0;
	size_t determinism_compared = 0;
	size_t determinism_mismatches = 0;
	uint64_t frames = 0;
	uint64_t particle_steps = 0;
	uint64_t emitter_starts_dropped = 0;
	uint32_t max_live = 0;
	std::string max_live_effect;
	std::vector<EffectCheck> results;
	double seconds = 0;
};
CheckReport fx_check_effects(const std::vector<std::shared_ptr<const EffectDef>> &p_defs, const CheckOptions &p_options);

struct BenchRun {
	double ns_per_particle = 0;
	double mean_live = 0;
	double mean_emitters = 0;
	uint64_t admitted = 0;
	double timed_seconds = 0;
	FxStats stats;
};
struct BenchReport {
	std::vector<BenchRun> runs;
	double median_ns = 0, min_ns = 0, max_ns = 0;
	bool valid = false;
	std::string invalid_reason;
};
inline constexpr double kBenchGateNs = 50.0;
inline constexpr double kBenchReferenceNs = 24.7;
BenchReport fx_bench(const EffectDef &p_oilfire, const EffectDef &p_muzzle, const EffectDef &p_impact, uint64_t p_seed, uint32_t p_runs = 5,
		uint32_t p_warmup_frames = 720, uint32_t p_timed_frames = 600);

}
