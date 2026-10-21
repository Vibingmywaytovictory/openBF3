#include <doctest/doctest.h>

#include "bf3/core/gameplay/work_budget.h"
#include "bf3/core/util/splitmix64.h"
#include "bf3/fx/atlas_pixels.h"
#include "bf3/fx/emitter_atlas.h"
#include "bf3/fx/fx_harness.h"
#include "bf3/fx/render_prep.h"
#include "bf3/fx/simulator.h"
#include "fx_test_support.h"
#include "test_support.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace bf3::fx;
using bf3::core::gameplay::WorkBudget;

namespace {

using bf3::core::splitmix64_next;

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

fxt::Partition atlas_partition(uint32_t p_seed, uint32_t p_base_seed, uint32_t p_normal_seed, const std::vector<AtlasInfo> &p_infos) {
	fxt::Partition p;
	p.seed = p_seed;
	p.add({ "EmitterSystemAsset", {} });
	fxt::set(p.instances[0].values, "BaseAtlasTexture", p.ext(p_base_seed, 0));
	fxt::set(p.instances[0].values, "NormalAtlasTexture", p.ext(p_normal_seed, 0));
	fxt::Array infos{ "EmitterTextureAtlasInfo", {}, {} };
	for (const AtlasInfo &i : p_infos) {
		infos.structs.push_back({ { "TextureName", p.str(i.name) }, { "MinUv", fxt::v2(i.min_x, i.min_y) }, { "MaxUv", fxt::v2(i.max_x, i.max_y) } });
	}
	fxt::set(p.instances[0].values, "TextureInfos", p.array(std::move(infos)));
	return p;
}

EmitterAtlas make_atlas() {
	fxt::MemResolver db;
	REQUIRE(db.set("fx/r/atlasbase", fxt::target(81)));
	REQUIRE(db.set("fx/r/atlasnormal_n", fxt::target(82)));
	std::map<std::string, std::vector<uint8_t>> res;
	res["fx/r/atlasbase"] = dx_header(2, 0x22, 256, 256, 9);
	res["fx/r/atlasnormal_n"] = dx_header(20, 0x20, 64, 128, 8);
	const std::vector<AtlasInfo> infos{ { "FX/T/Smoke", 0.25f, 0.5f, 0.5f, 0.75f }, { "fx/t/smoke_n", 0.0f, 0.0f, 0.5f, 0.25f },
		{ "Systems/Emitter/DefaultTexture", 0.0f, 0.0f, 0.125f, 0.125f } };
	REQUIRE(db.set("fx/r/emittersystem_win32", atlas_partition(80, 81, 82, infos)));
	EmitterAtlas::ResSource source = [&res](const std::string &p_name) -> bf3::core::Result<std::vector<uint8_t>> {
		const auto it = res.find(p_name);
		if (it == res.end()) {
			return bf3::core::make_error(p_name + ": no such res");
		}
		return it->second;
	};
	WorkBudget budget;
	bf3::core::Result<EmitterAtlas> a = EmitterAtlas::load(db, source, "fx/r/emittersystem_win32", budget);
	REQUIRE_MESSAGE(a.ok(), (a.ok() ? std::string() : a.error().message));
	return std::move(*a);
}

EffectStart at(float p_x, float p_y, float p_z, uint64_t p_seed = 1, bool p_first_person = false) {
	EffectStart s;
	s.transform.trans = Vec3{ p_x, p_y, p_z };
	s.seed = p_seed;
	s.first_person = p_first_person;
	return s;
}

EmitterDef burst(const std::string &p_name, uint32_t p_count, EmittableType p_type = EmittableType::ScreenAlignedQuad, uint32_t p_flags = 0,
		const std::string &p_texture = "fx/t/smoke") {

	EmitterDef e = fxt::emitter_def(p_name, p_count, float(p_count - 1) * 100.0f + 1.0f, 100.0f, 0.01f);
	e.params.distribute_over_time = false;
	e.tmpl.emittable = p_type;
	e.tmpl.flags |= p_flags;
	e.texture.texture = p_texture;
	e.texture.normal_map = "fx/t/smoke_n";
	e.texture.frame_count = 8;
	e.texture.frame_columns = 4;
	return e;
}

FxView view_along_z() {
	FxView v;
	v.camera_position = Vec3{ 0, 0, 0 };
	v.camera_forward = Vec3{ 0, 0, 1 };
	return v;
}

uint32_t row_of(const FxInstance &p_in) {
	return uint32_t(p_in.custom[0]);
}

}

