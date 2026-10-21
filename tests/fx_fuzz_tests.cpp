#include <doctest/doctest.h>

#include "bf3/core/util/elapsed.h"
#include "bf3/fx/effect_cache.h"
#include "bf3/fx/effect_compiler.h"
#include "bf3/fx/emitter_atlas.h"
#include "bf3/fx/fx_harness.h"
#include "bf3/fx/impact_table.h"
#include "bf3/fx/simulator.h"
#include "fx_test_support.h"
#include "test_support.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
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

constexpr double kFastSeconds = 30.0;

using bf3::core::seconds_since;

uint64_t splitmix(uint64_t &r_state) {
	return splitmix64(r_state++);
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

void check_blob(const std::vector<uint8_t> &p_blob) {
	const bf3::core::Result<EffectDef> d = decode_effect_def(p_blob);
	if (d.ok()) {
		CHECK(encode_effect_def(*d) == p_blob);
	}
}

using bf3::core::gameplay::WorkBudget;
using CellFn = std::function<std::vector<uint32_t>(fxt::Partition &, size_t, size_t)>;

fxt::Partition grid_partition(uint32_t p_seed, size_t p_rows, size_t p_items, const CellFn &p_cell) {
	fxt::Partition p;
	p.seed = p_seed;
	p.add({ "MaterialGridData", {} });
	fxt::Array map{ "*i", {}, {} };
	for (size_t i = 0; i < p_rows; ++i) {
		map.words.push_back(uint32_t(i));
	}
	fxt::Array rows{ "MaterialInteractionGridRow", {}, {} };
	for (size_t r = 0; r < p_rows; ++r) {
		fxt::Array items{ "MaterialRelationPropertyPair", {}, {} };
		for (size_t c = 0; c < p_items; ++c) {
			fxt::Array props;
			props.words = p_cell(p, r, c);
			items.structs.push_back({ { "PhysicsMaterialProperties", p.array(std::move(props)) } });
		}
		rows.structs.push_back({ { "Items", p.array(std::move(items)) } });
	}
	fxt::set(p.instances[0].values, "MaterialIndexMap", p.array(std::move(map)));
	fxt::set(p.instances[0].values, "DefaultMaterialIndex", i32(0));
	fxt::set(p.instances[0].values, "InteractionGrid", p.array(std::move(rows)));
	return p;
}

uint32_t effect_relation(fxt::Partition &r_p, float p_spread, std::vector<uint32_t> p_effects = {}) {
	fxt::Vals v{ { "ImpactEffectMaxSpreadAngle", f32(p_spread) }, { "MirrorImpactDirection", flag(false) } };
	if (!p_effects.empty()) {
		fxt::Array a;
		a.words = std::move(p_effects);
		fxt::set(v, "ImpactEffects", r_p.array(std::move(a)));
	}
	return r_p.ref(r_p.add({ "MaterialRelationEffectData", std::move(v) })).word;
}

bf3::core::Result<ImpactTable> load_grid(const fxt::Partition &p_grid, WorkBudget &r_budget, fxt::MemResolver *p_db = nullptr) {
	fxt::MemResolver local;
	fxt::MemResolver &db = p_db != nullptr ? *p_db : local;
	if (!db.set("fx/fuzz/grid", p_grid)) {
		return bf3::core::make_error("synthetic grid did not parse");
	}
	return ImpactTable::load(db, "FX/Fuzz/Grid", r_budget);
}

std::vector<uint8_t> dx_header(uint32_t p_format, uint32_t p_flags, uint16_t p_width, uint16_t p_height, uint8_t p_mips) {
	std::vector<uint8_t> b(128, 0);
	const uint32_t version = 10;
	std::memcpy(b.data() + 0x00, &version, 4);
	std::memcpy(b.data() + 0x08, &p_format, 4);
	std::memcpy(b.data() + 0x0C, &p_flags, 4);
	std::memcpy(b.data() + 0x10, &p_width, 2);
	std::memcpy(b.data() + 0x12, &p_height, 2);
	const uint16_t one = 1;
	std::memcpy(b.data() + 0x14, &one, 2);
	std::memcpy(b.data() + 0x16, &one, 2);
	b[0x1A] = p_mips;
	return b;
}

struct AtlasInfo {
	std::string name;
	float min_x = 0, min_y = 0, max_x = 0, max_y = 0;
};

fxt::Partition atlas_partition(uint32_t p_seed, uint32_t p_base_seed, uint32_t p_normal_seed, const std::vector<AtlasInfo> &p_infos,
		const std::string &p_type = "EmitterSystemAsset") {
	fxt::Partition p;
	p.seed = p_seed;
	p.add({ p_type, {} });
	if (p_type != "EmitterSystemAsset") {
		return p;
	}
	if (p_base_seed != 0) {
		fxt::set(p.instances[0].values, "BaseAtlasTexture", p.ext(p_base_seed, 0));
	}
	if (p_normal_seed != 0) {
		fxt::set(p.instances[0].values, "NormalAtlasTexture", p.ext(p_normal_seed, 0));
	}
	fxt::Array infos{ "EmitterTextureAtlasInfo", {}, {} };
	for (const AtlasInfo &i : p_infos) {
		infos.structs.push_back({ { "TextureName", p.str(i.name) }, { "MinUv", fxt::v2(i.min_x, i.min_y) }, { "MaxUv", fxt::v2(i.max_x, i.max_y) } });
	}
	fxt::set(p.instances[0].values, "TextureInfos", p.array(std::move(infos)));
	return p;
}

struct AtlasDb {
	fxt::MemResolver db;
	std::map<std::string, std::vector<uint8_t>> res;
	size_t res_calls = 0;
	AtlasDb() {
		REQUIRE(db.set("fx/fuzz/atlasbase", fxt::target(81)));
		REQUIRE(db.set("fx/fuzz/atlasnormal_n", fxt::target(82)));
		res["fx/fuzz/atlasbase"] = dx_header(2, 0x22, 4096, 4096, 13);
		res["fx/fuzz/atlasnormal_n"] = dx_header(20, 0x20, 1024, 2048, 12);
	}
	EmitterAtlas::ResSource source() {
		return [this](const std::string &p_name) -> bf3::core::Result<std::vector<uint8_t>> {
			++res_calls;
			const auto it = res.find(p_name);
			if (it == res.end()) {
				return bf3::core::make_error(p_name + ": no such res");
			}
			return it->second;
		};
	}
	bf3::core::Result<EmitterAtlas> load(const fxt::Partition &p_system, WorkBudget &r_budget) {
		if (!db.set("fx/fuzz/emittersystem_win32", p_system)) {
			return bf3::core::make_error("synthetic system did not parse");
		}
		return EmitterAtlas::load(db, source(), "fx/fuzz/emittersystem_win32", r_budget);
	}

	std::optional<bf3::core::Result<EmitterAtlas>> load_bytes(std::vector<uint8_t> p_bytes, WorkBudget &r_budget) {
		if (!db.set_bytes("fx/fuzz/emittersystem_win32", std::move(p_bytes))) {
			return std::nullopt;
		}
		return EmitterAtlas::load(db, source(), "fx/fuzz/emittersystem_win32", r_budget);
	}
};

std::vector<AtlasInfo> valid_infos() {
	return { { "FX/T/Flash", 0.25f, 0.5f, 0.5f, 0.75f }, { "fx/t/flash_n", 0.0f, 0.0f, 0.5f, 0.5f }, { "fx/t/defaultnormalmap", 0.5f, 0.5f, 0.25f, 0.25f } };
}

void mutate(std::vector<uint8_t> &r_bytes, size_t p_from, uint64_t &r_rng) {
	const size_t flips = 1 + splitmix(r_rng) % 4;
	for (size_t k = 0; k < flips; ++k) {
		const uint64_t r = splitmix(r_rng);
		const size_t at = size_t(p_from + r % (r_bytes.size() - p_from));
		switch ((r >> 32) % 4) {
			case 0:
				r_bytes[at] ^= uint8_t(1u << ((r >> 40) % 8));
				break;
			case 1:
				r_bytes[at] = 0xFF;
				break;
			case 2: {
				const float hostile[] = { std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), -1.0e30f, 1.0e30f, -1.0f, 2.5f };
				if (at + 4 <= r_bytes.size()) {
					std::memcpy(r_bytes.data() + at - at % 4, &hostile[(r >> 48) % 6], 4);
				}
				break;
			}
			default:
				if (at + 4 <= r_bytes.size()) {
					const uint32_t big = uint32_t(r >> 16);
					std::memcpy(r_bytes.data() + at - at % 4, &big, 4);
				}
				break;
		}
	}
}

