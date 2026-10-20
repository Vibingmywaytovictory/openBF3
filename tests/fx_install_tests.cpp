#include <doctest/doctest.h>

#include "bf3/core/cache/disk_cache.h"
#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/material_grid.h"
#include "bf3/core/gameplay/weapon_blob.h"
#include "bf3/core/gameplay/weapon_catalog.h"
#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/install_snapshot.h"
#include "bf3/core/thread_pool.h"
#include "bf3/fx/effect_cache.h"
#include "bf3/fx/effect_compiler.h"
#include "bf3/fx/emitter_atlas.h"
#include "bf3/fx/fx_harness.h"
#include "bf3/fx/impact_table.h"
#include "test_support.h"

#include <windows.h>

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

using namespace bf3::core;
using namespace bf3::fx;

namespace {

constexpr const char *kMuzzle = "fx/weapons/muzzleflashes/generic/fx_muzz_generic_rifle_1p";
constexpr const char *kOilfire = "fx/ambient/levelspecific/mp_12/emitters_oilfire/em_amb_mp12_oilfire_l_fire_01";

std::optional<InstallSnapshot> g_before;

struct Session {
	InstallReader reader;
	std::shared_ptr<Install> install;
	std::unique_ptr<EbxLibrary> all;
};

bool open_or_skip(Session &r_s, bool p_all = true) {
	if (!r_s.reader.locate({})) {
		bf3::test::skip_install_case(r_s.reader.describe().c_str());
		return false;
	}
	if (!g_before) {
		g_before = r_s.reader.snapshot();
	}
	Result<std::shared_ptr<Install>> install = Install::open(r_s.reader);
	REQUIRE_MESSAGE(install.ok(), (install.ok() ? std::string() : install.error().message));
	r_s.install = *install;
	if (p_all) {
		Result<std::unique_ptr<EbxLibrary>> lib = EbxLibrary::open(r_s.install, expand_superbundles(*r_s.install, "@all-mp"));
		REQUIRE_MESSAGE(lib.ok(), (lib.ok() ? std::string() : lib.error().message));
		r_s.all = std::move(*lib);
		r_s.all->load_all();
	}
	return true;
}

Session *all_mp() {
	static std::unique_ptr<Session> s_session;
	static bool s_tried = false;
	if (!s_tried) {
		s_tried = true;
		auto made = std::make_unique<Session>();
		if (open_or_skip(*made)) {
			s_session = std::move(made);
		}
		return s_session.get();
	}
	if (s_session == nullptr) {
		bf3::test::skip_install_case("no BF3 install (see the first fx-install case)");
	}
	return s_session.get();
}

void print_failures(const HarnessReport &p_rep) {
	for (const std::string &f : p_rep.failures) {
		MESSAGE(f);
	}
}

std::vector<std::string> blueprints_of(const EbxLibrary &p_lib) {
	std::vector<std::string> out;
	for (const EbxLibrary::Entry &e : p_lib.entries()) {
		if (e.partition->primary().type_name() == "EffectBlueprint") {
			out.emplace_back(e.name);
		}
	}
	return out;
}

}

