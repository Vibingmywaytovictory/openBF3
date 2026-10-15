// M2a VFX1 fx.fuzz (gate G10, docs/design/m2a-vfx1.md §10): hostile synthetic EBX graphs through the compiler,
// truncated / bit-flipped / count-inflated blobs through the decoder, hostile parameters and dt sequences through the
// simulator. Every case must end with an error or a bounded result: no crash, no hang, no unbounded allocation. Runs
// in the dev and the asan presets; no install.
#include <doctest/doctest.h>

#include "bf3/fx/effect_cache.h"
#include "bf3/fx/effect_compiler.h"
#include "bf3/fx/fx_harness.h"
#include "bf3/fx/simulator.h"
#include "fx_test_support.h"
#include "test_support.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

using namespace bf3::fx;
using fxt::DocBuilder;
using fxt::EffectBuilder;
using fxt::f32;
using fxt::flag;
using fxt::i32;
using fxt::raw;

namespace {

constexpr double kFastSeconds = 30.0; // generous for the asan build on a busy machine

double seconds_since(std::chrono::steady_clock::time_point p_start) {
	return std::chrono::duration<double>(std::chrono::steady_clock::now() - p_start).count();
}

uint64_t splitmix(uint64_t &r_state) {
	return fx_mix64(r_state++);
}

DocBuilder rich_doc(uint32_t p_seed) {
	DocBuilder d(p_seed, 32, 0.5f);
	d.proc("SpawnRateData", { { "SpawnRate", f32(40) }, { "DistributeOverTime", flag(true) } }, 3);
	const uint32_t pos = d.proc("SpawnPositionData");
	d.pre(pos, "SphereEvaluatorData", { { "Radius", f32(2) }, { "Scale", fxt::v3(1, 1, 1) }, { "Pivot", fxt::v3(0, 1, 0) } });
	const uint32_t dir = d.proc("SpawnDirectionData", { { "DirectionFromEmitterOrigin", f32(0.5f) } });
	d.pre(dir, "RotateVectorData", { { "Angle", f32(30) } });
	const uint32_t speed = d.proc("SpawnSpeedData", { { "Speed", f32(3) } });
	d.pre(speed, "RandomEvaluatorData", { { "Min", f32(0.5f) }, { "Max", f32(1) } });
	d.proc("UpdateAgeData", { { "Lifetime", f32(1) } });
	d.proc("GravityData", { { "Gravity", f32(9.8f) } });
	d.proc("TurbulanceData", { { "Intensity", f32(2) } });
	d.proc("UpdateLinearVelocityData");
	const uint32_t size = d.proc("UpdateSizeData", { { "Pivot", fxt::v2(0, 0) } });
	d.pre(size, "PolynomialOperatorData",
			{ { "FirstOperand", fxt::st(DocBuilder::poly(0, 0, 1, 0)) }, { "SecondOperand", fxt::st(DocBuilder::poly(0, 0, 0, 2)) }, { "Operation", raw(1) },
					{ "MinClampResult", f32(0) }, { "MaxClampResult", f32(5) } });
	const uint32_t col = d.proc("UpdateColorData", { { "Color", fxt::v3(1, 1, 1) } });
	d.pre(col, "SampleTextureData",
			{ { "ColorIntensityMin", fxt::v3(0, 0, 0) }, { "ColorIntensityMax", fxt::v3(1, 1, 1) },
					{ "GradientData", d.p.array(fxt::Array{ "Vec4", {}, { DocBuilder::poly(0, 0, 0, 0), {} } }) } });
	d.proc("UpdateTextureCoordsData",
			{ { "TextureBlendValue", f32(1) },
					{ "TextureInfo1", fxt::st({ { "TextureName", d.p.str("fx/t/a") }, { "AnimationFrameCount", f32(8) }, { "AnimationFrameColumnCount", f32(4) } }) } });
	return d;
}

// A small valid effect: two documents (one a child of the other) and a blueprint.
void rich_db(fxt::MemResolver &r_db) {
	DocBuilder child = rich_doc(2);
	REQUIRE(r_db.set("fx/fuzz/child", child.p));
	DocBuilder parent = rich_doc(1);
	const uint32_t em = parent.proc("EmitterData");
	fxt::Array a;
	a.words.push_back(parent.p.ext(2, 1).word);
	fxt::set(parent.p.instances[em].values, "EmitterAssets", parent.p.array(std::move(a)));
	REQUIRE(r_db.set("fx/fuzz/parent", parent.p));
	EffectBuilder fx(3, 4);
	fx.emitter(1, 0.75f);
	fx.emitter(2, 1.0f, { { "StartDelay", f32(0.2f) } });
	REQUIRE(r_db.set("fx/fuzz/effect", fx.done()));
}

// The decode rule of G10: a blob decodes only when it re-encodes byte-identically.
void check_blob(const std::vector<uint8_t> &p_blob) {
	const bf3::core::Result<EffectDef> d = decode_effect_def(p_blob);
	if (d.ok()) {
		CHECK(encode_effect_def(*d) == p_blob);
	}
}

} // namespace