TEST_SUITE("fx-render-unit") {

TEST_CASE("batch keys: index round trip, classes, blends, passes") {
	std::set<size_t> seen;
	for (size_t i = 0; i < kMaxFxBatches; ++i) {
		const FxBatchKey k = fx_batch_key_at(i);
		CHECK(fx_batch_index(k) == i);
		seen.insert(fx_batch_index(k));
	}
	CHECK(seen.size() == kMaxFxBatches);
	EmitterDef e;
	e.tmpl.emittable = EmittableType::ScreenAlignedQuad;
	CHECK(fx_batch_key(e, false) == FxBatchKey{ FxShaderClass::Saq, FxBlend::Alpha, FxPass::World });
	e.tmpl.emittable = EmittableType::DirectionAlignedQuad;
	e.tmpl.flags = kEmitterEmissive;
	CHECK(fx_batch_key(e, true) == FxBatchKey{ FxShaderClass::Daq, FxBlend::Additive, FxPass::FirstPerson });
	e.tmpl.emittable = EmittableType::WorldAlignedQuad;
	CHECK(fx_batch_key(e, false).shader == FxShaderClass::Waq);
	e.tmpl.emittable = EmittableType::Quad;
	CHECK(fx_batch_key(e, false).shader == FxShaderClass::Waq);
	e.tmpl.emittable = EmittableType::ParticleMesh;
	CHECK(fx_batch_key(e, false).shader == FxShaderClass::Mesh);
	e.tmpl.emittable = EmittableType::Trail;
	CHECK(fx_batch_key(e, false).shader == FxShaderClass::Saq);
	e.tmpl.emittable = EmittableType::Point;
	CHECK(fx_batch_key(e, false).shader == FxShaderClass::Saq);
}

TEST_CASE("alpha levels: packing exact on the 10-bit grid, clamped, finite") {
	for (uint32_t a = 0; a <= kFxAlphaLevelSteps; a += 31) {
		for (uint32_t b = 0; b <= kFxAlphaLevelSteps; b += 37) {
			const float mn = float(a) / float(kFxAlphaLevelSteps), mx = float(b) / float(kFxAlphaLevelSteps);
			const float packed = fx_pack_alpha_levels(mn, mx);
			CHECK(packed == float(a * (kFxAlphaLevelSteps + 1) + b));
			float umn = -1, umx = -1;
			fx_unpack_alpha_levels(packed, umn, umx);
			CHECK(std::fabs(umn - mn) < 1e-6f);
			CHECK(std::fabs(umx - mx) < 1e-6f);
		}
	}
	float mn = 0, mx = 0;
	fx_unpack_alpha_levels(fx_pack_alpha_levels(-3.0f, 7.0f), mn, mx);
	CHECK(mn == 0.0f);
	CHECK(mx == 1.0f);
	fx_unpack_alpha_levels(fx_pack_alpha_levels(std::nanf(""), std::nanf("")), mn, mx);
	CHECK(mn == 0.0f);
	CHECK(mx == 1.0f);

	CHECK(fx_pack_alpha_levels(1.0f, 1.0f) == float(kFxAlphaLevelSteps * (kFxAlphaLevelSteps + 1) + kFxAlphaLevelSteps));
}

TEST_CASE("prepare: instance count, batch order, back-to-front sort, rows and params") {
	const EmitterAtlas atlas = make_atlas();
	FxWorld w;

	const auto alpha = fxt::effect_def("alpha", { burst("a", 3) });
	const auto add = fxt::effect_def("add", { burst("b", 2, EmittableType::ScreenAlignedQuad, kEmitterEmissive) });
	const auto daq = fxt::effect_def("daq", { burst("c", 4, EmittableType::DirectionAlignedQuad) });
	REQUIRE(w.start(alpha, at(0, 0, 5, 1)).ok());
	REQUIRE(w.start(fxt::effect_def("alpha2", { burst("a", 3) }), at(0, 0, 20, 2)).ok());
	REQUIRE(w.start(add, at(0, 0, 30, 3)).ok());
	REQUIRE(w.start(daq, at(1, 0, 8, 4)).ok());
	REQUIRE(w.start(fxt::effect_def("fp", { burst("d", 2) }), at(0, 0, 1, 5, true)).ok());
	fx_step_frame(w, 1.0f / 60.0f);
	REQUIRE(w.particles().size() == 3 + 3 + 2 + 4 + 2);

	RenderPrep prep;
	prep.prepare(w, view_along_z(), atlas);
	CHECK(prep.instances().size() == w.particles().size());
	CHECK(prep.stats().instances == w.particles().size());
	CHECK(prep.emitter_params().size() == w.outputs().size());
	CHECK(prep.stats().unresolved_this_frame == 0);
	CHECK(prep.stats().first_person_instances == 2);

	const auto batches = prep.batches();
	REQUIRE(batches.size() == 4);
	CHECK(batches[0].key == FxBatchKey{ FxShaderClass::Saq, FxBlend::Alpha, FxPass::World });
	CHECK(batches[1].key == FxBatchKey{ FxShaderClass::Daq, FxBlend::Alpha, FxPass::World });
	CHECK(batches[2].key == FxBatchKey{ FxShaderClass::Saq, FxBlend::Additive, FxPass::World });
	CHECK(batches[3].key == FxBatchKey{ FxShaderClass::Saq, FxBlend::Alpha, FxPass::FirstPerson });
	uint32_t next = 0;
	for (const FxBatch &b : batches) {
		CHECK(b.first_instance == next);
		next += b.instance_count;
	}
	CHECK(next == prep.instances().size());
	CHECK(batches[0].instance_count == 6);
	CHECK(batches[0].emitters == 2);

	const auto inst = prep.instances();
	const auto outs = w.outputs();
	const auto parts = w.particles();
	const EmitterOutput &first = outs[row_of(inst[0])];
	CHECK(first.emitter_transform.trans.z == doctest::Approx(20.0f));
	CHECK(outs[row_of(inst[3])].emitter_transform.trans.z == doctest::Approx(5.0f));
	for (uint32_t k = 0; k < 3; ++k) {
		const ParticleVertex &p = parts[first.first_particle + k];
		const FxInstance &in = inst[k];
		CHECK(row_of(in) == row_of(inst[0]));

		CHECK(in.transform[3] == p.position.x);
		CHECK(in.transform[7] == p.position.y);
		CHECK(in.transform[11] == p.position.z);
		CHECK(in.transform[0] == doctest::Approx(p.size.x * std::cos(p.rotation)));
		CHECK(in.transform[4] == doctest::Approx(p.size.x * std::sin(p.rotation)));
		CHECK(in.transform[1] == doctest::Approx(-p.size.y * std::sin(p.rotation)));
		CHECK(in.transform[5] == doctest::Approx(p.size.y * std::cos(p.rotation)));
		CHECK(in.color[0] == p.color.x);
		CHECK(in.color[3] == p.color.w);
		CHECK(in.custom[1] == p.frame);
		CHECK(in.custom[2] == fx_pack_alpha_levels(p.alpha_min, p.alpha_max));
		CHECK(in.custom[3] == p.alpha_scale);
	}

	const FxEmitterParams &row = prep.emitter_params()[row_of(inst[0])];
	CHECK(row.base_rect[0] == doctest::Approx(0.25f));
	CHECK(row.base_rect[1] == doctest::Approx(0.5f));
	CHECK(row.base_rect[2] == doctest::Approx(0.25f));
	CHECK(row.base_rect[3] == doctest::Approx(0.25f));
	CHECK(row.flipbook[0] == 8.0f);
	CHECK(row.flipbook[1] == 4.0f);
	CHECK(row.normal_rect[2] == doctest::Approx(0.5f));
	CHECK(row.normal_rect[3] == doctest::Approx(0.25f));
	CHECK(row.lighting[2] == 0.0f);
	const FxBatch &addb = batches[2];
	CHECK(prep.emitter_params()[row_of(inst[addb.first_instance])].lighting[2] == 1.0f);

	const FxInstance &d = inst[batches[1].first_instance];
	const float zl = std::sqrt(d.transform[2] * d.transform[2] + d.transform[6] * d.transform[6] + d.transform[10] * d.transform[10]);
	CHECK(zl == doctest::Approx(1.0f));
	CHECK(d.transform[4] == 0.0f);
	CHECK(d.transform[1] == 0.0f);

	for (const FxBatch &b : batches) {
		for (uint32_t k = 0; k < b.instance_count; ++k) {
			const FxInstance &in = inst[b.first_instance + k];
			CHECK(in.transform[3] >= b.aabb_min.x);
			CHECK(in.transform[3] <= b.aabb_max.x);
			CHECK(in.transform[11] >= b.aabb_min.z);
			CHECK(in.transform[11] <= b.aabb_max.z);
		}
	}

	FxView back;
	back.camera_position = Vec3{ 0, 0, 40 };
	back.camera_forward = Vec3{ 0, 0, -1 };
	prep.prepare(w, back, atlas);
	CHECK(outs[row_of(prep.instances()[0])].emitter_transform.trans.z == doctest::Approx(5.0f));
	CHECK(prep.instances().size() == w.particles().size());
}

TEST_CASE("prepare: unresolved textures fall back to the default entry and are counted") {
	const EmitterAtlas atlas = make_atlas();
	FxWorld w;
	REQUIRE(w.start(fxt::effect_def("missing", { burst("m", 2, EmittableType::ScreenAlignedQuad, 0, "fx/t/nothere") }), at(0, 0, 4)).ok());
	REQUIRE(w.start(fxt::effect_def("empty", { burst("e", 1, EmittableType::ScreenAlignedQuad, 0, "") }), at(0, 0, 6, 2)).ok());

	REQUIRE(w.start(fxt::effect_def("normal", { burst("n", 1, EmittableType::ScreenAlignedQuad, 0, "fx/t/smoke_n") }), at(0, 0, 7, 3)).ok());
	fx_step_frame(w, 1.0f / 60.0f);
	RenderPrep prep;
	prep.prepare(w, view_along_z(), atlas);
	CHECK(prep.stats().unresolved_this_frame == 3);
	CHECK(prep.stats().unresolved_textures == 3);
	for (const FxEmitterParams &row : prep.emitter_params()) {
		CHECK(row.base_rect[2] == doctest::Approx(0.125f));
	}
	CHECK(prep.instances().size() == w.particles().size());
	prep.prepare(w, view_along_z(), atlas);
	CHECK(prep.stats().unresolved_textures == 6);
	CHECK(prep.stats().frames == 2);
}

TEST_CASE("prepare: Waq and mesh stand-in transforms") {
	const EmitterAtlas atlas = make_atlas();
	FxWorld w;
	EmitterDef waq = burst("w", 2, EmittableType::WorldAlignedQuad);
	EmitterDef mesh = burst("m", 3, EmittableType::ParticleMesh);
	REQUIRE(w.start(fxt::effect_def("waq", { waq }), at(0, 0, 3)).ok());
	REQUIRE(w.start(fxt::effect_def("mesh", { mesh }), at(0, 0, 4, 2)).ok());
	fx_step_frame(w, 1.0f / 60.0f);
	RenderPrep prep;
	prep.prepare(w, view_along_z(), atlas);
	CHECK(prep.stats().mesh_standins == 3);

	FxWorld w2;
	REQUIRE(w2.start(fxt::effect_def("bare", { burst("b", 2, EmittableType::ParticleMesh, 0, "") }), at(0, 0, 4, 3)).ok());
	fx_step_frame(w2, 1.0f / 60.0f);
	RenderPrep prep2;
	prep2.prepare(w2, view_along_z(), atlas);
	CHECK(prep2.stats().unresolved_this_frame == 0);
	CHECK(prep2.stats().mesh_standins == 2);
	REQUIRE(prep2.emitter_params().size() == 1);
	CHECK(prep2.emitter_params()[0].base_rect[2] == kFxRectWhite);
	for (const FxBatch &b : prep.batches()) {
		const FxInstance &in = prep.instances()[b.first_instance];
		if (b.key.shader == FxShaderClass::Waq) {

			CHECK(in.transform[2] == 0.0f);
			CHECK(in.transform[6] == 1.0f);
			CHECK(in.transform[10] == 0.0f);
		}
		if (b.key.shader == FxShaderClass::Mesh) {

			for (uint32_t i = 0; i < b.instance_count; ++i) {
				const float *t = prep.instances()[b.first_instance + i].transform;
				for (int c = 0; c < 3; ++c) {
					CHECK(std::sqrt(t[c] * t[c] + t[4 + c] * t[4 + c] + t[8 + c] * t[8 + c]) == doctest::Approx(kFxMeshStandinMeters));
				}
			}
		}
	}
}

TEST_CASE("lights: merged per output, intensity-weighted, priority, the 64 pool") {
	const EmitterAtlas atlas = make_atlas();
	FxWorldConfig cfg;
	FxWorld w(cfg);
	EmitterDef e = burst("l", 4, EmittableType::ScreenAlignedQuad, kEmitterActAsPointLight | kEmitterEmissive);
	e.tmpl.light.radius = 5;
	e.tmpl.light.color = Vec3{ 1, 0.5f, 0.25f };
	e.tmpl.light.pivot = Vec3{ 0, 2, 0 };
	const auto def = fxt::effect_def("light", { e }, { 0 }, 100);

	for (uint32_t i = 0; i < 70; ++i) {
		REQUIRE(w.start(def, at(0, 0, 10.0f + float(i), 100 + i)).ok());
	}
	fx_step_frame(w, 1.0f / 60.0f);
	REQUIRE(w.lights().size() == 70 * 4);
	RenderPrep prep;
	prep.prepare(w, view_along_z(), atlas);
	CHECK(prep.stats().light_candidates == 70);
	CHECK(prep.lights().size() == kMaxFxLights);
	CHECK(prep.stats().lights_dropped == 70 - kMaxFxLights);
	float last = std::numeric_limits<float>::max();
	for (const FxLight &l : prep.lights()) {
		CHECK(l.priority <= last);
		last = l.priority;
		CHECK(l.radius == 5.0f);
		CHECK(l.intensity == doctest::Approx(4.0f));
		CHECK(l.color.x == doctest::Approx(1.0f));
		CHECK(l.color.y == doctest::Approx(0.5f));
		CHECK(l.color.z == doctest::Approx(0.25f));
		CHECK(l.position.y == doctest::Approx(2.0f));
		CHECK(l.position.z < 10.0f + 64.0f);
		CHECK_FALSE(l.first_person);
	}

	FxWorld w2;
	REQUIRE(w2.start(def, at(0, -2, 0, 7, true)).ok());
	fx_step_frame(w2, 1.0f / 60.0f);
	prep.prepare(w2, view_along_z(), atlas);
	REQUIRE(prep.lights().size() == 1);
	CHECK(std::isfinite(prep.lights()[0].priority));
	CHECK(prep.lights()[0].priority == doctest::Approx(4.0f / kFxLightMinDistance2));
	CHECK(prep.lights()[0].first_person);
}

TEST_CASE("atlas pixels: NormalDXT5RGA -> BC5 on synthetic blocks") {

	std::vector<uint8_t> block(16, 0);
	const uint8_t alpha[8] = { 200, 10, 0x24, 0x92, 0x49, 0x24, 0x92, 0x49 };
	std::memcpy(block.data(), alpha, 8);
	const uint16_t c0 = uint16_t(31 << 11), c1 = 0;
	std::memcpy(block.data() + 8, &c0, 2);
	std::memcpy(block.data() + 10, &c1, 2);
	const uint32_t idx = 0xE4E4E4E4u;
	std::memcpy(block.data() + 12, &idx, 4);
	std::vector<uint8_t> out;
	WorkBudget budget;
	REQUIRE(convert_normal_dxt5rga_to_bc5(block.data(), block.size(), 4, 4, out, budget).ok());
	REQUIRE(out.size() == 16);

	CHECK(std::memcmp(out.data() + 8, alpha, 8) == 0);

	CHECK(out[0] == 255);
	CHECK(out[1] == 0);
	uint64_t bits = 0;
	for (int i = 0; i < 6; ++i) {
		bits |= uint64_t(out[2 + i]) << (8 * i);
	}
	const int expect[4] = { 255, 0, 170, 85 };
	for (int i = 0; i < 16; ++i) {
		const uint32_t k = uint32_t((bits >> (3 * i)) & 7);
		const int v = k == 0 ? 255 : (k == 1 ? 0 : ((7 - int(k - 1)) * 255 + int(k - 1) * 0 + 3) / 7);
		CHECK(std::abs(v - expect[i % 4]) <= 19);
	}

	std::vector<uint8_t> flat(16, 0);
	const uint16_t mid = uint16_t(16 << 11);
	std::memcpy(flat.data() + 8, &mid, 2);
	std::memcpy(flat.data() + 10, &mid, 2);
	out.clear();
	REQUIRE(convert_normal_dxt5rga_to_bc5(flat.data(), flat.size(), 4, 4, out, budget).ok());
	CHECK(out[0] == out[1]);

	out.clear();
	CHECK_FALSE(convert_normal_dxt5rga_to_bc5(block.data(), 15, 4, 4, out, budget).ok());
	CHECK_FALSE(convert_normal_dxt5rga_to_bc5(block.data(), 16, 8, 4, out, budget).ok());
	CHECK_FALSE(convert_normal_dxt5rga_to_bc5(block.data(), 16, 0, 4, out, budget).ok());
	CHECK_FALSE(convert_normal_dxt5rga_to_bc5(nullptr, 16, 4, 4, out, budget).ok());
	CHECK_FALSE(convert_normal_dxt5rga_to_bc5(block.data(), 16, 1u << 20, 4, out, budget).ok());
	CHECK(out.empty());

	std::vector<uint8_t> big(atlas_level_bytes(64, 64), 0);
	WorkBudget tiny(3);
	CHECK_FALSE(convert_normal_dxt5rga_to_bc5(big.data(), big.size(), 64, 64, out, tiny).ok());
	CHECK(out.empty());
	CHECK(atlas_level_bytes(1, 1) == 16);
	CHECK(atlas_level_bytes(5, 3) == 32);
}

namespace {

bf3::core::res::LoadedTexture synthetic_texture(bf3::core::res::TextureFormat p_format, uint16_t p_w, uint16_t p_h, uint8_t p_mips) {
	bf3::core::res::LoadedTexture t;
	t.texture.kind = bf3::core::res::TextureKind::Texture2D;
	t.texture.format = p_format;
	t.texture.width = p_w;
	t.texture.height = p_h;
	t.texture.depth = 1;
	t.texture.slice_count = 1;
	t.texture.mip_count = p_mips;
	uint64_t rng = 99;
	for (uint8_t m = 0; m < p_mips; ++m) {
		const uint64_t n = atlas_level_bytes(std::max(1u, uint32_t(p_w) >> m), std::max(1u, uint32_t(p_h) >> m));
		t.texture.mip_sizes.push_back(uint32_t(n));
		for (uint64_t i = 0; i < n; ++i) {
			t.chunk.bytes.push_back(uint8_t(splitmix64_next(rng)));
		}
	}
	t.texture.mip_chain_size = uint32_t(t.chunk.bytes.size());
	return t;
}

AtlasTextureInfo info_of(const bf3::core::res::LoadedTexture &p_t, const std::string &p_name, bool p_srgb) {
	AtlasTextureInfo i;
	i.partition = p_name;
	i.format = p_t.texture.format;
	i.srgb = p_srgb;
	i.width = p_t.texture.width;
	i.height = p_t.texture.height;
	i.mips = p_t.texture.mip_count;
	return i;
}

AtlasPixels synthetic_pixels() {
	WorkBudget budget;
	const auto base = synthetic_texture(bf3::core::res::TextureFormat::Dxt5, 64, 64, 7);
	const auto normal = synthetic_texture(bf3::core::res::TextureFormat::NormalDxt5Rga, 32, 64, 7);
	AtlasPixels px;
	auto b = prepare_atlas_image(base, info_of(base, "fx/r/atlasbase", true), budget);
	auto n = prepare_atlas_image(normal, info_of(normal, "fx/r/atlasnormal_n", false), budget);
	REQUIRE(b.ok());
	REQUIRE(n.ok());
	px.base = std::move(*b);
	px.normal = std::move(*n);
	return px;
}

}

TEST_CASE("atlas pixels: image preparation from a synthetic texture") {
	WorkBudget budget;
	const auto base = synthetic_texture(bf3::core::res::TextureFormat::Dxt5, 64, 64, 7);
	auto b = prepare_atlas_image(base, info_of(base, "fx/r/atlasbase", true), budget);
	REQUIRE(b.ok());
	CHECK(b->format == bf3::core::res::PixelFormat::Bc3);
	CHECK(b->srgb);
	CHECK(b->mips.size() == 7);
	CHECK(b->bytes == base.chunk.bytes);
	CHECK(b->mips[6].width == 1);
	CHECK(b->mips[6].size == 16);

	const auto normal = synthetic_texture(bf3::core::res::TextureFormat::NormalDxt5Rga, 32, 64, 7);
	auto n = prepare_atlas_image(normal, info_of(normal, "fx/r/atlasnormal_n", true), budget);
	REQUIRE(n.ok());
	CHECK(n->format == bf3::core::res::PixelFormat::Bc5);
	CHECK_FALSE(n->srgb);
	CHECK(n->bytes.size() == normal.chunk.bytes.size());

	for (size_t at = 0; at < n->bytes.size(); at += 16) {
		CHECK(std::memcmp(n->bytes.data() + at + 8, normal.chunk.bytes.data() + at, 8) == 0);
	}

	AtlasTextureInfo wrong = info_of(base, "x", true);
	wrong.width = 128;
	CHECK_FALSE(prepare_atlas_image(base, wrong, budget).ok());
	auto dxt1 = base;
	dxt1.texture.format = bf3::core::res::TextureFormat::Dxt1;
	CHECK_FALSE(prepare_atlas_image(dxt1, info_of(dxt1, "x", true), budget).ok());
	auto short_chunk = base;
	short_chunk.chunk.bytes.resize(short_chunk.chunk.bytes.size() - 1);
	CHECK_FALSE(prepare_atlas_image(short_chunk, info_of(short_chunk, "x", true), budget).ok());
	AtlasTextureInfo more = info_of(base, "x", true);
	more.mips = 8;
	CHECK_FALSE(prepare_atlas_image(base, more, budget).ok());
}

TEST_CASE("atlas pixels: blob round trip, every truncation, seeded mutations") {
	const AtlasPixels px = synthetic_pixels();
	const std::vector<uint8_t> blob = encode_atlas_pixels(px);
	AtlasPixels back;
	REQUIRE(decode_atlas_pixels(blob, back).ok());
	CHECK(back.base.bytes == px.base.bytes);
	CHECK(back.normal.bytes == px.normal.bytes);
	CHECK(back.base.partition == px.base.partition);
	CHECK(back.base.srgb);
	CHECK_FALSE(back.normal.srgb);
	CHECK(back.normal.mips.size() == px.normal.mips.size());
	CHECK(encode_atlas_pixels(back) == blob);

	size_t failures = 0;
	for (size_t n = 0; n < blob.size(); n += (n < 512 ? 1 : 97)) {
		std::vector<uint8_t> cut(blob.begin(), blob.begin() + ptrdiff_t(n));
		AtlasPixels x;
		if (!decode_atlas_pixels(cut, x).ok()) {
			++failures;
		} else {
			FAIL("a truncated blob decoded at " << n);
		}
	}
	CHECK(failures > 0);

	std::vector<uint8_t> longer = blob;
	longer.push_back(0);
	CHECK_FALSE(decode_atlas_pixels(longer, back).ok());

	uint64_t rng = 0xF2;
	size_t decoded = 0;
	for (int round = 0; round < 4000; ++round) {
		std::vector<uint8_t> m = blob;
		const size_t flips = 1 + splitmix64_next(rng) % 4;
		for (size_t k = 0; k < flips; ++k) {
			const uint64_t r = splitmix64_next(rng);
			const size_t limit = std::min<size_t>(m.size(), 400);
			const size_t at = size_t(r % limit);
			switch ((r >> 32) % 4) {
				case 0:
					m[at] ^= uint8_t(1u << ((r >> 40) % 8));
					break;
				case 1:
					m[at] = 0xFF;
					break;
				case 2:
					if (at + 4 <= m.size()) {
						const uint32_t big = 0x7FFFFFFFu;
						std::memcpy(m.data() + at, &big, 4);
					}
					break;
				default:
					m[at] = 0;
					break;
			}
		}
		AtlasPixels x;
		if (decode_atlas_pixels(m, x).ok()) {
			++decoded;
			CHECK(x.base.bytes.size() <= kAtlasPixelsMaxBytes);
			uint64_t sum = 0;
			for (const AtlasMip &mip : x.base.mips) {
				sum += mip.size;
			}
			CHECK(sum == x.base.bytes.size());
		}
	}
	MESSAGE("mutations that still decoded: " << decoded << " of 4000");

	std::vector<uint8_t> empty;
	CHECK_FALSE(decode_atlas_pixels(empty, back).ok());
}

}