TEST_SUITE("fx-install") {

TEST_CASE("census: G1, every blueprint and emitter document compiles") {
	Session *shared = all_mp();
	if (shared == nullptr) {
		return;
	}
	Session &s = *shared;
	const Result<CensusReport> r = run_census(*s.all);
	REQUIRE(r.ok());
	for (const std::string &e : r->errors) {
		MESSAGE(e);
	}
	CHECK(r->blueprints == 1101);
	CHECK(r->blueprints_compiled == 1101);
	CHECK(r->emitter_documents == 3410);
	CHECK(r->emitter_documents_compiled == 3410);
	CHECK(r->unknown_types.empty());
	CHECK(r->unknown_enumerators.empty());
	for (size_t i = 0; i < kProcessorTypeCount; ++i) {
		CHECK_MESSAGE(r->processor_emitters[i] > 0, processor_type_name(ProcessorType(i)));
	}
	for (size_t i = 0; i < kEvaluatorTypeCount; ++i) {
		CHECK_MESSAGE(r->evaluator_emitters[i] > 0, evaluator_type_name(EvaluatorType(i)));
	}
	CHECK(r->max_work_units <= kDefaultCompileWorkLimit / 16);
	CHECK(r->effects_with_transform == 21);
	CHECK(r->flipbooks_normalised == 1);
	CHECK(r->dual_role_documents == 0);
	MESSAGE("census seconds " << r->seconds << ", max work units " << r->max_work_units);
}

TEST_CASE("muzzle: G3, fx_muzz_generic_rifle_1p over 10,000 starts") {
	Session *shared = all_mp();
	if (shared == nullptr) {
		return;
	}
	Session &s = *shared;
	CompileContext ctx;
	ctx.db = s.all.get();
	Result<EffectDef> d = compile_effect(kMuzzle, ctx);
	REQUIRE(d.ok());
	CHECK(d->max_instance_count == 7);
	CHECK(d->components.size() == 5);
	HarnessRun run;
	run.seed = 1;
	run.seconds = 1;
	run.dt = 0.0166667f;
	const HarnessReport rep = fx_golden_muzzle1p(std::make_shared<const EffectDef>(std::move(*d)), 10000, run);
	print_failures(rep);
	CHECK(rep.pass);
}

TEST_CASE("oilfire: G4, the wrapped fire_01 holds 79..81 live, fixed and variable dt") {
	Session *shared = all_mp();
	if (shared == nullptr) {
		return;
	}
	Session &s = *shared;
	CompileContext ctx;
	ctx.db = s.all.get();
	Result<EffectDef> d = compile_emitter_effect(kOilfire, ctx);
	REQUIRE(d.ok());
	const auto def = std::make_shared<const EffectDef>(std::move(*d));
	for (const bool variable : { false, true }) {
		HarnessRun run;
		run.seed = 1;
		run.seconds = 75;
		run.dt = 0.0166667f;
		run.variable = variable;
		const HarnessReport rep = fx_golden_oilfire(def, run);
		print_failures(rep);
		CHECK_MESSAGE(rep.pass, (variable ? "variable dt" : "fixed dt"));
	}
}

TEST_CASE("impacts: G5, Metro and Caspian under the counting rules of section 5.3") {
	Session s;
	if (!open_or_skip(s, false)) {
		return;
	}
	for (const char *level : { "MP_Subway", "MP_007" }) {
		Result<gameplay::WeaponSources> src = gameplay::open_weapon_sources(s.install, std::string("Win32/Levels/") + level + "/" + level);
		REQUIRE(src.ok());
		const std::optional<std::string> grid = gameplay::MaterialGrid::grid_of_level(*src->level, src->level_partition);
		REQUIRE(grid.has_value());
		gameplay::WorkBudget budget{ kDefaultCompileWorkLimit };
		Result<ImpactTable> table = ImpactTable::load(*src->level, *grid, budget);
		REQUIRE(table.ok());
		const std::optional<uint8_t> bullet = table->material_by_name("Bullet");
		REQUIRE(bullet.has_value());
		CHECK(*bullet == 1);
		const HarnessReport rep = fx_golden_impacts(*table, level, *bullet);
		print_failures(rep);
		CHECK_MESSAGE(rep.pass, level);
	}
}

TEST_CASE("atlas: G6, Metro's emitter atlas and every name its documents use") {
	Session s;
	if (!open_or_skip(s, false)) {
		return;
	}
	Result<std::unique_ptr<EbxLibrary>> lib = EbxLibrary::open(s.install, { "Win32/Levels/MP_Subway/MP_Subway" });
	REQUIRE(lib.ok());
	(*lib)->load_all();
	gameplay::WorkBudget budget{ kDefaultCompileWorkLimit };
	Result<EmitterAtlas> atlas = EmitterAtlas::load(**lib, (*lib)->mount(0), EmitterAtlas::emitter_system_of("MP_Subway"), budget);
	REQUIRE(atlas.ok());
	std::vector<std::string> docs;
	for (const EbxLibrary::Entry &e : (*lib)->entries()) {
		if (e.partition->primary().type_name() != "EffectBlueprint" && !e.partition->instances_of("EmitterDocument").empty()) {
			docs.emplace_back(e.name);
		}
	}
	std::set<std::string> used;
	size_t failed = 0;
	for (const std::string &d : docs) {
		CompileContext ctx;
		ctx.db = lib->get();
		const Result<std::vector<EmitterDef>> e = compile_emitter(d, ctx);
		if (!e) {
			++failed;
			continue;
		}
		for (const EmitterDef &x : *e) {
			if (!x.texture.texture.empty()) {
				used.insert(x.texture.texture);
			}
			if (!x.texture.normal_map.empty()) {
				used.insert(x.texture.normal_map);
			}
		}
	}
	CHECK(failed == 0);
	const HarnessReport rep = fx_golden_atlas(*atlas, "MP_Subway", used, docs.size());
	print_failures(rep);
	CHECK(rep.pass);

	const AtlasEntry *flash = atlas->resolve("fx/textures/muzzleflash/muzzleflash_01");
	REQUIRE(flash != nullptr);
	Vec2 mn, mx;
	EmitterAtlas::frame_rect(*flash, 5, 16, 4, mn, mx);
	CHECK(mn.x == doctest::Approx(flash->min_uv.x + (flash->max_uv.x - flash->min_uv.x) * 0.25f));
	CHECK(mn.y == doctest::Approx(flash->min_uv.y + (flash->max_uv.y - flash->min_uv.y) * 0.25f));
}

TEST_CASE("cache: cold and warm compiles give the same defs") {
	Session *shared = all_mp();
	if (shared == nullptr) {
		return;
	}
	Session &s = *shared;
	const std::filesystem::path root =
			std::filesystem::temp_directory_path() / ("openbf3_fx_install_cache_" + std::to_string(GetCurrentProcessId()));
	std::error_code ec;
	std::filesystem::remove_all(root, ec);
	{
		Cache cache;
		CacheOptions o;
		o.root = root;
		o.guard = s.reader.write_guard();
		REQUIRE(cache.open(std::move(o)) == CacheStatus::Ok);
		FxSources src;
		src.db = s.all.get();
		src.digest = gameplay::library_partition_digest(*s.all);
		const std::vector<std::string> names = blueprints_of(*s.all);
		REQUIRE(names.size() == 1101);
		std::vector<std::string> cold(names.size()), warm(names.size());
		std::vector<int> warm_hit(names.size(), 0);
		ConvertTotals cold_totals, warm_totals;
		parallel_for(names.size(), [&](size_t i) {
			const Result<CachedEffect> c = compile_effect_cached(&cache, src, names[i], &cold_totals);
			if (c) {
				cold[i] = format_effect_def(c->def);
			}
		});
		parallel_for(names.size(), [&](size_t i) {
			const Result<CachedEffect> c = compile_effect_cached(&cache, src, names[i], &warm_totals);
			if (c) {
				warm[i] = format_effect_def(c->def);
				warm_hit[i] = c->item.outcome == CacheOutcome::Hit ? 1 : 0;
			}
		});
		size_t differ = 0, hits = 0;
		for (size_t i = 0; i < names.size(); ++i) {
			differ += (cold[i].empty() || cold[i] != warm[i]) ? 1 : 0;
			hits += size_t(warm_hit[i]);
		}
		CHECK(differ == 0);
		CHECK(hits == names.size());
		CHECK(cold_totals.failed.load() == 0);

		CompileContext ctx;
		ctx.db = s.all.get();
		const Result<EffectDef> direct = compile_effect(kMuzzle, ctx);
		REQUIRE(direct.ok());
		for (size_t i = 0; i < names.size(); ++i) {
			if (names[i] == kMuzzle) {
				CHECK(warm[i] == format_effect_def(*direct));
			}
		}
	}
	std::filesystem::remove_all(root, ec);
}

TEST_CASE("check: G7 and G8 over every effect, 10 s, seed 1") {
	Session *shared = all_mp();
	if (shared == nullptr) {
		return;
	}
	Session &s = *shared;
	const std::vector<std::string> names = blueprints_of(*s.all);
	std::vector<std::shared_ptr<const EffectDef>> defs(names.size());
	parallel_for(names.size(), [&](size_t i) {
		CompileContext ctx;
		ctx.db = s.all.get();
		Result<EffectDef> d = compile_effect(names[i], ctx);
		if (d) {
			defs[i] = std::make_shared<const EffectDef>(std::move(*d));
		}
	});
	size_t missing = 0;
	std::vector<std::shared_ptr<const EffectDef>> ok;
	for (const auto &d : defs) {
		if (d) {
			ok.push_back(d);
		} else {
			++missing;
		}
	}
	CHECK(missing == 0);
	CheckOptions options;
	options.seconds = 10;
	options.seed = 1;
	const CheckReport rep = fx_check_effects(ok, options);
	size_t shown = 0;
	for (const EffectCheck &e : rep.results) {
		for (const std::string &f : e.failures) {
			if (shown++ < 20) {
				MESSAGE(e.name << ": " << f);
			}
		}
	}
	CHECK(rep.effects == 1101);
	CHECK(rep.failed_effects == 0);
	CHECK(rep.determinism_compared == 1101);
	CHECK(rep.determinism_mismatches == 0);
	MESSAGE("check: " << rep.finite_effects << " finite, " << rep.unchecked_termination << " unchecked termination, " << rep.seconds << " s");
}

TEST_CASE("install unchanged: G11") {
	Session s;
	if (!open_or_skip(s, false)) {
		return;
	}
	REQUIRE(g_before.has_value());
	const SnapshotDiff diff = diff_snapshots(*g_before, s.reader.snapshot());
	CHECK_MESSAGE(diff.clean(), diff.summary());
}

}
