#pragma once
// The bf3_fx measurement harness (docs/design/m2a-vfx1.md §7.7, §2 gates G3, G4, G7, G8, G9): independent starts in a
// cleared world, the variable-dt sequence, a frame digest for the determinism comparisons, a persistent lane pool for
// multi-threaded stepping, and the golden / check / bench procedures. `bf3dump fx simulate | check | bench` and the
// fx.install / fx.unit tests call these same functions, so a gate's command and its ctest measure the same thing.
// Godot-free; the pool's threads are its own (never the game's job system).

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

// The variable frame time of `--variable` (gate G4, G7 start 3): uniform in [kVariableDtMin, kVariableDtMax], a
// counter-based draw keyed by (seed, frame index), so the sequence is reproducible.
inline constexpr float kVariableDtMin = 1.0f / 240.0f;
inline constexpr float kVariableDtMax = 1.0f / 20.0f;
float fx_variable_dt(uint64_t p_seed, uint64_t p_frame);

// Persistent worker threads that run one sub-step's items in chunks (run_items over disjoint ranges). Used where the
// per-call thread start of bf3::core::parallel_for would dominate (the determinism comparisons of gate G8).
class LanePool {
public:
	explicit LanePool(unsigned p_lanes);
	~LanePool();
	LanePool(const LanePool &) = delete;
	LanePool &operator=(const LanePool &) = delete;
	unsigned lanes() const;
	// run_items over [0, p_items) in chunks of p_chunk items, spread over the lanes (the calling thread is one).
	void run(FxWorld &r_world, size_t p_items, size_t p_chunk);

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

// One frame through the phased API. p_pool null: every sub-step's items in one run_items call on this thread.
void fx_step_frame(FxWorld &r_world, float p_dt, LanePool *p_pool = nullptr, size_t p_chunk = 1);

// FNV-1a over the bytes of particles() and lights() (no padding in either record), the fields of every
// outputs() record and every FxStats field: two frames with equal digests are identical in the sense of gate G8.
// The effect handle's generation is left out (it keeps counting across clear(), so it differs between a reused and
// a fresh world for the same start); its slot is included.
uint64_t fx_frame_digest(const FxWorld &p_world);

struct HarnessRun {
	uint64_t seed = 1;
	float seconds = 1.0f;
	float dt = 1.0f / 60.0f;
	bool variable = false; // fx_variable_dt(seed, frame) instead of dt
	uint8_t tier = kTierHigh;
};

// Frames of one start: ceil(seconds / dt) at a fixed dt, or variable frames until their sum reaches seconds.
// p_frame is called after each frame with the frame index, the time at its end and its dt.
using FrameHook = std::function<void(const FxWorld &p_world, EffectHandle p_handle, uint64_t p_frame, double p_time, float p_dt)>;
// Start p_index of a multi-start run (docs §7.7): clear(), start p_def once with seed fx_harness_seed(seed, index),
// identity transform, zero velocity, tier, start_age 0, then step. Returns the handle (alive() afterwards tells
// whether the instance outlived the run).
core::Result<EffectHandle> fx_run_start(FxWorld &r_world, const std::shared_ptr<const EffectDef> &p_def, uint64_t p_index, const HarnessRun &p_run,
		const FrameHook &p_frame, LanePool *p_pool = nullptr, size_t p_chunk = 1);

struct HarnessReport {
	bool pass = true;
	std::vector<std::string> lines; // "name\tvalue" rows, printed by bf3dump
	std::vector<std::string> failures;
	FxStats stats; // summed over the starts (counters), maxima for the live_* fields
};

// Per-component start fractions and stats of K starts (`bf3dump fx simulate` without --golden).
HarnessReport fx_simulate_starts(const std::shared_ptr<const EffectDef> &p_def, uint32_t p_starts, const HarnessRun &p_run);

// Gate G3 on fx/weapons/muzzleflashes/generic/fx_muzz_generic_rifle_1p (docs §2, §5.1).
HarnessReport fx_golden_muzzle1p(const std::shared_ptr<const EffectDef> &p_def, uint32_t p_starts, const HarnessRun &p_run);
// Gate G4 on the wrapped em_amb_mp12_oilfire_l_fire_01 (docs §2, §5.2): one start of p_run.seconds (75).
HarnessReport fx_golden_oilfire(const std::shared_ptr<const EffectDef> &p_def, const HarnessRun &p_run);

// Gate G5 (docs §5.3): the counts of the projectile's row under the counting rules of §5.3, then the named cells.
struct ImpactCounts {
	size_t rows = 0;
	size_t effect_relations = 0;
	size_t decal_relations = 0;
	size_t default_row_indices = 0;
	size_t generic_effect_indices = 0;
	size_t no_effect_indices = 0;
	size_t columns_without_effect_relation = 0;
	std::set<std::string> distinct_effects;
	bool default_cells_generic = true; // every default-row index's cell is exactly {fx_impact_generic_01_s, no decal}
};
ImpactCounts fx_impact_counts(const ImpactTable &p_table, uint8_t p_projectile);
// Prints the counts and the 8 named 5.56 cells; checks the §5.3 values when p_level is MP_Subway or MP_007
// (another level: report only, pass).
HarnessReport fx_golden_impacts(const ImpactTable &p_table, std::string_view p_level, uint8_t p_projectile);

// Gate G6 (docs §5.4): the atlas against the names its level's EmitterDocuments use. Checks the Metro values when
// p_level is MP_Subway (another level: the name checks only).
HarnessReport fx_golden_atlas(const EmitterAtlas &p_atlas, std::string_view p_level, const std::set<std::string> &p_used_names, size_t p_documents);

// Gates G7 and G8 over many effects (`bf3dump fx check`, ctest fx.install "check").
struct CheckOptions {
	float seconds = 10.0f;
	uint64_t seed = 1;
	unsigned threads = 0; // effects simulated in parallel (0 = all cores)
	bool determinism = true; // start 0 of every effect again on 8 lanes with item chunks of 1 and 7
	unsigned determinism_lanes = 8;
};
struct EffectCheck {
	std::string name;
	std::vector<std::string> failures;
	bool unchecked_termination = false; // finite extent, but end + 1 s > seconds
	bool finite = false;
	uint32_t max_live = 0;
	uint64_t emitter_starts_dropped = 0;
};
struct CheckReport {
	size_t effects = 0;
	size_t failed_effects = 0;
	size_t finite_effects = 0;
	size_t unchecked_termination = 0;
	size_t determinism_compared = 0; // effects whose start 0 was re-run on the lane pool
	size_t determinism_mismatches = 0;
	uint64_t frames = 0;
	uint64_t particle_steps = 0; // sum of live particles over every frame
	uint64_t emitter_starts_dropped = 0;
	uint32_t max_live = 0;
	std::string max_live_effect;
	std::vector<EffectCheck> results; // every effect, input order
	double seconds = 0;
};
CheckReport fx_check_effects(const std::vector<std::shared_ptr<const EffectDef>> &p_defs, const CheckOptions &p_options);

// Gate G9 (docs §9): the oilfire / 1p muzzle / impact mix on one lane; ns per admitted particle per sub-step.
struct BenchRun {
	double ns_per_particle = 0;
	double mean_live = 0;
	double mean_emitters = 0; // live emitter instances, mean over the timed steps
	uint64_t admitted = 0;
	double timed_seconds = 0;
	FxStats stats;
};
struct BenchReport {
	std::vector<BenchRun> runs;
	double median_ns = 0, min_ns = 0, max_ns = 0;
	bool valid = false; // mean live in [12,000, 20,000], spawns_dropped 0, refused_starts 0 on every run
	std::string invalid_reason;
};
inline constexpr double kBenchGateNs = 50.0;
inline constexpr double kBenchReferenceNs = 24.7;
BenchReport fx_bench(const EffectDef &p_oilfire, const EffectDef &p_muzzle, const EffectDef &p_impact, uint64_t p_seed, uint32_t p_runs = 5,
		uint32_t p_warmup_frames = 720, uint32_t p_timed_frames = 600);

} // namespace bf3::fx