size_t payload_start(const std::vector<uint8_t> &p_bytes) {
	uint32_t meta = 0;
	std::memcpy(&meta, p_bytes.data() + 4, 4);
	return meta < p_bytes.size() ? meta : 0;
}

}

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
	uint32_t meta = 0;
	std::memcpy(&meta, parent.data() + 4, 4);
	REQUIRE(meta < parent.size());
	uint64_t rng = 0x5eed;
	size_t parsed = 0, compiled = 0;
	for (int round = 0; round < 1500; ++round) {
		fxt::MemResolver db;
		rich_db(db);
		std::vector<uint8_t> bytes = parent;

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
	CHECK(parsed > 700);
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

	const std::pair<float, float> books[] = { { 2.5f, 1.0f }, { -1.0f, 1.0f }, { nan, 1.0f }, { inf, 1.0f }, { 1.0e9f, 1.0f }, { 4.0f, 8.0f }, { 4.0f, 0.5f }, { 2048.0f, 4.0f } };
	for (const auto &[frames, columns] : books) {
		DocBuilder d(1, 4);
		d.proc("UpdateTextureCoordsData",
				{ { "TextureBlendValue", f32(1) },
						{ "TextureInfo1", fxt::st({ { "AnimationFrameCount", f32(frames) }, { "AnimationFrameColumnCount", f32(columns) } }) } });
		CHECK_FALSE(compile_doc(d).ok());
	}

	for (const float bad : { nan, inf, -inf, 1.0e30f, -2.0e7f }) {
		DocBuilder d(1, 4);
		d.proc("SpawnRateData", { { "SpawnRate", f32(bad) }, { "DistributeOverTime", flag(true) } });
		CHECK_FALSE(compile_doc(d).ok());
		DocBuilder t(1, 4);
		fxt::set(t.tmpl(), "PointLightRadius", f32(bad));
		CHECK_FALSE(compile_doc(t).ok());
	}

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

	{
		DocBuilder d(1, 4);
		d.proc("GravityData", { { "Gravity", f32(1) } });
		d.proc("GravityData", { { "Gravity", f32(2) } });
		CHECK_FALSE(compile_doc(d).ok());
	}

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
		u.pre(s, "PolynomialData", DocBuilder::poly(0, 0, 0, 1), 8);
		CHECK_FALSE(compile_doc(u).ok());
		DocBuilder o(1, 4);
		const uint32_t sz = o.proc("UpdateSizeData", { { "Pivot", fxt::v2(0, 0) } });
		o.pre(sz, "PolynomialOperatorData", { { "Operation", raw(3) } });
		CHECK_FALSE(compile_doc(o).ok());
	}

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

	EmitterDef e = fxt::emitter_def("noage", 64, 50, 0.0f);
	e.slots[size_t(ProcessorType::UpdateTransparency)].present = true;
	e.slots[size_t(ProcessorType::UpdateTransparency)].evaluator = 0;
	EvaluatorDef p;
	p.type = EvaluatorType::Polynomial;
	p.polynomial = PolynomialDef{ { 1, 1, 1, 1 }, 0, 1, 1 };
	e.evaluators.push_back(p);
	EmitterDef zero = fxt::emitter_def("zerolife", 64, 50, 0.0f);
	zero.slots[size_t(ProcessorType::UpdateAge)].present = true;
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
	CHECK(w.begin_frame(1.0e30f, FxView{}) == 8);
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
	CHECK(ok == 4096);
	CHECK(w.stats().stolen_instances == 4096 - kMaxInstanceCount);
	for (int f = 0; f < 20; ++f) {
		fx_step_frame(w, 1.0f / 30.0f);
		CHECK(w.stats().live_particles <= w.config().max_particles);
		CHECK(w.particles().size() <= w.config().max_particles);
	}
	CHECK(w.stats().spawns_dropped > 0);
	CHECK(seconds_since(start) < kFastSeconds);
}

