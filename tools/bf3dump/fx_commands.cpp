#include "fx_commands.h"

#include "bf3/core/cache/disk_cache.h"
#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/material_grid.h"
#include "bf3/core/gameplay/weapon_blob.h"
#include "bf3/core/gameplay/weapon_catalog.h"
#include "bf3/core/ids.h"
#include "bf3/core/thread_pool.h"
#include "bf3/fx/effect_cache.h"
#include "bf3/fx/effect_compiler.h"
#include "bf3/fx/emitter_atlas.h"
#include "bf3/fx/fx_harness.h"
#include "bf3/fx/impact_table.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>

using namespace bf3::core;
using namespace bf3::fx;

namespace bf3dump {

namespace {

constexpr const char *kMuzzle1p = "fx/weapons/muzzleflashes/generic/fx_muzz_generic_rifle_1p";
constexpr const char *kOilfire = "fx/ambient/levelspecific/mp_12/emitters_oilfire/em_amb_mp12_oilfire_l_fire_01";
constexpr const char *kImpactConcrete = "fx_impact_concrete_01_s_xp2";

double seconds_since(std::chrono::steady_clock::time_point p_start) {
	return std::chrono::duration<double>(std::chrono::steady_clock::now() - p_start).count();
}

struct Args {
	std::vector<std::string> positional;
	std::map<std::string, std::string> options;
	std::set<std::string> flags;
	std::string error;