TEST_SUITE("fx-fuzz") {

TEST_CASE("fuzz: mutated EBX graphs compile or fail, within the work budget") {
	const auto start = std::chrono::steady_clock::now();
	fxt::MemResolver base;
	rich_db(base);
	{
		CompileContext ctx;
		ctx.db = &base;
		const auto ok = compile_effect("fx/fuzz/effect", ctx);
		REQUIRE_MESSAGE(ok.ok(), (ok.ok() ? std::string() : ok.error().message));
	}
	const std::vector<uint8_t> parent = rich_doc(1).p.build();
	uint32_t meta = 0; // the payload (strings, instances, arrays) starts at the header's meta size
	std::memcpy(&meta, parent.data() + 4, 4);
	REQUIRE(meta < parent.size());
	uint64_t rng = 0x5eed;
	size_t parsed = 0, compiled = 0;
	for (int round = 0; round < 1500; ++round) {
		fxt::MemResolver db;
		rich_db(db);
		std::vector<uint8_t> bytes = parent;
		// The payload is where field values live (most metadata mutations only fail the parse): 7 of 8 rounds
		// mutate it, the 8th anywhere.
		const size_t flips = 1 + splitmix(rng) % 4;
		const bool anywhere = round % 8 == 7;
		for (size_t k = 0; k < flips; ++k) {
			const uint64_t r = splitmix(rng);
			const size_t at = anywhere ? size_t(r % bytes.size()) : size_t(meta + r % (bytes.size() - meta));
			switch ((r >> 32) % 4) {
				case 0:
					bytes[at] ^= uint8_t(1u << ((r >> 40) % 8));
					break;
				case 1:
					bytes[at] = 0xFF;
					break;
				case 2: {
					const float hostile[] = { std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), -1.0e30f, 1.0e30f, -1.0f, 2.5f };
					if (at + 4 <= bytes.size()) {
						std::memcpy(bytes.data() + at - at % 4, &hostile[(r >> 48) % 6], 4);
					}
					break;
				}
				default:
					if (at + 4 <= bytes.size()) {
						const uint32_t big = uint32_t(r >> 16);
						std::memcpy(bytes.data() + at - at % 4, &big, 4);
					}
					break;
			}
		}
		if (!db.set_bytes("fx/fuzz/parent", bytes)) {
			continue;
		}
		++parsed;
		CompileContext ctx;
		ctx.db = &db;
		const auto r = compile_effect("fx/fuzz/effect", ctx);
		CHECK(ctx.budget.used() <= ctx.budget.limit());
		if (r.ok()) {
			++compiled;
			check_blob(encode_effect_def(*r));
			// A def that compiles also simulates sanely for a moment.
			FxWorld w;
			EffectStart s;
			s.seed = round;
			const auto h = w.start(std::make_shared<const EffectDef>(*r), s);
			if (h.ok()) {
				for (int f = 0; f < 30; ++f) {
					fx_step_frame(w, 1.0f / 30.0f);
				}
				CHECK(w.stats().live_particles <= r->max_particles);
				for (const ParticleVertex &v : w.particles()) {
					CHECK(std::isfinite(v.position.x));
				}
			}
		}
	}
	MESSAGE("mutated graphs: " << parsed << " parsed, " << compiled << " compiled");
	CHECK(parsed > 700); // about 950 of the 1,500 rounds parse
	CHECK(seconds_since(start) < kFastSeconds * 4);
}

