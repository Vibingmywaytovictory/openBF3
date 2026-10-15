// M2a VFX1 fx.unit (docs/design/m2a-vfx1.md §11): the compiler on synthetic EBX (fx_test_support.h), the blob and the
// two cache rules of §8 through a temp Cache root and an in-memory resolver, and the simulator's spawn / age / pool /
// child / transform / determinism rules on hand-built defs. No install, no Godot.
#include <doctest/doctest.h>

#include "bf3/core/cache/disk_cache.h"
#include "bf3/core/path_guard.h"
#include "bf3/fx/effect_cache.h"
#include "bf3/fx/effect_compiler.h"
#include "bf3/fx/fx_harness.h"
#include "bf3/fx/simulator.h"
#include "fx_test_support.h"
#include "test_support.h"

#include <windows.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

using namespace bf3::fx;
using fxt::DocBuilder;
using fxt::EffectBuilder;
using fxt::f32;
using fxt::flag;
using fxt::i32;
using fxt::raw;
using fxt::v3;
using fxt::v4;

namespace {

constexpr uint32_t kNormTime = 1, kEmitterNormTime = 2, kOne = 3;

// A document with the usual chain: SpawnRate, UpdateAge, UpdateLinearVelocity, UpdateTransparency (poly Pre).
DocBuilder simple_doc(uint32_t p_seed, uint32_t p_max_count = 8, float p_rate = 10, float p_life = 1, float p_window = 0) {
	DocBuilder d(p_seed, p_max_count, p_window);
	d.proc("SpawnRateData", { { "SpawnRate", f32(p_rate) }, { "DistributeOverTime", flag(true) } }, kOne);
	d.proc("UpdateAgeData", { { "Lifetime", f32(p_life) } }, kNormTime);
	d.proc("UpdateLinearVelocityData");
	const uint32_t tr = d.proc("UpdateTransparencyData", { { "CullThreshold", f32(0) } });
	d.pre(tr, "PolynomialData", DocBuilder::poly(0, -1.9477582f, 0.95294344f, 1.0f));
	return d;
}

void run_frames(FxWorld &r_world, float p_dt, uint32_t p_frames) {
	for (uint32_t i = 0; i < p_frames; ++i) {
		fx_step_frame(r_world, p_dt);
	}
}

EffectStart start_with(uint64_t p_seed) {
	EffectStart s;
	s.seed = p_seed;
	return s;
}

// A temp Cache root removed at the end of the case.
struct TempCache {
	std::filesystem::path root;
	std::filesystem::path fence;
	bf3::core::Cache cache;
	bool open = false;

	explicit TempCache(const char *p_name) {
		const std::filesystem::path base = std::filesystem::temp_directory_path() / ("openbf3_fx_" + std::string(p_name) + "_" + std::to_string(GetCurrentProcessId()));
		std::error_code ec;
		std::filesystem::remove_all(base, ec);
		std::filesystem::create_directories(base / "fence", ec);
		root = base / "cache";
		fence = base / "fence";
		bf3::core::CacheOptions o;
		o.root = root;
		o.guard.add_protected_root(fence); // stands in for the install
		std::string detail;
		open = cache.open(std::move(o), &detail) == bf3::core::CacheStatus::Ok;
	}
	~TempCache() {
		cache = bf3::core::Cache();
		std::error_code ec;
		std::filesystem::remove_all(root.parent_path(), ec);
	}
};

FxSources sources_of(const fxt::MemResolver &p_db) {
	FxSources s;
	s.db = &p_db;
	s.digest = [&p_db](std::string_view p_name) { return p_db.digest(p_name); };
	return s;
}

} // namespace