	bool has(const std::string &p_name) const { return options.contains(p_name) || flags.contains(p_name); }
	std::string get(const std::string &p_name, const std::string &p_default = std::string()) const {
		const auto it = options.find(p_name);
		return it == options.end() ? p_default : it->second;
	}
};

Args parse(const std::vector<std::string> &p_args, const std::set<std::string> &p_valued, const std::set<std::string> &p_flags) {
	Args a;
	for (size_t i = 0; i < p_args.size(); ++i) {
		const std::string &s = p_args[i];
		if (s.rfind("--", 0) != 0) {
			a.positional.push_back(s);
			continue;
		}
		const size_t eq = s.find('=');
		const std::string name = s.substr(0, eq);
		if (p_valued.contains(name)) {
			if (eq != std::string::npos) {
				a.options[name] = s.substr(eq + 1);
			} else if (i + 1 < p_args.size()) {
				a.options[name] = p_args[++i];
			} else {
				a.error = name + ": value missing";
			}
		} else if (p_flags.contains(name) && eq == std::string::npos) {
			a.flags.insert(name);
		} else {
			a.error = "unexpected argument: " + s;
		}
	}
	return a;
}

template <class T>
bool number_of(const Args &p_a, const std::string &p_name, T &r_value) {
	if (!p_a.options.contains(p_name)) {
		return true;
	}
	const std::string v = p_a.get(p_name);
	const auto res = std::from_chars(v.data(), v.data() + v.size(), r_value);
	if (res.ec != std::errc() || res.ptr != v.data() + v.size()) {
		std::fprintf(stderr, "%s: not a number: %s\n", p_name.c_str(), v.c_str());
		return false;
	}
	return true;
}

std::unique_ptr<EbxLibrary> open_library(const EbxContext &p_ctx, const std::string &p_spec, bool p_bulk) {
	const auto start = std::chrono::steady_clock::now();
	Result<std::unique_ptr<EbxLibrary>> lib = EbxLibrary::open(p_ctx.install, expand_superbundles(*p_ctx.install, p_spec));
	if (!lib) {
		std::fprintf(stderr, "%s\n", lib.error().message.c_str());
		return nullptr;
	}
	if (p_bulk) {
		const EbxLibrary::LoadReport r = (*lib)->load_all();
		std::fprintf(stderr, "%s: %zu partitions (%zu failed) in %.1f s\n", p_spec.c_str(), r.partitions, r.failed, seconds_since(start));
	}
	return std::move(*lib);
}

struct CacheHandle {
	std::unique_ptr<Cache> cache;
	bool ok = true;
};

CacheHandle open_cache(const EbxContext &p_ctx, const Args &p_a) {
	CacheHandle h;
	if (!p_a.options.contains("--cache")) {
		return h;
	}
	h.cache = std::make_unique<Cache>();
	CacheOptions options;
	options.root = std::filesystem::path(p_a.get("--cache"));
	options.guard = p_ctx.reader->write_guard();
	std::string detail;
	const CacheStatus status = h.cache->open(std::move(options), &detail);
	if (status != CacheStatus::Ok) {
		std::fprintf(stderr, "fx: cache %s: %s %s\n", p_a.get("--cache").c_str(), cache_status_name(status), detail.c_str());
		h.ok = false;
	}
	return h;
}

struct Catalogue {
	std::vector<std::string> blueprints;
	std::vector<std::string> documents;
};

Catalogue catalogue_of(const EbxLibrary &p_lib) {
	Catalogue c;
	for (const EbxLibrary::Entry &e : p_lib.entries()) {
		if (e.partition->primary().type_name() == "EffectBlueprint") {
			c.blueprints.emplace_back(e.name);
		} else if (!e.partition->instances_of("EmitterDocument").empty()) {
			c.documents.emplace_back(e.name);
		}
	}
	return c;
}

Result<std::string> resolve_name(EbxLibrary &r_lib, std::optional<Catalogue> &r_catalogue, const std::string &p_name, bool p_document) {
	const std::string name = to_lower_ascii(p_name);
	if (name.find('/') != std::string::npos && r_lib.partition_by_name(name) != nullptr) {
		return name;
	}
	if (!r_catalogue) {
		if (!r_lib.bulk_loaded()) {
			r_lib.load_all();
		}
		r_catalogue = catalogue_of(r_lib);
	}
	return find_partition(p_document ? r_catalogue->documents : r_catalogue->blueprints, name);
}

Result<EffectDef> compile_named(EbxLibrary &r_lib, Cache *p_cache, const std::string &p_name, bool p_emitter, ConvertTotals *r_totals) {
	if (p_emitter || p_cache == nullptr) {
		CompileContext ctx;
		ctx.db = &r_lib;
		return p_emitter ? compile_emitter_effect(p_name, ctx) : compile_effect(p_name, ctx);
	}
	FxSources sources;
	sources.db = &r_lib;
	sources.digest = gameplay::library_partition_digest(r_lib);
	Result<CachedEffect> c = compile_effect_cached(p_cache, sources, p_name, r_totals);
	if (!c) {
		return c.error();
	}
	return std::move(c->def);
}

void print_report(const HarnessReport &p_rep) {
	for (const std::string &l : p_rep.lines) {
		std::printf("%s\n", l.c_str());
	}
	for (const std::string &f : p_rep.failures) {
		std::printf("FAIL\t%s\n", f.c_str());
	}
}

void print_totals(const ConvertTotals &p_t) {
	std::fprintf(stderr, "cache: %llu items, %llu hits, %llu converted, %llu reused, %llu uncached, %llu failed, %llu dropped corrupt\n",
			static_cast<unsigned long long>(p_t.items.load()), static_cast<unsigned long long>(p_t.hits.load()),
			static_cast<unsigned long long>(p_t.converted.load()), static_cast<unsigned long long>(p_t.reused.load()),
			static_cast<unsigned long long>(p_t.uncached.load()), static_cast<unsigned long long>(p_t.failed.load()),
			static_cast<unsigned long long>(p_t.dropped_corrupt.load()));
}

std::string level_sb(const std::string &p_level) {
	return "Win32/Levels/" + p_level + "/" + p_level;
}

int fx_census(const EbxContext &p_ctx, const Args &p_a) {
	const std::string spec = p_a.positional.size() > 1 ? p_a.positional[1] : "@all-mp";
	unsigned threads = 0;
	if (!number_of(p_a, "--threads", threads)) {
		return 2;
	}
	std::unique_ptr<EbxLibrary> lib = open_library(p_ctx, spec, true);
	if (!lib) {
		return 1;
	}
	Result<CensusReport> r = run_census(*lib, threads);
	if (!r) {
		std::fprintf(stderr, "census: %s\n", r.error().message.c_str());
		return 1;
	}
	const CensusReport &c = *r;
	std::printf("census\t%s\n", spec.c_str());
	std::printf("blueprints\t%zu\tcompiled\t%zu\n", c.blueprints, c.blueprints_compiled);
	std::printf("emitter_documents\t%zu\tcompiled\t%zu\n", c.emitter_documents, c.emitter_documents_compiled);
	std::printf("emitter_entities\t%zu\n", c.emitter_entities);
	size_t processors_seen = 0, evaluators_seen = 0;
	for (size_t i = 0; i < kProcessorTypeCount; ++i) {
		std::printf("processor\t%.*s\t%zu\n", int(processor_type_name(ProcessorType(i)).size()), processor_type_name(ProcessorType(i)).data(),
				c.processor_emitters[i]);
		processors_seen += c.processor_emitters[i] > 0 ? 1 : 0;
	}
	for (size_t i = 0; i < kEvaluatorTypeCount; ++i) {
		std::printf("evaluator\t%.*s\t%zu\n", int(evaluator_type_name(EvaluatorType(i)).size()), evaluator_type_name(EvaluatorType(i)).data(),
				c.evaluator_emitters[i]);
		evaluators_seen += c.evaluator_emitters[i] > 0 ? 1 : 0;
	}
	std::printf("types_seen\tprocessors\t%zu/%zu\tevaluators\t%zu/%zu\n", processors_seen, kProcessorTypeCount, evaluators_seen, kEvaluatorTypeCount);
	for (const std::string &u : c.unknown_types) {
		std::printf("unknown_type\t%s\n", u.c_str());
	}
	for (const std::string &u : c.unknown_enumerators) {
		std::printf("unknown_enumerator\t%s\n", u.c_str());
	}
	for (const std::string &e : c.errors) {
		std::printf("error\t%s\n", e.c_str());
	}
	std::printf("dual_role_documents\t%zu\neffects_with_transform\t%zu\ncomponent_pools\t%zu\nflipbooks_normalised\t%zu\n", c.dual_role_documents,
			c.effects_with_transform, c.component_pools, c.flipbooks_normalised);
	std::printf("max_work_units\t%zu\tlimit\t%zu\n", c.max_work_units, kDefaultCompileWorkLimit / 16);
	std::printf("seconds\t%.3f\n", c.seconds);
	if (spec != "@all-mp") {
		return 0;
	}

	const bool pass = c.blueprints == 1101 && c.blueprints_compiled == 1101 && c.emitter_documents == 3410 && c.emitter_documents_compiled == 3410 &&
			c.unknown_types.empty() && c.unknown_enumerators.empty() && processors_seen == kProcessorTypeCount && evaluators_seen == kEvaluatorTypeCount &&
			c.max_work_units <= kDefaultCompileWorkLimit / 16;
	std::printf("G1\t%s\n", pass ? "PASS" : "FAIL");
	return pass ? 0 : 1;
}

int fx_compile(const EbxContext &p_ctx, const Args &p_a) {
	if (p_a.positional.size() < 2) {
		std::fprintf(stderr, "fx compile: partition missing\n");
		return 2;
	}
	CacheHandle cache = open_cache(p_ctx, p_a);
	if (!cache.ok) {
		return 1;
	}
	std::unique_ptr<EbxLibrary> lib = open_library(p_ctx, p_a.get("--sb", "@all-mp"), false);
	if (!lib) {
		return 1;
	}
	std::optional<Catalogue> catalogue;
	ConvertTotals totals;
	int rc = 0;
	for (size_t i = 1; i < p_a.positional.size(); ++i) {
		const std::string &want = p_a.positional[i];

		Result<std::string> name = resolve_name(*lib, catalogue, want, false);
		bool document = false;
		if (name) {
			const EbxPartition *p = lib->partition_by_name(*name);
			document = p != nullptr && p->primary().type_name() != "EffectBlueprint" && !p->instances_of("EmitterDocument").empty();
		} else {
			Result<std::string> doc = resolve_name(*lib, catalogue, want, true);
			if (!doc) {
				std::fprintf(stderr, "%s: %s\n", want.c_str(), name.error().message.c_str());
				rc = 1;
				continue;
			}
			name = std::move(doc);
			document = true;
		}
		if (document) {
			CompileContext ctx;
			ctx.db = lib.get();
			Result<std::vector<EmitterDef>> emitters = compile_emitter(*name, ctx);
			if (!emitters) {
				std::fprintf(stderr, "%s: %s\n", name->c_str(), emitters.error().message.c_str());
				rc = 1;
				continue;
			}
			for (const EmitterDef &e : *emitters) {
				const std::string text = format_emitter_def(e);
				std::fwrite(text.data(), 1, text.size(), stdout);
			}
			continue;
		}
		Result<EffectDef> def = compile_named(*lib, cache.cache.get(), *name, false, &totals);
		if (!def) {
			std::fprintf(stderr, "%s: %s\n", name->c_str(), def.error().message.c_str());
			rc = 1;
			continue;
		}
		const std::string text = format_effect_def(*def);
		std::fwrite(text.data(), 1, text.size(), stdout);
	}
	if (cache.cache) {
		print_totals(totals);
	}
	return rc;
}

int fx_simulate(const EbxContext &p_ctx, const Args &p_a) {
	const bool emitter = p_a.has("--emitter");
	const std::string want = emitter ? p_a.get("--emitter") : (p_a.positional.size() > 1 ? p_a.positional[1] : std::string());
	if (want.empty() || (!emitter && p_a.positional.size() != 2) || (emitter && p_a.positional.size() != 1)) {
		std::fprintf(stderr, "fx simulate: one effect or --emitter <document>\n");
		return 2;
	}
	HarnessRun run;
	uint32_t starts = 1;
	if (!number_of(p_a, "--seconds", run.seconds) || !number_of(p_a, "--dt", run.dt) || !number_of(p_a, "--seed", run.seed) ||
			!number_of(p_a, "--starts", starts)) {
		return 2;
	}
	run.variable = p_a.has("--variable");
	if (run.variable && p_a.options.contains("--dt")) {
		std::fprintf(stderr, "fx simulate: --dt and --variable exclude each other\n");
		return 2;
	}
	if (!(run.seconds > 0) || !(run.dt > 0) || starts == 0) {
		std::fprintf(stderr, "fx simulate: --seconds, --dt and --starts must be positive\n");
		return 2;
	}
	const std::string golden = p_a.get("--golden");
	if (!golden.empty() && golden != "muzzle1p" && golden != "oilfire") {
		std::fprintf(stderr, "fx simulate: --golden muzzle1p | oilfire\n");
		return 2;
	}
	CacheHandle cache = open_cache(p_ctx, p_a);
	if (!cache.ok) {
		return 1;
	}
	std::unique_ptr<EbxLibrary> lib = open_library(p_ctx, p_a.get("--sb", "@all-mp"), false);
	if (!lib) {
		return 1;
	}
	std::optional<Catalogue> catalogue;
	const Result<std::string> name = resolve_name(*lib, catalogue, want, emitter);
	if (!name) {
		std::fprintf(stderr, "%s: %s\n", want.c_str(), name.error().message.c_str());
		return 1;
	}
	ConvertTotals totals;
	Result<EffectDef> def = compile_named(*lib, cache.cache.get(), *name, emitter, &totals);
	if (!def) {
		std::fprintf(stderr, "%s: %s\n", name->c_str(), def.error().message.c_str());
		return 1;
	}
	const auto shared = std::make_shared<const EffectDef>(std::move(*def));
	const auto start = std::chrono::steady_clock::now();
	HarnessReport rep;
	if (golden == "muzzle1p") {
		rep = fx_golden_muzzle1p(shared, starts, run);
	} else if (golden == "oilfire") {
		rep = fx_golden_oilfire(shared, run);
	} else {
		rep = fx_simulate_starts(shared, starts, run);
	}
	print_report(rep);
	std::fprintf(stderr, "simulated in %.3f s\n", seconds_since(start));
	if (!golden.empty()) {
		std::printf("%s\t%s\n", golden == "muzzle1p" ? "G3" : "G4", rep.pass ? "PASS" : "FAIL");
	}
	return rep.pass ? 0 : 1;
}

int fx_check(const EbxContext &p_ctx, const Args &p_a) {
	const std::string spec = p_a.positional.size() > 1 ? p_a.positional[1] : "@all-mp";
	CheckOptions options;
	if (!number_of(p_a, "--seconds", options.seconds) || !number_of(p_a, "--seed", options.seed) || !number_of(p_a, "--threads", options.threads)) {
		return 2;
	}
	options.determinism = !p_a.has("--no-determinism");
	CacheHandle cache = open_cache(p_ctx, p_a);
	if (!cache.ok) {
		return 1;
	}
	std::unique_ptr<EbxLibrary> lib = open_library(p_ctx, spec, true);
	if (!lib) {
		return 1;
	}
	const Catalogue cat = catalogue_of(*lib);
	std::vector<std::shared_ptr<const EffectDef>> defs(cat.blueprints.size());
	std::vector<std::string> errors(cat.blueprints.size());
	ConvertTotals totals;
	const auto compile_start = std::chrono::steady_clock::now();
	parallel_for(
			cat.blueprints.size(),
			[&](size_t i) {
				Result<EffectDef> d = compile_named(*lib, cache.cache.get(), cat.blueprints[i], false, &totals);
				if (d) {
					defs[i] = std::make_shared<const EffectDef>(std::move(*d));
				} else {
					errors[i] = d.error().message;
				}
			},
			options.threads);
	size_t compile_failures = 0;
	std::vector<std::shared_ptr<const EffectDef>> ok;
	for (size_t i = 0; i < defs.size(); ++i) {
		if (defs[i]) {
			ok.push_back(defs[i]);
		} else {
			++compile_failures;
			std::printf("compile_error\t%s\t%s\n", cat.blueprints[i].c_str(), errors[i].c_str());
		}
	}
	std::fprintf(stderr, "compiled %zu effects in %.1f s\n", ok.size(), seconds_since(compile_start));
	const CheckReport rep = fx_check_effects(ok, options);
	for (const EffectCheck &e : rep.results) {
		for (const std::string &f : e.failures) {
			std::printf("FAIL\t%s\t%s\n", e.name.c_str(), f.c_str());
		}
	}
	for (const EffectCheck &e : rep.results) {
		if (e.unchecked_termination) {
			std::printf("unchecked_termination\t%s\n", e.name.c_str());
		}
	}
	std::printf("check\t%s\tseconds\t%g\tseed\t%llu\n", spec.c_str(), double(options.seconds), static_cast<unsigned long long>(options.seed));
	std::printf("effects\t%zu\tcompile_failures\t%zu\tfailed\t%zu\tfinite\t%zu\tunchecked_termination\t%zu\n", rep.effects, compile_failures,
			rep.failed_effects, rep.finite_effects, rep.unchecked_termination);
	std::printf("determinism\tcompared\t%zu\tmismatches\t%zu\tlanes\t%u\n", rep.determinism_compared, rep.determinism_mismatches, options.determinism_lanes);
	std::printf("frames\t%llu\tparticle_steps\t%llu\temitter_starts_dropped\t%llu\tmax_live\t%u\t%s\n", static_cast<unsigned long long>(rep.frames),
			static_cast<unsigned long long>(rep.particle_steps), static_cast<unsigned long long>(rep.emitter_starts_dropped), rep.max_live,
			rep.max_live_effect.c_str());
	std::printf("seconds\t%.1f\n", rep.seconds);
	if (cache.cache) {
		print_totals(totals);
	}
	const bool pass = compile_failures == 0 && rep.failed_effects == 0 && rep.determinism_mismatches == 0;
	std::printf("G7\t%s\n", pass ? "PASS" : "FAIL");
	return pass ? 0 : 1;
}

int fx_bench_command(const EbxContext &p_ctx, const Args &p_a) {
	uint64_t seed = 1;
	uint32_t runs = 5;
	if (!number_of(p_a, "--seed", seed) || !number_of(p_a, "--runs", runs)) {
		return 2;
	}
	if (runs == 0) {
		std::fprintf(stderr, "fx bench: --runs must be positive\n");
		return 2;
	}
	std::unique_ptr<EbxLibrary> lib = open_library(p_ctx, p_a.get("--sb", "@all-mp"), false);
	if (!lib) {
		return 1;
	}
	std::optional<Catalogue> catalogue;
	const Result<std::string> impact = resolve_name(*lib, catalogue, kImpactConcrete, false);
	if (!impact) {
		std::fprintf(stderr, "%s: %s\n", kImpactConcrete, impact.error().message.c_str());
		return 1;
	}
	Result<EffectDef> oil = compile_named(*lib, nullptr, kOilfire, true, nullptr);
	Result<EffectDef> muzzle = compile_named(*lib, nullptr, kMuzzle1p, false, nullptr);
	Result<EffectDef> imp = compile_named(*lib, nullptr, *impact, false, nullptr);
	if (!oil || !muzzle || !imp) {
		std::fprintf(stderr, "fx bench: %s\n", (!oil ? oil.error() : !muzzle ? muzzle.error() : imp.error()).message.c_str());
		return 1;
	}
	const BenchReport rep = fx_bench(*oil, *muzzle, *imp, seed, runs);
	std::printf("bench\tseed\t%llu\truns\t%u\tpools raised to %u instances (private copies; the bench measures particle cost)\n",
			static_cast<unsigned long long>(seed), runs, kMaxInstanceCount);
	for (size_t i = 0; i < rep.runs.size(); ++i) {
		const BenchRun &r = rep.runs[i];
		std::printf("run\t%zu\tns_per_particle\t%.2f\tmean_live\t%.0f\tmean_emitters\t%.0f\tadmitted\t%llu\ttimed_s\t%.3f\tspawns_dropped\t%llu\trefused_starts\t%llu\t"
					"stolen_instances\t%llu\tspawns_capped\t%llu\n",
				i, r.ns_per_particle, r.mean_live, r.mean_emitters, static_cast<unsigned long long>(r.admitted), r.timed_seconds,
				static_cast<unsigned long long>(r.stats.spawns_dropped), static_cast<unsigned long long>(r.stats.refused_starts),
				static_cast<unsigned long long>(r.stats.stolen_instances), static_cast<unsigned long long>(r.stats.spawns_capped));
	}
	std::printf("median_ns\t%.2f\tmin_ns\t%.2f\tmax_ns\t%.2f\tgate_ns\t%.1f\treference_ns\t%.1f\n", rep.median_ns, rep.min_ns, rep.max_ns, kBenchGateNs,
			kBenchReferenceNs);
	if (!rep.valid) {
		std::printf("invalid\t%s\n", rep.invalid_reason.c_str());
	}
	const bool pass = rep.valid && rep.median_ns <= kBenchGateNs;
	std::printf("G9\t%s\n", pass ? "PASS" : "FAIL");
	return pass ? 0 : 1;
}

int fx_atlas(const EbxContext &p_ctx, const Args &p_a) {
	if (p_a.positional.size() != 2) {
		std::fprintf(stderr, "fx atlas <LEVEL>\n");
		return 2;
	}
	const std::string level = p_a.positional[1];
	std::unique_ptr<EbxLibrary> lib = open_library(p_ctx, level_sb(level), true);
	if (!lib) {
		return 1;
	}
	const auto start = std::chrono::steady_clock::now();
	gameplay::WorkBudget budget{ kDefaultCompileWorkLimit };
	Result<EmitterAtlas> atlas = EmitterAtlas::load(*lib, lib->mount(0), EmitterAtlas::emitter_system_of(level), budget);
	if (!atlas) {
		std::fprintf(stderr, "atlas: %s\n", atlas.error().message.c_str());
		return 1;
	}
	std::fprintf(stderr, "atlas loaded in %.3f s\n", seconds_since(start));
	const Catalogue cat = catalogue_of(*lib);
	std::set<std::string> used;
	std::atomic<size_t> failed{ 0 };
	std::vector<std::vector<std::string>> names(cat.documents.size());
	parallel_for(cat.documents.size(), [&](size_t i) {
		CompileContext ctx;
		ctx.db = lib.get();
		const Result<std::vector<EmitterDef>> e = compile_emitter(cat.documents[i], ctx);
		if (!e) {
			++failed;
			return;
		}
		for (const EmitterDef &d : *e) {
			if (!d.texture.texture.empty()) {
				names[i].push_back(d.texture.texture);
			}
			if (!d.texture.normal_map.empty()) {
				names[i].push_back(d.texture.normal_map);
			}
		}
	});
	for (const std::vector<std::string> &n : names) {
		used.insert(n.begin(), n.end());
	}
	HarnessReport rep = fx_golden_atlas(*atlas, level, used, cat.documents.size());
	if (failed > 0) {
		rep.failures.push_back(std::to_string(failed.load()) + " EmitterDocuments did not compile");
		rep.pass = false;
	}
	print_report(rep);
	std::printf("G6\t%s\n", rep.pass ? "PASS" : "FAIL");
	return rep.pass ? 0 : 1;
}

int fx_impacts(const EbxContext &p_ctx, const Args &p_a) {
	if (p_a.positional.size() != 2) {
		std::fprintf(stderr, "fx impacts <LEVEL> [--projectile=bullet|<material index>]\n");
		return 2;
	}
	const std::string level = p_a.positional[1];
	const std::string projectile = p_a.get("--projectile", "bullet");
	Result<gameplay::WeaponSources> sources = gameplay::open_weapon_sources(p_ctx.install, level_sb(level));
	if (!sources) {
		std::fprintf(stderr, "%s\n", sources.error().message.c_str());
		return 1;
	}
	const std::optional<std::string> grid = gameplay::MaterialGrid::grid_of_level(*sources->level, sources->level_partition);
	if (!grid) {
		std::fprintf(stderr, "%s: no RuntimeMaterialGrid\n", sources->level_partition.c_str());
		return 1;
	}
	const auto start = std::chrono::steady_clock::now();
	gameplay::WorkBudget budget{ kDefaultCompileWorkLimit };
	Result<ImpactTable> table = ImpactTable::load(*sources->level, *grid, budget);
	if (!table) {
		std::fprintf(stderr, "impacts: %s\n", table.error().message.c_str());
		return 1;
	}
	std::fprintf(stderr, "impact table %s loaded in %.3f s\n", grid->c_str(), seconds_since(start));
	uint8_t material = 0;
	if (projectile == "bullet") {
		const std::optional<uint8_t> m = table->material_by_name("Bullet");
		if (!m) {
			std::fprintf(stderr, "impacts: no material named Bullet\n");
			return 1;
		}
		material = *m;
	} else {
		unsigned v = 0;
		const auto res = std::from_chars(projectile.data(), projectile.data() + projectile.size(), v);
		if (res.ec != std::errc() || res.ptr != projectile.data() + projectile.size() || v > 255) {
			std::fprintf(stderr, "--projectile: bullet or a material index 0..255\n");
			return 2;
		}
		material = uint8_t(v);
	}
	const HarnessReport rep = fx_golden_impacts(*table, level, material);
	print_report(rep);
	std::printf("G5\t%s\n", rep.pass ? "PASS" : "FAIL");
	return rep.pass ? 0 : 1;
}

}

const char *fx_usage() {
	return "  fx census [<sb>|@all-mp] [--threads N]\n"
		   "  fx compile <partition>... [--sb <spec>] [--cache <root>]\n"
		   "  fx simulate (<effect> | --emitter <document>) [--seconds S] [--dt D | --variable] [--seed N] [--starts K]\n"
		   "              [--golden muzzle1p|oilfire] [--sb <spec>] [--cache <root>]\n"
		   "  fx check [<sb>|@all-mp] [--seconds S] [--seed N] [--threads N] [--no-determinism] [--cache <root>]\n"
		   "  fx bench [--seed N] [--runs 5]\n"
		   "  fx atlas <LEVEL>\n"
		   "  fx impacts <LEVEL> [--projectile=bullet|<material index>]\n";
}

int cmd_fx(const EbxContext &p_ctx, const std::vector<std::string> &p_args) {
	const Args a = parse(p_args, { "--threads", "--seconds", "--dt", "--seed", "--starts", "--golden", "--emitter", "--runs", "--cache", "--sb", "--projectile" },
			{ "--variable", "--no-determinism" });
	if (!a.error.empty() || a.positional.empty()) {
		std::fprintf(stderr, "fx: %s\n%s", a.error.empty() ? "subcommand missing" : a.error.c_str(), fx_usage());
		return 2;
	}
	const std::string &sub = a.positional[0];
	if (sub == "census") {
		return fx_census(p_ctx, a);
	}
	if (sub == "compile") {
		return fx_compile(p_ctx, a);
	}
	if (sub == "simulate") {
		return fx_simulate(p_ctx, a);
	}
	if (sub == "check") {
		return fx_check(p_ctx, a);
	}
	if (sub == "bench") {
		return fx_bench_command(p_ctx, a);
	}
	if (sub == "atlas") {
		return fx_atlas(p_ctx, a);
	}
	if (sub == "impacts") {
		return fx_impacts(p_ctx, a);
	}
	std::fprintf(stderr, "fx: unknown subcommand %s\n%s", sub.c_str(), fx_usage());
	return 2;
}

}