TEST_CASE("fuzz: hostile flipbooks, parameters, structure limits") {
	const auto compile_doc = [](DocBuilder &r_d) {
		fxt::MemResolver db;
		REQUIRE(db.set("fx/fuzz/doc", r_d.p));
		CompileContext ctx;
		ctx.db = &db;
		return compile_emitter("fx/fuzz/doc", ctx);
	};
	const float nan = std::numeric_limits<float>::quiet_NaN();
	const float inf = std::numeric_limits<float>::infinity();
	// Flipbook counts: fractional, negative, NaN, huge, columns past frames.
	const std::pair<float, float> books[] = { { 2.5f, 1.0f }, { -1.0f, 1.0f }, { nan, 1.0f }, { inf, 1.0f }, { 1.0e9f, 1.0f }, { 4.0f, 8.0f }, { 4.0f, 0.5f }, { 2048.0f, 4.0f } };
	for (const auto &[frames, columns] : books) {
		DocBuilder d(1, 4);
		d.proc("UpdateTextureCoordsData",
				{ { "TextureBlendValue", f32(1) },
						{ "TextureInfo1", fxt::st({ { "AnimationFrameCount", f32(frames) }, { "AnimationFrameColumnCount", f32(columns) } }) } });
		CHECK_FALSE(compile_doc(d).ok());
	}
	// Non-finite and huge floats anywhere.
	for (const float bad : { nan, inf, -inf, 1.0e30f, -2.0e7f }) {
		DocBuilder d(1, 4);
		d.proc("SpawnRateData", { { "SpawnRate", f32(bad) }, { "DistributeOverTime", flag(true) } });
		CHECK_FALSE(compile_doc(d).ok());
		DocBuilder t(1, 4);
		fxt::set(t.tmpl(), "PointLightRadius", f32(bad));
		CHECK_FALSE(compile_doc(t).ok());
	}
	// Template bounds: MaxCount 0 / past 16,384, negative Lifetime, TimeScale 0.
	for (const int64_t mc : { int64_t(0), int64_t(16385), int64_t(-5) }) {
		DocBuilder d(1, 4);
		fxt::set(d.tmpl(), "MaxCount", i32(mc));
		CHECK_FALSE(compile_doc(d).ok());
	}
	{
		DocBuilder d(1, 4, -1.0f);
		CHECK_FALSE(compile_doc(d).ok());
		DocBuilder z(1, 4, 0, 0);
		CHECK_FALSE(compile_doc(z).ok());
	}
	// A chain past kMaxChainLength cannot be built without duplicate types (32 types): a duplicate type is an error.
	{
		DocBuilder d(1, 4);
		d.proc("GravityData", { { "Gravity", f32(1) } });
		d.proc("GravityData", { { "Gravity", f32(2) } });
		CHECK_FALSE(compile_doc(d).ok());
	}
	// An evaluator where a processor is expected, a processor as a Pre, an unknown enumerator.
	{
		DocBuilder d(1, 4);
		const uint32_t e = d.p.add({ "PolynomialData", DocBuilder::poly(0, 0, 0, 1) });
		fxt::set(d.p.instances[1].values, "RootProcessor", d.p.ref(e));
		CHECK_FALSE(compile_doc(d).ok());
		DocBuilder q(1, 4);
		const uint32_t g = q.proc("GravityData", { { "Gravity", f32(1) } });
		const uint32_t other = q.p.add({ "AirResistanceData", { { "Enable", flag(true) }, { "EvaluatorInput", raw(1) } } });
		fxt::set(q.p.instances[g].values, "Pre", q.p.ref(other));
		CHECK_FALSE(compile_doc(q).ok());
		DocBuilder u(1, 4);
		const uint32_t s = u.proc("SpawnSizeData", { { "Size", f32(1) } });
		u.pre(s, "PolynomialData", DocBuilder::poly(0, 0, 0, 1), 8); // EmitterParameterBogus
		CHECK_FALSE(compile_doc(u).ok());
		DocBuilder o(1, 4);
		const uint32_t sz = o.proc("UpdateSizeData", { { "Pivot", fxt::v2(0, 0) } });
		o.pre(sz, "PolynomialOperatorData", { { "Operation", raw(3) } }); // "Division"
		CHECK_FALSE(compile_doc(o).ok());
	}
	// Base emitters nested past kMaxBaseDepth, and a base cycle.
	{
		fxt::MemResolver db;
		for (uint32_t k = 0; k < 7; ++k) {
			DocBuilder d(10 + k, 4);
			d.proc("BaseEmitterData", { { "EmitterAsset", d.p.ext(10 + (k + 1) % 7, 1) } });
			REQUIRE(db.set("fx/fuzz/base" + std::to_string(k), d.p));
		}
		CompileContext ctx;
		ctx.db = &db;
		const auto r = compile_emitter("fx/fuzz/base0", ctx);
		REQUIRE_FALSE(r.ok());
		CHECK(r.error().message.find("deeper") != std::string::npos);
	}
	// Children nested past kMaxChildDepth (a child cycle: a -> b -> a).
	{
		fxt::MemResolver db;
		for (uint32_t k = 0; k < 2; ++k) {
			DocBuilder d(20 + k, 4);
			d.proc("SpawnRateData", { { "SpawnRate", f32(1) }, { "DistributeOverTime", flag(true) } }, 3);
			const uint32_t em = d.proc("EmitterData");
			fxt::Array a;
			a.words.push_back(d.p.ext(20 + (k + 1) % 2, 1).word);
			fxt::set(d.p.instances[em].values, "EmitterAssets", d.p.array(std::move(a)));
			REQUIRE(db.set("fx/fuzz/kid" + std::to_string(k), d.p));
		}
		EffectBuilder fx(30);
		fx.emitter(20);
		REQUIRE(db.set("fx/fuzz/kids", fx.done()));
		CompileContext ctx;
		ctx.db = &db;
		CHECK_FALSE(compile_effect("fx/fuzz/kids", ctx).ok());
	}
	// MaxCount products past kMaxEffectParticles: 16 children of 16,384 under a parent of 16,384.
	{
		fxt::MemResolver db;
		REQUIRE(db.set("fx/fuzz/leaf", DocBuilder(40, 16384).p));
		DocBuilder d(41, 16384);
		const uint32_t em = d.proc("EmitterData");
		fxt::Array a;
		a.words.push_back(d.p.ext(40, 1).word);
		fxt::set(d.p.instances[em].values, "EmitterAssets", d.p.array(std::move(a)));
		REQUIRE(db.set("fx/fuzz/root", d.p));
		EffectBuilder fx(42);
		fx.emitter(41);
		REQUIRE(db.set("fx/fuzz/huge", fx.done()));
		CompileContext ctx;
		ctx.db = &db;
		CHECK_FALSE(compile_effect("fx/fuzz/huge", ctx).ok());
	}
	// Components past kMaxEffectComponents, and a component that is not an EffectComponent.
	{
		fxt::MemResolver db;
		REQUIRE(db.set("fx/fuzz/doc", DocBuilder(50, 1).p));
		EffectBuilder fx(51);
		for (int k = 0; k < 257; ++k) {
			fx.emitter(50);
		}
		REQUIRE(db.set("fx/fuzz/many", fx.done()));
		CompileContext ctx;
		ctx.db = &db;
		CHECK_FALSE(compile_effect("fx/fuzz/many", ctx).ok());
		EffectBuilder odd(52);
		odd.component({ "EmitterDocument", {} });
		REQUIRE(db.set("fx/fuzz/odd", odd.done()));
		CompileContext c2;
		c2.db = &db;
		CHECK_FALSE(compile_effect("fx/fuzz/odd", c2).ok());
	}
	// A tiny work budget ends any graph with an error.
	{
		fxt::MemResolver db;
		rich_db(db);
		CompileContext ctx;
		ctx.db = &db;
		ctx.budget = bf3::core::gameplay::WorkBudget{ 16 };
		const auto r = compile_effect("fx/fuzz/effect", ctx);
		REQUIRE_FALSE(r.ok());
		CHECK(r.error().message.find("budget") != std::string::npos);
	}
}