TEST_CASE("fuzz: hostile material grids load or fail, linear in the cells") {

	{
		fxt::MemResolver db;
		REQUIRE(db.set("fx/fuzz/impactfx", fxt::target(71)));
		uint32_t shared = 0;
		const fxt::Partition g = grid_partition(70, 3, 3, [&](fxt::Partition &r_p, size_t p_r, size_t p_c) -> std::vector<uint32_t> {
			if (p_r == 0 && p_c == 0) {
				return { effect_relation(r_p, 5, { r_p.ext(71, 0).word }) };
			}
			if (p_r == 1) {
				if (shared == 0) {
					shared = effect_relation(r_p, 30);
				}
				return { shared };
			}
			if (p_r == 2 && p_c == 2) {
				return { effect_relation(r_p, 30) };
			}
			return {};
		});
		WorkBudget budget;
		const auto t = load_grid(g, budget, &db);
		REQUIRE_MESSAGE(t.ok(), (t.ok() ? std::string() : t.error().message));
		CHECK(t->rows() == 3);
		CHECK(t->distinct_cells() == 3);
		REQUIRE(t->select(0, 0) != nullptr);
		CHECK(t->select(0, 0)->effects == std::vector<std::string>{ "fx/fuzz/impactfx" });
		CHECK(t->select(1, 2)->max_spread_degrees == 30);
		CHECK(&t->cell(1, 0) == &t->cell(2, 2));
		CHECK(t->select(0, 1)->empty());
		CHECK(t->select(3, 0) == nullptr);
	}

	{
		const auto distinct = [](size_t p_n) {
			return grid_partition(72, p_n, p_n, [p_n](fxt::Partition &r_p, size_t p_r, size_t p_c) -> std::vector<uint32_t> {
				return { effect_relation(r_p, float(p_r * p_n + p_c) * 1.0e-3f) };
			});
		};
		double best[2] = { 1e9, 1e9 };
		const size_t sizes[2] = { 128, 256 };
		for (int k = 0; k < 2; ++k) {
			fxt::MemResolver db;
			REQUIRE(db.set("fx/fuzz/grid", distinct(sizes[k])));
			for (int run = 0; run < 3; ++run) {
				WorkBudget budget;
				const auto start = std::chrono::steady_clock::now();
				const auto t = ImpactTable::load(db, "fx/fuzz/grid", budget);
				best[k] = std::min(best[k], seconds_since(start));
				REQUIRE_MESSAGE(t.ok(), (t.ok() ? std::string() : t.error().message));
				CHECK(t->distinct_cells() == sizes[k] * sizes[k] + 1);
				CHECK(budget.used() <= budget.limit());
				if (k == 1 && run == 0) {
					MESSAGE("256 x 256 distinct cells: budget used " << budget.used() << " of " << budget.limit());
				}
			}
		}
		MESSAGE("distinct-cell grids: 128 x 128 best " << best[0] << " s, 256 x 256 best " << best[1] << " s, ratio " << best[1] / best[0]);
		CHECK(best[1] < kFastSeconds);
		CHECK(best[1] / best[0] < 10.0);
	}

	{
		const fxt::Partition g = grid_partition(73, 256, 256, [](fxt::Partition &r_p, size_t, size_t) -> std::vector<uint32_t> { return { effect_relation(r_p, 45) }; });
		WorkBudget budget;
		const auto t = load_grid(g, budget);
		REQUIRE(t.ok());
		CHECK(t->distinct_cells() == 2);
	}

	const CellFn empty = [](fxt::Partition &, size_t, size_t) -> std::vector<uint32_t> { return {}; };
	{
		WorkBudget b1, b2, b3;
		CHECK_FALSE(load_grid(grid_partition(74, 257, 257, empty), b1).ok());
		CHECK_FALSE(load_grid(grid_partition(75, 4, 3, empty), b2).ok());
		CHECK_FALSE(load_grid(grid_partition(76, 4, 300, empty), b3).ok());
	}
	{

		fxt::MemResolver db;
		REQUIRE(db.set("fx/fuzz/impactfx", fxt::target(71)));
		WorkBudget budget;
		const CellFn external = [](fxt::Partition &r_p, size_t, size_t) -> std::vector<uint32_t> { return { r_p.ext(71, 0).word }; };
		CHECK_FALSE(load_grid(grid_partition(77, 2, 2, external), budget, &db).ok());
	}
	{

		const CellFn past = [](fxt::Partition &, size_t, size_t) -> std::vector<uint32_t> { return { 0x00FFFFFFu }; };
		const CellFn internal = [](fxt::Partition &r_p, size_t, size_t) -> std::vector<uint32_t> { return { effect_relation(r_p, 1, { r_p.ref(0).word }) }; };
		const CellFn nan = [](fxt::Partition &r_p, size_t, size_t) -> std::vector<uint32_t> {
			return { effect_relation(r_p, std::numeric_limits<float>::quiet_NaN()) };
		};
		const CellFn other = [](fxt::Partition &r_p, size_t, size_t) -> std::vector<uint32_t> { return { r_p.ref(0).word }; };
		WorkBudget b1, b2, b3, b4;
		CHECK_FALSE(load_grid(grid_partition(78, 2, 2, past), b1).ok());
		CHECK_FALSE(load_grid(grid_partition(79, 2, 2, internal), b2).ok());
		CHECK_FALSE(load_grid(grid_partition(80, 2, 2, nan), b3).ok());
		const auto t = load_grid(grid_partition(81, 2, 2, other), b4);
		REQUIRE(t.ok());
		CHECK(t->select(1, 1)->empty());
	}
	{

		WorkBudget budget{ 40 };
		const CellFn each = [](fxt::Partition &r_p, size_t p_r, size_t p_c) -> std::vector<uint32_t> { return { effect_relation(r_p, float(p_r * 16 + p_c)) }; };
		const auto t = load_grid(grid_partition(82, 16, 16, each), budget);
		REQUIRE_FALSE(t.ok());
		CHECK(t.error().message.find("budget") != std::string::npos);
	}

	{
		const auto start = std::chrono::steady_clock::now();
		uint32_t shared = 0;
		const std::vector<uint8_t> base = grid_partition(83, 6, 6, [&](fxt::Partition &r_p, size_t p_r, size_t p_c) -> std::vector<uint32_t> {
			if ((p_r + p_c) % 3 == 0) {
				return {};
			}
			if (p_r % 2 == 0) {
				if (shared == 0) {
					shared = effect_relation(r_p, 10);
				}
				return { shared };
			}
			return { effect_relation(r_p, float(p_c)) };
		}).build();
		const size_t from = payload_start(base);
		uint64_t rng = 0x1a7b;
		size_t parsed = 0, loaded = 0;
		for (int round = 0; round < 2000; ++round) {
			std::vector<uint8_t> bytes = base;
			mutate(bytes, round % 8 == 7 ? 0 : from, rng);
			fxt::MemResolver db;
			if (!db.set_bytes("fx/fuzz/grid", bytes)) {
				continue;
			}
			++parsed;
			WorkBudget budget;
			const auto t = ImpactTable::load(db, "fx/fuzz/grid", budget);
			CHECK(budget.used() <= budget.limit());
			if (!t.ok()) {
				continue;
			}
			++loaded;
			CHECK(t->rows() <= 256);
			CHECK(t->distinct_cells() <= t->rows() * t->rows() + 1);
			for (uint32_t a = 0; a < 8; ++a) {
				for (uint32_t b = 0; b < 8; ++b) {
					if (const ImpactCell *c = t->select(uint8_t(a), uint8_t(b))) {
						CHECK(std::isfinite(c->max_spread_degrees));
					}
				}
			}
		}
		MESSAGE("mutated grids: " << parsed << " parsed, " << loaded << " loaded");
		CHECK(parsed > 300);
		CHECK(seconds_since(start) < kFastSeconds);
	}
}