TEST_SUITE("fx-unit") {

// ---- compiler ----------------------------------------------------------------------------------------------------

TEST_CASE("compile: a synthetic blueprint with an emitter, a child and a sound") {
	fxt::MemResolver db;
	DocBuilder child = simple_doc(20, 4, 20, 0.25f);
	REQUIRE(db.set("fx/test/child", child.p));
	DocBuilder parent = simple_doc(10, 8, 10, 1);
	const uint32_t em = parent.proc("EmitterData", {});
	{
		fxt::Array a;
		a.words.push_back(parent.p.ext(20, 1).word);
		fxt::set(parent.p.instances[em].values, "EmitterAssets", parent.p.array(std::move(a)));
	}
	const uint32_t tc = parent.proc("UpdateTextureCoordsData",
			{ { "TextureBlendValue", f32(1) },
					{ "TextureInfo1",
							fxt::st({ { "TextureName", parent.p.str("FX/Textures/Test_01") }, { "NormalmapName", parent.p.str("systems/emitter/defaultnormalmap") },
									{ "EnableAnimation", flag(true) }, { "AnimationFrameCount", f32(16) }, { "AnimationFrameColumnCount", f32(4) } }) } });
	(void)tc;
	REQUIRE(db.set("fx/test/parent", parent.p));
	REQUIRE(db.set("sound/test/patch", fxt::target(30)));
	EffectBuilder fx(40, 3);
	fx.emitter(10, 0.5f);
	fx.sound(30);
	REQUIRE(db.set("fx/test/effect", fx.done()));

	CompileContext ctx;
	ctx.db = &db;
	const bf3::core::Result<EffectDef> d = compile_effect("fx/test/effect", ctx);
	REQUIRE_MESSAGE(d.ok(), (d.ok() ? std::string() : d.error().message));
	CHECK(d->max_instance_count == 3);
	REQUIRE(d->components.size() == 2);
	CHECK(d->components[0].kind == ComponentKind::Emitter);
	CHECK(d->components[0].spawn_probability == 0.5f);
	CHECK(d->components[1].kind == ComponentKind::Sound);
	CHECK(d->components[1].asset == "sound/test/patch");
	REQUIRE(d->emitters.size() == 2);
	const EmitterDef &p = d->emitters[0];
	CHECK(p.name == "fx/test/parent");
	CHECK(p.tmpl.max_count == 8);
	CHECK(p.has(ProcessorType::SpawnRate));
	CHECK(p.params.spawn_rate == 10.0f);
	CHECK(p.params.particle_lifetime == 1.0f);
	CHECK(p.texture.texture == "fx/textures/test_01");
	CHECK(p.texture.frame_count == 16);
	CHECK(p.texture.frame_columns == 4);
	REQUIRE(p.children.size() == 1);
	CHECK(d->emitters[p.children[0]].name == "fx/test/child");
	CHECK((d->emitters[p.children[0]].tmpl.flags & kEmitterChild) != 0);
	CHECK(d->max_particles == 8 * (1 + 4));
	const ProcessorSlot &tr = p.slot(ProcessorType::UpdateTransparency);
	REQUIRE(tr.evaluator != kNoEvaluator);
	const EvaluatorDef &ev = p.evaluators[size_t(tr.evaluator)];
	CHECK(ev.type == EvaluatorType::Polynomial);
	// G2 on the compiled evaluator: f(0) = w and f(1) = x + y + z + w exactly; the 1p flash alpha 1 -> 0.0051852.
	CHECK(evaluate_polynomial(ev.polynomial, 0.0f) == 1.0f);
	CHECK(evaluate_polynomial(ev.polynomial, 1.0f) == doctest::Approx(0.00518524).epsilon(1e-4));
	// Every partition looked up is a dependency.
	const std::set<std::string> deps(ctx.sources.begin(), ctx.sources.end());
	CHECK(deps.contains("fx/test/effect"));
	CHECK(deps.contains("fx/test/parent"));
	CHECK(deps.contains("fx/test/child"));
	CHECK(deps.contains("sound/test/patch"));
	// Blob round trip: the canonical text and the bytes.
	const std::vector<uint8_t> blob = encode_effect_def(*d);
	const bf3::core::Result<EffectDef> back = decode_effect_def(blob);
	REQUIRE(back.ok());
	CHECK(format_effect_def(*back) == format_effect_def(*d));
	CHECK(encode_effect_def(*back) == blob);
	std::vector<uint8_t> longer = blob;
	longer.push_back(0);
	CHECK_FALSE(decode_effect_def(longer).ok());
	// The lone document wrapped (MaxInstanceCount 1, one component).
	CompileContext c2;
	c2.db = &db;
	const bf3::core::Result<EffectDef> w = compile_emitter_effect("fx/test/parent", c2);
	REQUIRE(w.ok());
	CHECK(w->components.size() == 1);
	CHECK(w->max_instance_count == 1);
	CHECK(w->max_particles == 40);
}

TEST_CASE("compile: G2 clamp then scale, colour interpolation endpoints") {
	fxt::MemResolver db;
	DocBuilder d(1, 6, 0.1f);
	d.proc("SpawnRateData", { { "SpawnRate", f32(100) }, { "DistributeOverTime", flag(true) } }, kOne);
	const uint32_t tr = d.proc("UpdateTransparencyData", { { "CullThreshold", f32(0) } });
	d.pre(tr, "PolynomialData", DocBuilder::poly(0, -1.117318f, 0, 1.1179048f, 0, 1, 0.1f));
	const uint32_t col = d.proc("UpdateColorData", { { "Color", v3(1, 1, 1) } });
	d.pre(col, "PolynomialColorInterpData", { { "Coefficients", v4(0, 0, 1, 0) }, { "Color0", v3(10, 20, 30) }, { "Color1", v3(1, 2, 3) } });
	REQUIRE(db.set("fx/test/rapid", d.p));
	CompileContext ctx;
	ctx.db = &db;
	const bf3::core::Result<std::vector<EmitterDef>> e = compile_emitter("fx/test/rapid", ctx);
	REQUIRE(e.ok());
	const EmitterDef &em = e->front();
	const EvaluatorDef &a = em.evaluators[size_t(em.slot(ProcessorType::UpdateTransparency).evaluator)];
	CHECK(evaluate_polynomial(a.polynomial, 0.0f) == doctest::Approx(0.1f).epsilon(1e-6)); // clamp to 1, then x 0.1
	const EvaluatorDef &c = em.evaluators[size_t(em.slot(ProcessorType::UpdateColor).evaluator)];
	const Vec3 at0 = evaluate_color_interp(c.color_interp.coefficients, c.color_interp.color0, c.color_interp.color1, 0.0f);
	const Vec3 at1 = evaluate_color_interp(c.color_interp.coefficients, c.color_interp.color0, c.color_interp.color1, 1.0f);
	CHECK(at0.x == 1.0f); // f = 0: Color1
	CHECK(at1.z == 30.0f); // f = 1: Color0
}

TEST_CASE("compile: unknown types, unknown enumerators, cycles, base emitters, flipbooks") {
	fxt::MemResolver db;
	{
		DocBuilder d = simple_doc(1);
		d.proc("MysteryProcessorData");
		REQUIRE(db.set("fx/test/mystery", d.p));
		CompileContext ctx;
		ctx.db = &db;
		CHECK_FALSE(compile_emitter("fx/test/mystery", ctx).ok());
		CHECK(ctx.unknown_types == std::vector<std::string>{ "MysteryProcessorData" });
	}
	{
		DocBuilder d = simple_doc(2);
		fxt::set(d.tmpl(), "EmittableType", raw(7)); // "Ribbon"
		REQUIRE(db.set("fx/test/ribbon", d.p));
		CompileContext ctx;
		ctx.db = &db;
		CHECK_FALSE(compile_emitter("fx/test/ribbon", ctx).ok());
		REQUIRE(ctx.unknown_enumerators.size() == 1);
		CHECK(ctx.unknown_enumerators[0].find("Ribbon") != std::string::npos);
	}
	{
		DocBuilder d = simple_doc(3);
		fxt::set(d.p.instances[d.last].values, "NextProcessor", d.p.ref(2)); // back to the first processor
		REQUIRE(db.set("fx/test/cycle", d.p));
		CompileContext ctx;
		ctx.db = &db;
		const auto r = compile_emitter("fx/test/cycle", ctx);
		REQUIRE_FALSE(r.ok());
		CHECK(r.error().message.find("cycle") != std::string::npos);
	}
	{
		// A document whose chain is only BaseEmitterData takes its base's processors (docs §6.2).
		REQUIRE(db.set("fx/test/base", simple_doc(4, 5, 7, 2).p));
		DocBuilder d(5, 9);
		d.proc("BaseEmitterData", { { "EmitterAsset", d.p.ext(4, 1) } });
		REQUIRE(db.set("fx/test/derived", d.p));
		CompileContext ctx;
		ctx.db = &db;
		const auto r = compile_emitter("fx/test/derived", ctx);
		REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
		CHECK(r->front().base == "fx/test/base");
		CHECK((r->front().tmpl.flags & kEmitterDerived) != 0);
		CHECK(r->front().tmpl.max_count == 9); // the template is the document's own
		CHECK(r->front().params.spawn_rate == 7.0f);
	}
	{
		// Flipbook counts: 0 is stored as 1 and counted; a fraction is an error.
		for (const float frames : { 0.0f, 2.5f }) {
			DocBuilder d = simple_doc(6);
			d.proc("UpdateTextureCoordsData",
					{ { "TextureBlendValue", f32(1) },
							{ "TextureInfo1", fxt::st({ { "AnimationFrameCount", f32(frames) }, { "AnimationFrameColumnCount", f32(0) } }) } });
			REQUIRE(db.set("fx/test/flipbook", d.p));
			CompileContext ctx;
			ctx.db = &db;
			const auto r = compile_emitter("fx/test/flipbook", ctx);
			if (frames == 0) {
				REQUIRE(r.ok());
				CHECK(r->front().texture.frame_count == 1);
				CHECK(ctx.stats.flipbooks_normalised == 1);
			} else {
				CHECK_FALSE(r.ok());
			}
		}
	}
}

TEST_CASE("find_partition: unique suffix, none, ambiguous") {
	const std::vector<std::string> cat{ "fx/a/fx_one", "fx/b/fx_two", "fx/c/fx_two", "fx/d/xfx_one" };
	CHECK(find_partition(cat, "fx_one").value() == "fx/a/fx_one");
	CHECK(find_partition(cat, "FX/A/FX_ONE").value() == "fx/a/fx_one");
	CHECK_FALSE(find_partition(cat, "fx_three").ok());
	const auto amb = find_partition(cat, "fx_two");
	REQUIRE_FALSE(amb.ok());
	CHECK(amb.error().message.find("fx/b/fx_two") != std::string::npos);
}

// ---- cache -------------------------------------------------------------------------------------------------------

TEST_CASE("cache: cold, warm, a patched document, a patched blueprint") {
	TempCache tc("unit_cache");
	REQUIRE(tc.open);
	fxt::MemResolver db;
	REQUIRE(db.set("fx/test/a", simple_doc(1, 8, 10).p));
	REQUIRE(db.set("fx/test/b", simple_doc(2, 3, 5).p));
	EffectBuilder fx(9);
	fx.emitter(1);
	REQUIRE(db.set("fx/test/effect", fx.done()));
	const FxSources src = sources_of(db);

	const auto cold = compile_effect_cached(&tc.cache, src, "fx/test/effect");
	REQUIRE(cold.ok());
	CHECK(cold->item.outcome == bf3::core::CacheOutcome::Converted);
	const auto warm = compile_effect_cached(&tc.cache, src, "fx/test/effect");
	REQUIRE(warm.ok());
	CHECK(warm->item.outcome == bf3::core::CacheOutcome::Hit);
	CHECK(format_effect_def(warm->def) == format_effect_def(cold->def));

	// A patched emitter document changes the effect key (its digest is in the list).
	REQUIRE(db.set("fx/test/a", simple_doc(1, 8, 12).p));
	const auto patched = compile_effect_cached(&tc.cache, src, "fx/test/effect");
	REQUIRE(patched.ok());
	CHECK(patched->item.outcome == bf3::core::CacheOutcome::Converted);
	CHECK(patched->def.emitters[0].params.spawn_rate == 12.0f);

	// A patched blueprint that adds a component: the dependency list changes and is rewritten (rule 2), so the
	// next lookup is warm and holds the new document.
	EffectBuilder fx2(9);
	fx2.emitter(1);
	fx2.emitter(2);
	REQUIRE(db.set("fx/test/effect", fx2.done()));
	const auto grown = compile_effect_cached(&tc.cache, src, "fx/test/effect");
	REQUIRE(grown.ok());
	CHECK(grown->def.components.size() == 2);
	const auto grown_warm = compile_effect_cached(&tc.cache, src, "fx/test/effect");
	REQUIRE(grown_warm.ok());
	CHECK(grown_warm->item.outcome == bf3::core::CacheOutcome::Hit);
	CHECK(grown_warm->def.components.size() == 2);
	// And a patch of the newly listed document reaches the effect.
	REQUIRE(db.set("fx/test/b", simple_doc(2, 3, 6).p));
	const auto b_patched = compile_effect_cached(&tc.cache, src, "fx/test/effect");
	REQUIRE(b_patched.ok());
	CHECK(b_patched->item.outcome == bf3::core::CacheOutcome::Converted);
	CHECK(b_patched->def.emitters[1].params.spawn_rate == 6.0f);
}

TEST_CASE("cache: an absent dependency is in the key (rule 1)") {
	fxt::MemResolver db;
	REQUIRE(db.set("fx/test/effect", fxt::target(3)));
	const FxSources src = sources_of(db);
	const std::vector<std::string> deps{ "fx/test/effect", "fx/test/missing" };
	const auto absent = effect_def_key(src, "fx/test/effect", deps);
	REQUIRE(absent.has_value());
	REQUIRE(db.set("fx/test/missing", fxt::target(4)));
	const auto present = effect_def_key(src, "fx/test/effect", deps);
	REQUIRE(present.has_value());
	CHECK_FALSE(*absent == *present);
	// The deps entry is keyed by the blueprint only.
	CHECK(effect_deps_key(src, "fx/test/effect").has_value());
	CHECK_FALSE(effect_deps_key(src, "fx/test/nothing").has_value());
}

TEST_CASE("cache: an undecodable entry is dropped and recompiled") {
	TempCache tc("unit_corrupt");
	REQUIRE(tc.open);
	fxt::MemResolver db;
	REQUIRE(db.set("fx/test/a", simple_doc(1).p));
	EffectBuilder fx(9);
	fx.emitter(1);
	REQUIRE(db.set("fx/test/effect", fx.done()));
	const FxSources src = sources_of(db);
	REQUIRE(compile_effect_cached(&tc.cache, src, "fx/test/effect").ok());
	const auto deps = decode_effect_deps(encode_effect_deps({ "fx/test/a", "fx/test/effect" }));
	REQUIRE(deps.ok());
	const auto key = effect_def_key(src, "fx/test/effect", *deps);
	REQUIRE(key.has_value());
	const std::vector<uint8_t> junk{ 'n', 'o', 't', ' ', 'a', ' ', 'b', 'l', 'o', 'b' };
	tc.cache.remove(*key);
	REQUIRE(tc.cache.put(*key, junk).written);
	bf3::core::ConvertTotals totals;
	const auto again = compile_effect_cached(&tc.cache, src, "fx/test/effect", &totals);
	REQUIRE(again.ok());
	CHECK(again->item.dropped_corrupt);
	CHECK(totals.dropped_corrupt.load() == 1);
	CHECK(compile_effect_cached(&tc.cache, src, "fx/test/effect")->item.outcome == bf3::core::CacheOutcome::Hit);
}

// ---- simulator: spawning and ageing -----------------------------------------------------------------------------

TEST_CASE("sim: the accumulator starts at 1, rate mode, window, repeat") {
	FxWorld w;
	{
		// rate x dt = 1 exactly: 1 at the first step, then 1 per step.
		const auto d = fxt::effect_def("rate", { fxt::emitter_def("e", 100, 6, 100) });
		REQUIRE(w.start(d, start_with(1)).ok());
		run_frames(w, 1.0f / 6.0f, 6);
		CHECK(w.stats().spawned == 7);
		CHECK(w.stats().live_particles == 7);
	}
	w.clear();
	{
		// Window 0.5 s, rate 4, dt 0.25: steps at e_age 0 and 0.25 spawn 2 and 1, then the window is over.
		const auto d = fxt::effect_def("window", { fxt::emitter_def("e", 100, 4, 100, 0.5f) });
		REQUIRE(w.start(d, start_with(1)).ok());
		run_frames(w, 0.25f, 8);
		CHECK(w.stats().spawned == 3);
	}
	w.clear();
	{
		// Repeat: the window wraps and re-arms the accumulator: 3 per window.
		EmitterDef e = fxt::emitter_def("e", 100, 4, 100, 0.5f);
		e.tmpl.flags |= kEmitterRepeatParticleSpawning;
		const auto d = fxt::effect_def("repeat", { e });
		const auto h = w.start(d, start_with(1));
		REQUIRE(h.ok());
		run_frames(w, 0.25f, 4);
		CHECK(w.stats().spawned == 6);
		CHECK(w.alive(*h));
	}
	w.clear();
	{
		// Burst: floor(1 + rate x window) at the window start.
		EmitterDef e = fxt::emitter_def("e", 100, 10, 100, 0.5f);
		e.params.distribute_over_time = false;
		const auto d = fxt::effect_def("burst", { e });
		REQUIRE(w.start(d, start_with(1)).ok());
		run_frames(w, 1.0f / 60.0f, 1);
		CHECK(w.stats().spawned == 6);
		run_frames(w, 1.0f / 60.0f, 60);
		CHECK(w.stats().spawned == 6);
	}
}

TEST_CASE("sim: MaxCount cap across sub-steps, TimeScale, life") {
	FxWorldConfig cfg;
	cfg.max_substep = 0.25f;
	FxWorld w(cfg);
	{
		const auto d = fxt::effect_def("cap", { fxt::emitter_def("e", 3, 100, 100) });
		REQUIRE(w.start(d, start_with(1)).ok());
		const uint32_t k = w.begin_frame(1.0f, FxView{});
		CHECK(k == 4);
		for (uint32_t s = 0; s < k; ++s) {
			w.run_items(0, w.begin_step());
			w.end_step();
			CHECK(w.stats().live_particles <= 3);
		}
		CHECK(w.stats().live_particles == 3);
		CHECK(w.stats().spawns_capped > 0);
		CHECK(w.stats().substeps == 4);
	}
	w.clear();
	{
		// TimeScale 0.5: a 1 s particle lives 2 s of real time.
		EmitterDef e = fxt::emitter_def("e", 1, 1, 1.0f, 0.01f);
		e.params.distribute_over_time = false;
		e.tmpl.time_scale = 0.5f;
		const auto d = fxt::effect_def("timescale", { e });
		REQUIRE(w.start(d, start_with(1)).ok());
		run_frames(w, 0.1f, 19);
		CHECK(w.stats().live_particles == 1);
		run_frames(w, 0.1f, 2);
		CHECK(w.stats().live_particles == 0);
	}
}

TEST_CASE("sim: gravity sign, drag, size and colour products, frames") {
	FxWorld w;
	EmitterDef e = fxt::emitter_def("e", 1, 1, 10, 0.01f);
	e.params.distribute_over_time = false;
	e.slots[size_t(ProcessorType::Gravity)].present = true;
	e.params.gravity = 10.0f;
	e.slots[size_t(ProcessorType::SpawnSize)].present = true;
	e.params.size = 2.0f;
	e.slots[size_t(ProcessorType::UpdateSize)].present = true;
	e.slots[size_t(ProcessorType::UpdateSize)].default_values = { 0.5f, 0.5f, 0.5f, 1 };
	e.slots[size_t(ProcessorType::UpdateSizeX)].present = true;
	e.slots[size_t(ProcessorType::UpdateSizeX)].default_values = { 3, 1, 1, 1 };
	e.slots[size_t(ProcessorType::UpdateColor)].present = true;
	e.params.color = { 2, 3, 4 };
	e.slots[size_t(ProcessorType::SpawnAnimationFrame)].present = true;
	e.params.animation_frame = 5;
	e.texture.frame_count = 4;
	e.texture.frame_columns = 2;
	const auto d = fxt::effect_def("forces", { e });
	REQUIRE(w.start(d, start_with(1)).ok());
	run_frames(w, 0.1f, 10);
	REQUIRE(w.particles().size() == 1);
	const ParticleVertex &v = w.particles()[0];
	CHECK(v.position.y < 0.0f); // positive Gravity pulls down
	CHECK(v.size.x == doctest::Approx(3.0f)); // 2 x 0.5 x 3
	CHECK(v.size.y == doctest::Approx(1.0f));
	CHECK(v.color.x == 2.0f);
	CHECK(v.color.z == 4.0f);
	CHECK(v.frame == 1.0f); // 5 wrapped into [0, 4)
	// Drag slows: the same with AirResistance falls less far.
	FxWorld w2;
	EmitterDef e2 = e;
	e2.slots[size_t(ProcessorType::AirResistance)].present = true;
	e2.params.drag = 2.0f;
	REQUIRE(w2.start(fxt::effect_def("drag", { e2 }), start_with(1)).ok());
	run_frames(w2, 0.1f, 10);
	REQUIRE(w2.particles().size() == 1);
	CHECK(w2.particles()[0].position.y > v.position.y);
}

TEST_CASE("sim: effect x component transform order, start_age") {
	FxWorld w;
	auto d = std::make_shared<EffectDef>(*fxt::effect_def("xf", { fxt::emitter_def("e", 4, 10, 10) }));
	d->transform.trans = { 0, 0, 2 };
	d->components[0].transform.trans = { 0, 5, 0 };
	EffectStart s = start_with(1);
	s.transform.trans = { 10, 0, 0 };
	REQUIRE(w.start(d, s).ok());
	run_frames(w, 1.0f / 60.0f, 1);
	REQUIRE(w.particles().size() == 1);
	CHECK(w.particles()[0].position.x == doctest::Approx(10.0f));
	CHECK(w.particles()[0].position.y == doctest::Approx(5.0f));
	CHECK(w.particles()[0].position.z == doctest::Approx(2.0f));
	w.clear();
	// A windowed emitter whose window ended before start_age does not start.
	const auto windowed = fxt::effect_def("late", { fxt::emitter_def("e", 4, 10, 10, 1.0f) });
	EffectStart late = start_with(1);
	late.start_age = 2.0f;
	REQUIRE(w.start(windowed, late).ok());
	run_frames(w, 1.0f / 60.0f, 30);
	CHECK(w.stats().spawned == 0);
	w.clear();
	// A continuous one starts with its clock at start_age: no first-step particle from the initial accumulator.
	const auto cont = fxt::effect_def("cont", { fxt::emitter_def("e", 100, 6, 100) });
	EffectStart late2 = start_with(1);
	late2.start_age = 5.0f;
	REQUIRE(w.start(cont, late2).ok());
	run_frames(w, 1.0f / 6.0f, 6);
	CHECK(w.stats().spawned == 6);
}

TEST_CASE("sim: children start per parent particle, follow it and stop with it") {
	FxWorld w;
	EmitterDef parent = fxt::emitter_def("parent", 2, 2, 0.5f, 0.01f);
	parent.params.distribute_over_time = false;
	parent.params.spawn_rate = 150; // burst: floor(1 + 150 x 0.01) = 2
	parent.slots[size_t(ProcessorType::SpawnPosition)].present = true;
	parent.slots[size_t(ProcessorType::SpawnPosition)].evaluator = 0;
	EvaluatorDef box;
	box.type = EvaluatorType::Box;
	box.box = BoxDef{ Vec3{ 10, 10, 10 }, Vec3{} };
	parent.evaluators.push_back(box);
	parent.children = { 1 };
	EmitterDef child = fxt::emitter_def("child", 4, 30, 0.1f);
	child.tmpl.flags |= kEmitterChild;
	const auto d = fxt::effect_def("kids", { parent, child });
	CHECK(d->max_particles == 2 * (1 + 4));
	const auto h = w.start(d, start_with(7));
	REQUIRE(h.ok());
	run_frames(w, 1.0f / 60.0f, 6);
	// Two parents, two child instances, each spawning at its own parent.
	std::vector<Vec3> child_pos;
	std::vector<Vec3> parent_pos;
	for (const EmitterOutput &o : w.outputs()) {
		for (uint32_t k = 0; k < o.particle_count; ++k) {
			(o.emitter == 1 ? child_pos : parent_pos).push_back(w.particles()[o.first_particle + k].position);
		}
	}
	REQUIRE(parent_pos.size() == 2);
	REQUIRE(child_pos.size() >= 2);
	CHECK(w.stats().live_emitters == 3);
	const auto close_to = [](const Vec3 &p_a, const Vec3 &p_b) { return std::fabs(p_a.x - p_b.x) + std::fabs(p_a.y - p_b.y) + std::fabs(p_a.z - p_b.z) < 1e-3f; };
	for (const Vec3 &c : child_pos) {
		CHECK((close_to(c, parent_pos[0]) || close_to(c, parent_pos[1])));
	}
	CHECK_FALSE(close_to(parent_pos[0], parent_pos[1]));
	// The parents die at 0.5 s; their children stop and finish; the effect ends.
	run_frames(w, 1.0f / 60.0f, 60);
	CHECK(w.stats().live_particles == 0);
	CHECK_FALSE(w.alive(*h));
}

TEST_CASE("sim: pools steal with reset and refuse without") {
	FxWorld w;
	const auto steal = fxt::effect_def("steal", { fxt::emitter_def("e", 4, 10, 10) }, { 0 }, 2, true);
	const auto h0 = w.start(steal, start_with(1));
	const auto h1 = w.start(steal, start_with(2));
	REQUIRE(h0.ok());
	REQUIRE(h1.ok());
	const auto h2 = w.start(steal, start_with(3));
	REQUIRE(h2.ok());
	CHECK(w.stats().stolen_instances == 1);
	CHECK_FALSE(w.alive(*h0));
	CHECK(w.alive(*h1));
	const auto refuse = fxt::effect_def("refuse", { fxt::emitter_def("e", 4, 10, 10) }, { 0 }, 1, false);
	REQUIRE(w.start(refuse, start_with(4)).ok());
	CHECK_FALSE(w.start(refuse, start_with(5)).ok());
	CHECK(w.stats().refused_starts == 1);
	// A def larger than the world's budget is refused.
	FxWorldConfig small;
	small.max_particles = 10;
	FxWorld tiny(small);
	CHECK_FALSE(tiny.start(fxt::effect_def("big", { fxt::emitter_def("e", 100, 10, 10) }), start_with(1)).ok());
}

TEST_CASE("sim: component selection fractions match fx_component_selected") {
	auto d = std::make_shared<EffectDef>(*fxt::effect_def("prob", { fxt::emitter_def("e", 1, 1, 0.05f, 0.01f) }));
	d->components[0].spawn_probability = 0.3f;
	d->emitters[0].params.distribute_over_time = false;
	HarnessRun run;
	run.seconds = 0.1f;
	const HarnessReport rep = fx_simulate_starts(d, 2000, run);
	REQUIRE(rep.pass);
	size_t selected = 0;
	for (uint32_t i = 0; i < 2000; ++i) {
		selected += fx_component_selected(*d, 0, fx_harness_seed(1, i), kTierHigh) ? 1 : 0;
	}
	CHECK(double(selected) / 2000.0 == doctest::Approx(0.3).epsilon(0.1));
	bool found = false;
	for (const std::string &l : rep.lines) {
		if (l.rfind("component\t0\t", 0) == 0) {
			found = true;
			// selected and observed are printed with the same format: equal fractions print equal text.
			const size_t s = l.find("selected\t");
			const size_t o = l.find("\tobserved\t");
			REQUIRE(s != std::string::npos);
			REQUIRE(o != std::string::npos);
			CHECK(l.substr(s + 9, o - s - 9) == l.substr(o + 10));
		}
	}
	CHECK(found);
}

TEST_CASE("sim: effect_extent") {
	const auto finite = fxt::effect_def("f", { fxt::emitter_def("e", 4, 10, 2.0f, 0.5f) });
	const EffectExtent a = effect_extent(*finite);
	CHECK(a.finite);
	CHECK(a.spawn_end == doctest::Approx(0.5f));
	CHECK(a.end == doctest::Approx(2.5f));
	const auto cont = fxt::effect_def("c", { fxt::emitter_def("e", 4, 10, 2.0f) });
	CHECK_FALSE(effect_extent(*cont).finite);
}

TEST_CASE("sim: G8 determinism on 1, 2 and 8 lanes and two partitions, with a saturated world") {
	FxWorldConfig cfg;
	cfg.max_particles = 1000;
	// One def for every run: EmitterOutput::def (in the digest) is the same pointer in all of them.
	const auto def = [] {
		EmitterDef e = fxt::emitter_def("e", 100, 1000, 0.5f);
		e.slots[size_t(ProcessorType::SpawnPosition)].present = true;
		e.slots[size_t(ProcessorType::SpawnPosition)].evaluator = 0;
		EvaluatorDef sph;
		sph.type = EvaluatorType::Sphere;
		sph.sphere.radius = 3;
		e.evaluators.push_back(sph);
		e.slots[size_t(ProcessorType::Turbulance)].present = true;
		e.params.turbulence = 5;
		return fxt::effect_def("sat", { e }, { 0 }, 64);
	}();
	const auto build = [&](FxWorld &r_w) {
		for (uint64_t i = 0; i < 50; ++i) {
			REQUIRE(r_w.start(def, start_with(100 + i)).ok());
		}
	};
	struct Run {
		std::vector<uint64_t> digests;
		std::vector<std::vector<ParticleVertex>> particles;
		FxStats stats;
	};
	const auto record = [&](LanePool *p_pool, size_t p_chunk, unsigned p_step_lanes) {
		FxWorld w(cfg);
		build(w);
		Run r;
		for (uint32_t f = 0; f < 90; ++f) {
			const float dt = fx_variable_dt(3, f);
			if (p_step_lanes > 0) {
				w.step(dt, FxView{}, p_step_lanes);
			} else {
				fx_step_frame(w, dt, p_pool, p_chunk);
			}
			r.digests.push_back(fx_frame_digest(w));
			r.particles.emplace_back(w.particles().begin(), w.particles().end());
		}
		r.stats = w.stats();
		return r;
	};
	const Run one = record(nullptr, 1, 0);
	CHECK(one.stats.spawns_dropped > 0);
	LanePool two(2), eight(8);
	const Run runs[] = { record(&two, 1, 0), record(&eight, 1, 0), record(&eight, 7, 0), record(nullptr, 1, 8) };
	for (const Run &r : runs) {
		CHECK(r.digests == one.digests);
		CHECK(r.stats.spawns_dropped == one.stats.spawns_dropped);
		REQUIRE(r.particles.size() == one.particles.size());
		for (size_t f = 0; f < r.particles.size(); ++f) {
			REQUIRE(r.particles[f].size() == one.particles[f].size());
			CHECK(std::memcmp(r.particles[f].data(), one.particles[f].data(), r.particles[f].size() * sizeof(ParticleVertex)) == 0);
		}
	}
	CHECK(one.stats.live_particles <= cfg.max_particles);
}

} // TEST_SUITE