TEST_CASE("fuzz: truncated, bit-flipped and count-inflated blobs") {
	fxt::MemResolver db;
	rich_db(db);
	CompileContext ctx;
	ctx.db = &db;
	const auto d = compile_effect("fx/fuzz/effect", ctx);
	REQUIRE(d.ok());
	const std::vector<uint8_t> blob = encode_effect_def(*d);
	REQUIRE(decode_effect_def(blob).ok());
	for (size_t n = 0; n < blob.size(); ++n) {
		CHECK_FALSE(decode_effect_def(std::span<const uint8_t>(blob.data(), n)).ok());
	}
	for (size_t bit = 0; bit < blob.size() * 8; ++bit) {
		std::vector<uint8_t> b = blob;
		b[bit / 8] ^= uint8_t(1u << (bit % 8));
		check_blob(b);
	}
	for (size_t at = 0; at + 4 <= blob.size(); ++at) {
		for (const uint32_t big : { 0xFFFFFFFFu, 0x7FFFFFFFu, 0x00100000u }) {
			std::vector<uint8_t> b = blob;
			std::memcpy(b.data() + at, &big, 4);
			check_blob(b);
		}
	}
	// Dependency lists.
	const std::vector<uint8_t> deps = encode_effect_deps({ "fx/a", "fx/b" });
	for (size_t n = 0; n < deps.size(); ++n) {
		CHECK_FALSE(decode_effect_deps(std::span<const uint8_t>(deps.data(), n)).ok());
	}
	for (size_t at = 0; at + 4 <= deps.size(); ++at) {
		std::vector<uint8_t> b = deps;
		const uint32_t big = 0xFFFFFFF0u;
		std::memcpy(b.data() + at, &big, 4);
		(void)decode_effect_deps(b);
	}
}