TEST_CASE("fuzz: hostile emitter systems and atlas texture headers") {

	{
		AtlasDb a;
		WorkBudget budget;
		const auto atlas = a.load(atlas_partition(84, 81, 82, valid_infos()), budget);
		REQUIRE_MESSAGE(atlas.ok(), (atlas.ok() ? std::string() : atlas.error().message));
		CHECK(atlas->base().width == 4096);
		CHECK(atlas->base().srgb);
		CHECK(atlas->normal().height == 2048);
		CHECK(atlas->normal().mips == 12);
		REQUIRE(atlas->entries().size() == 3);
		const AtlasEntry *flash = atlas->resolve("fx\\T\\FLASH");
		REQUIRE(flash != nullptr);
		CHECK(flash->kind == AtlasKind::Base);
		CHECK(flash->px_min_x == 1024);
		CHECK(flash->px_max_y == 3072);
		CHECK(flash->pixel_exact);
		const AtlasEntry *normal = atlas->resolve("fx/t/flash_n");
		REQUIRE(normal != nullptr);
		CHECK(normal->kind == AtlasKind::Normal);
		CHECK(normal->px_max_x == 512);
		CHECK(normal->px_max_y == 1024);
		CHECK(atlas->resolve("fx/t/defaultnormalmap")->degenerate);
		CHECK(atlas->resolve("fx/t/absent") == nullptr);
	}

	{
		AtlasDb a;
		const std::vector<AtlasInfo> many(4097, AtlasInfo{ "fx/t/x", 0, 0, 0.5f, 0.5f });
		WorkBudget budget;
		CHECK_FALSE(a.load(atlas_partition(85, 81, 82, many), budget).ok());
		CHECK(a.res_calls == 0);
	}

	{
		AtlasDb a;
		std::vector<AtlasInfo> many(4096, AtlasInfo{ "fx/t/x", 0, 0, 0.5f, 0.5f });
		many[7].max_x = 0.25f;
		WorkBudget budget;
		const auto atlas = a.load(atlas_partition(86, 81, 82, many), budget);
		REQUIRE(atlas.ok());
		CHECK(atlas->entries().size() == 4096);
		CHECK(atlas->resolve("fx/t/x") == &atlas->entries()[0]);
	}

	{
		AtlasDb a;
		WorkBudget b1, b2, b3, b4, b5, b6, b7;
		CHECK_FALSE(a.load(atlas_partition(87, 81, 82, valid_infos(), "TargetAsset"), b1).ok());
		CHECK_FALSE(a.load(atlas_partition(88, 81, 0, valid_infos()), b2).ok());
		a.res.erase("fx/fuzz/atlasnormal_n");
		CHECK_FALSE(a.load(atlas_partition(89, 81, 82, valid_infos()), b3).ok());
		a.res["fx/fuzz/atlasnormal_n"] = dx_header(20, 0x20, 1024, 2048, 12);
		REQUIRE(a.db.set("fx/fuzz/emittersystem_win32", atlas_partition(90, 81, 82, valid_infos())));
		CHECK_FALSE(EmitterAtlas::load(a.db, EmitterAtlas::ResSource{}, "fx/fuzz/emittersystem_win32", b4).ok());
		CHECK_FALSE(a.load(atlas_partition(91, 81, 82, { { "fx/t/nan", std::numeric_limits<float>::quiet_NaN(), 0, 1, 1 } }), b5).ok());
		CHECK_FALSE(a.load(atlas_partition(92, 81, 82, { { "fx/t/huge", 0, 0, 1.0e30f, 1 } }), b6).ok());

		const auto clamped = a.load(atlas_partition(93, 81, 82, { { "fx/t/out", -3, -3, 9, 9 } }), b7);
		REQUIRE(clamped.ok());
		CHECK(clamped->entries()[0].px_min_x == 0);
		CHECK(clamped->entries()[0].px_max_x == 4096);
	}

	{
		const auto bad = [](const std::vector<uint8_t> &p_res) { return !EmitterAtlas::texture_info("fx/t/h", p_res).ok(); };
		CHECK(bad({}));
		CHECK(bad(std::vector<uint8_t>(127, 0)));
		std::vector<uint8_t> v9 = dx_header(2, 0, 64, 64, 7);
		v9[0] = 9;
		CHECK(bad(v9));
		CHECK(bad(dx_header(31, 0, 64, 64, 7)));
		CHECK(bad(dx_header(2, 0, 64, 64, 16)));
		CHECK(bad(dx_header(2, 0, 0, 64, 7)));
		CHECK(bad(dx_header(2, 0, 64, 0, 7)));
		CHECK(bad(dx_header(2, 0, 20000, 64, 7)));
		CHECK(bad(dx_header(2, 0, 64, 65535, 7)));
		CHECK_FALSE(bad(dx_header(2, 0, 16384, 16384, 15)));
		uint64_t rng = 0xa71a5;
		size_t accepted = 0;
		for (int round = 0; round < 4000; ++round) {
			std::vector<uint8_t> res = dx_header(2, 0x22, 4096, 4096, 13);
			if (round % 4 == 3) {
				res.resize(size_t(splitmix(rng) % 200));
				for (uint8_t &b : res) {
					b = uint8_t(splitmix(rng));
				}
			} else {
				mutate(res, 0, rng);
			}
			const auto info = EmitterAtlas::texture_info("fx/t/h", res);
			if (info.ok()) {
				++accepted;
				CHECK(info->width >= 1);
				CHECK(info->width <= 16384);
				CHECK(info->height >= 1);
				CHECK(info->height <= 16384);
				CHECK(info->mips <= 15);
			}
		}
		MESSAGE("mutated texture headers: " << accepted << " of 4000 accepted");
	}

	{
		const auto start = std::chrono::steady_clock::now();
		const std::vector<uint8_t> base = atlas_partition(94, 81, 82, valid_infos()).build();
		const size_t from = payload_start(base);
		uint64_t rng = 0xa71a6;
		size_t parsed = 0, loaded = 0;
		for (int round = 0; round < 2000; ++round) {
			std::vector<uint8_t> bytes = base;
			mutate(bytes, round % 8 == 7 ? 0 : from, rng);
			AtlasDb a;
			WorkBudget budget;
			const auto atlas = a.load_bytes(std::move(bytes), budget);
			if (!atlas) {
				continue;
			}
			++parsed;
			CHECK(budget.used() <= budget.limit());
			if (!atlas->ok()) {
				continue;
			}
			++loaded;
			const EmitterAtlas &x = **atlas;
			CHECK(x.entries().size() <= 4096);
			for (const AtlasEntry &e : x.entries()) {
				const AtlasTextureInfo &t = e.kind == AtlasKind::Normal ? x.normal() : x.base();
				CHECK(e.px_max_x <= t.width);
				CHECK(e.px_max_y <= t.height);
				CHECK(std::isfinite(e.min_uv.x));
			}
		}
		MESSAGE("mutated systems: " << parsed << " parsed, " << loaded << " loaded");
		CHECK(parsed > 300);
		CHECK(seconds_since(start) < kFastSeconds);
	}
}

}