TEST_CASE("fuzz: NormTime without UpdateAge, Lifetime 0, hostile dt sequences") {
	// NormTime evaluators on an emitter without UpdateAge and on a continuous one: t = 0, no division.
	EmitterDef e = fxt::emitter_def("noage", 64, 50, 0.0f);
	e.slots[size_t(ProcessorType::UpdateTransparency)].present = true;
	e.slots[size_t(ProcessorType::UpdateTransparency)].evaluator = 0;
	EvaluatorDef p;
	p.type = EvaluatorType::Polynomial;
	p.polynomial = PolynomialDef{ { 1, 1, 1, 1 }, 0, 1, 1 };
	e.evaluators.push_back(p);
	EmitterDef zero = fxt::emitter_def("zerolife", 64, 50, 0.0f);
	zero.slots[size_t(ProcessorType::UpdateAge)].present = true; // life 0 with the slot present: dies after one step
	zero.params.particle_lifetime = 0;
	const auto d = fxt::effect_def("t0", { e, zero }, { 0, 1 });
	FxWorld w;
	REQUIRE(w.start(d, EffectStart{}).ok());
	const float nan = std::numeric_limits<float>::quiet_NaN();
	const float seq[] = { 1.0f / 60, 0.0f, -1.0f, nan, std::numeric_limits<float>::infinity(), 1.0e30f, 1.0e-30f, 1.0f / 60, 5.0f, -0.0f, 1.0f / 60 };
	for (int round = 0; round < 20; ++round) {
		for (const float dt : seq) {
			fx_step_frame(w, dt);
			for (const ParticleVertex &v : w.particles()) {
				REQUIRE(std::isfinite(v.color.w));
				REQUIRE(std::isfinite(v.age));
			}
			CHECK(w.stats().live_particles <= d->max_particles);
		}
	}
	CHECK(w.stats().nonfinite_kills == 0);
	CHECK(w.stats().dt_clamped > 0);
	CHECK(w.begin_frame(nan, FxView{}) == 0);
	CHECK(w.begin_frame(-1.0f, FxView{}) == 0);
	CHECK(w.begin_frame(1.0e30f, FxView{}) == 8); // clamped to max_dt 2, sub-steps of 0.25
}

TEST_CASE("fuzz: 4,096 starts of a MaxCount 16,384 def hold the world's memory bound") {
	const auto start = std::chrono::steady_clock::now();
	const auto d = fxt::effect_def("flood", { fxt::emitter_def("e", 16384, 1.0e6f, 1.0f) }, { 0 }, uint16_t(kMaxInstanceCount));
	FxWorld w;
	size_t ok = 0;
	for (uint64_t i = 0; i < 4096; ++i) {
		EffectStart s;
		s.seed = i;
		ok += w.start(d, s).ok() ? 1 : 0;
	}
	CHECK(ok == 4096); // the pool of 1,024 steals; every start succeeds
	CHECK(w.stats().stolen_instances == 4096 - kMaxInstanceCount);
	for (int f = 0; f < 20; ++f) {
		fx_step_frame(w, 1.0f / 30.0f);
		CHECK(w.stats().live_particles <= w.config().max_particles);
		CHECK(w.particles().size() <= w.config().max_particles);
	}
	CHECK(w.stats().spawns_dropped > 0);
	CHECK(seconds_since(start) < kFastSeconds);
}

} // TEST_SUITE
