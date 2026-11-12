#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/install_snapshot.h"
#include "bf3/core/res/terrain.h"
#include "bf3/core/superbundle_mount.h"
#include "bf3/render/dxbc.h"
#include "bf3/render/look_materials.h"
#include "bf3/render/shader_translate.h"
#include "bf3/render/terrain_look.h"
#include "dxbc_test_support.h"
#include "test_support.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

using namespace bf3;
using namespace bf3::core;
using namespace bf3::render;
using namespace dxbct;

namespace {

SceneTerrainCombo combo(std::vector<uint8_t> p_layers) {
	SceneTerrainCombo c;
	c.layers = std::move(p_layers);
	return c;
}

}

TEST_SUITE("render-terrain-unit") {
	TEST_CASE("terrain: combination names give their layers") {
		const std::string asset = "Levels/MP_007/Terrain/MP_007";
		const std::optional<std::vector<uint8_t>> three =
				terrain_combo_layers("levels/mp_007/terrain/mp_007__0MV_5MV_9MV__2d__0", asset);
		REQUIRE(three.has_value());
		CHECK(*three == std::vector<uint8_t>{ 0, 5, 9 });
		CHECK(terrain_combo_layers("Levels/MP_007/Terrain/MP_007__7MV__2d__0", asset) == std::vector<uint8_t>{ 7 });
		CHECK_FALSE(terrain_combo_layers("Levels/MP_007/Terrain/MP_007__0MD_5MD_9M__3d__0", asset).has_value());
		CHECK_FALSE(terrain_combo_layers("Levels/MP_007/Terrain/MP_007__1MS_5M__MaskScale2d__0", asset).has_value());
		CHECK_FALSE(terrain_combo_layers("Levels/MP_007/Terrain/MP_007__9MV__2d__1", asset).has_value());
		CHECK_FALSE(terrain_combo_layers("Levels/MP_011/Terrain/MP_011__0MV__2d__0", asset).has_value());
		CHECK_FALSE(terrain_combo_layers("Levels/MP_007/Terrain/MP_007__MV__2d__0", asset).has_value());
	}

	TEST_CASE("terrain: a patch takes the exact combination, else the smallest superset, else the visible layers") {
		const std::vector<SceneTerrainCombo> combos = { combo({ 0 }), combo({ 0, 5 }), combo({ 0, 5, 9 }), combo({ 7 }),
			combo({ 5, 9 }) };
		TerrainChoiceResult r = choose_terrain_combo(combos, { 0, 5 }, {});
		CHECK(r.combo == 1);
		CHECK(r.kind == TerrainChoice::Exact);
		r = choose_terrain_combo(combos, { 0, 9 }, {});
		CHECK(r.combo == 2);
		CHECK(r.kind == TerrainChoice::Superset);
		r = choose_terrain_combo(combos, { 0, 3, 7 }, { 7 });
		CHECK(r.combo == 3);
		CHECK(r.kind == TerrainChoice::Opaque);
		r = choose_terrain_combo(combos, { 3, 4 }, {});
		CHECK(r.kind == TerrainChoice::Miss);
	}

	TEST_CASE("terrain: a pixel program becomes shader statements") {
		const std::vector<uint32_t> shader = {
			0x00000041u,
			0u,
			(101u | (3u << 24)),
			0x001020F2u,
			0u,
			(54u | (8u << 24)),
			0x001020F2u,
			0u,
			0x00004002u,
			0x3F800000u,
			0u,
			0xBF000000u,
			0x3F800000u,
			(62u | (1u << 24)),
		};
		std::vector<uint32_t> tokens = shader;
		tokens[1] = uint32_t(tokens.size());
		Result<dxbc::Program> program = dxbc::parse(container({ { "SHDR", words(tokens) } }));
		REQUIRE(program.ok());
		Result<PixelTranslation> t = translate_pixel_program(*program);
		REQUIRE_MESSAGE(t.ok(), (t.ok() ? std::string() : t.error().message));
		CHECK(t->outputs == 1);
		CHECK(t->instructions == 2);
		CHECK(t->body.find("o0.xyzw = ((vec4(1.0, 0.0, (-0.5), 1.0))).xyzw;") != std::string::npos);
		const auto with_and = [&](uint32_t p_mask) {
			std::vector<uint32_t> and_tokens(shader.begin(), shader.begin() + 5);
			and_tokens.insert(and_tokens.end(),
					{ (1u | (13u << 24)), 0x001020F2u, 0u, 0x00004002u, 0x3F800000u, 0x3F800000u, 0x3F800000u,
							0x3F800000u, 0x00004002u, p_mask, p_mask, p_mask, p_mask, (62u | (1u << 24)) });
			and_tokens[1] = uint32_t(and_tokens.size());
			Result<dxbc::Program> p = dxbc::parse(container({ { "SHDR", words(and_tokens) } }));
			REQUIRE(p.ok());
			return translate_pixel_program(*p);
		};
		CHECK(with_and(0x3F800000u).ok());
		CHECK_FALSE(with_and(0x000000FFu).ok());
		tokens[5] = 87u | (8u << 24);
		program = dxbc::parse(container({ { "SHDR", words(tokens) } }));
		REQUIRE(program.ok());
		CHECK_FALSE(translate_pixel_program(*program).ok());
		CHECK_FALSE(dxbc::parse(std::vector<uint8_t>(16, 0)).ok());
	}

	TEST_CASE("terrain: a pixel program with branches, gradients and an indexed temp translates, and a sample with an "
			  "offset is refused") {
		const std::vector<uint32_t> x0_1 = { 2u | kMaskMode << 2 | 0xFu << 4 | 3u << 12 | 2u << 20, 0, 1 };
		std::vector<uint32_t> x0_1_src = x0_1;
		x0_1_src[0] = 2u | kSwizzleMode << 2 | kIdentity << 4 | 3u << 12 | 2u << 20;
		const std::vector<uint32_t> v1_x = { 2u | 2u << 2 | kInput << 12 | 1u << 20, 1 };
		const auto program = [&](bool p_offset, bool p_endif) {
			std::vector<uint32_t> sample_d = ins(73,
					{ dst(kTemp, 0, 0xF), src(kInput, 1, swz(0, 1, 0, 0)), src(kResource, 0), sampler(0),
							src(kInput, 1, swz(0, 1, 0, 0)), src(kInput, 1, swz(0, 1, 0, 0)) });
			if (p_offset) {
				sample_d.insert(sample_d.begin() + 1, 1u | 1u << 9);
				sample_d[0] = 73u | uint32_t(sample_d.size()) << 24 | 1u << 31;
			}
			std::vector<std::vector<uint32_t>> code = { ins(101, { dst(kOutput, 0, 0xF) }),
				{ 105u | 4u << 24, 0, 2, 4 }, ins(54, { x0_1, imm(0.25f, 0.5f, 0.75f, 1.0f) }), ins(31, { v1_x }),
				sample_d, ins(18, {}), ins(54, { dst(kTemp, 0, 0xF), x0_1_src }) };
			code[3][0] |= 1u << 18;
			if (p_endif) {
				code.push_back(ins(21, {}));
			}
			code.push_back(ins(54, { dst(kOutput, 0, 0xF), src(kTemp, 0) }));
			return container({ { "SHDR", code_chunk(0, 1, code) } });
		};
		Result<dxbc::Program> p = dxbc::parse(program(false, true));
		REQUIRE_MESSAGE(p.ok(), (p.ok() ? std::string() : p.error().message));
		CHECK(p->declared_outputs == 1);
		CHECK(p->indexable_temps == std::vector<std::pair<uint32_t, uint32_t>>{ { 0, 2 } });
		CHECK(dxbc::format_instruction(p->code[1]) == "if_nz v1.x");
		Result<PixelTranslation> t = translate_pixel_program(*p);
		REQUIRE_MESSAGE(t.ok(), (t.ok() ? std::string() : t.error().message));
		CHECK(t->outputs == 1);
		CHECK(t->instructions == 8);
		CHECK(t->textures == std::vector<uint32_t>{ 0 });
		CHECK(t->inputs == std::vector<uint32_t>{ 1 });
		const std::string &b = t->body;
		CHECK(b.find("vec4 x0[2];") != std::string::npos);
		CHECK(b.find("x0[1].xyzw = ((vec4(0.25, 0.5, 0.75, 1.0))).xyzw;") != std::string::npos);
		CHECK(b.find("if ((v1.xxxx).x != 0.0) {") != std::string::npos);
		CHECK(b.find("r0.xyzw = (textureGrad(t0, (v1.xyxx).xy, (v1.xyxx).xy, (v1.xyxx).xy).xyzw).xyzw;") !=
				std::string::npos);
		CHECK(b.find("} else {\nr0.xyzw = ((x0[1].xyzw)).xyzw;\n}\n") != std::string::npos);
		Result<dxbc::Program> open = dxbc::parse(program(false, false));
		REQUIRE(open.ok());
		CHECK(!translate_pixel_program(*open).ok());
		Result<dxbc::Program> offset = dxbc::parse(program(true, true));
		REQUIRE(offset.ok());
		CHECK(dxbc::format_instruction(offset->code[2]).starts_with("sample_d_aoffimmi(1,0,0) r0.xyzw"));
		Result<PixelTranslation> refused = translate_pixel_program(*offset);
		REQUIRE(!refused.ok());
		CHECK(refused.error().message.find("sample offsets") != std::string::npos);
	}
}

TEST_SUITE("render-terrain") {
	TEST_CASE("terrain: MP_007 layer masks, colour map and combinations from the install") {
		test::Mounted m;
		if (!test::open_or_skip(m)) {
			return;
		}
		Result<LevelMount> mount = m.install->mount_superbundle("Win32/Levels/MP_007/MP_007");
		REQUIRE(mount.ok());
		REQUIRE(mount->load_all_bundles().ok());
		const std::string asset = "Levels/MP_007/Terrain/MP_007";
		auto terrain = mount->read_res(asset);
		REQUIRE(terrain.ok());
		std::string tree_name;
		REQUIRE(core::res::terrain_streaming_tree_name(terrain->second, tree_name).ok());
		auto tree_bytes = mount->read_res(tree_name);
		REQUIRE(tree_bytes.ok());
		core::res::TerrainStreamingTree tree;
		const core::res::ResError parsed = core::res::TerrainStreamingTree::parse(tree_bytes->second, tree);
		REQUIRE_MESSAGE(parsed.ok(), parsed.message());
		REQUIRE(tree.mask.has_value());
		REQUIRE(tree.colour.has_value());
		const core::res::TerrainMaskTree &mask = *tree.mask;
		CHECK(mask.samples_per_side == 66);
		CHECK(mask.nodes.size() == 173);
		CHECK(mask.layer_count() == 10);
		CHECK(mask.max_level() == 6);
		REQUIRE(mask.nodes.front().persistent.has_value());
		CHECK(mask.nodes.front().tiles == 33);
		CHECK(tree.colour->texels_per_side == 132);
		CHECK(tree.colour->nodes.size() == 213);

		std::map<std::array<uint8_t, 16>, std::vector<uint8_t>> chunks;
		const core::res::TerrainChunkFetch fetch =
				[&](const core::res::ChunkId &p_id) -> std::optional<std::vector<uint8_t>> {
			auto found = chunks.find(p_id.bytes);
			if (found == chunks.end()) {
				Result<std::vector<uint8_t>> c = mount->read_chunk(Guid::from_bytes(p_id.bytes.data()));
				if (!c) {
					return std::nullopt;
				}
				found = chunks.emplace(p_id.bytes, std::move(*c)).first;
			}
			return found->second;
		};
		uint64_t streamed_tiles = 0, layouts = 0;
		for (const core::res::TerrainStreamingNode &node : tree.streaming_nodes) {
			if (node.lod0_chunk_size == 0) {
				continue;
			}
			core::res::TerrainChunkLayout layout;
			const core::res::ResError e = tree.chunk_layout(node.id, layout);
			CHECK_MESSAGE(e.ok(), e.message());
			streamed_tiles += layout.mask_tiles;
			++layouts;
		}
		CHECK(layouts == 404);
		CHECK(streamed_tiles == 759);

		struct Point {
			std::array<float, 2> at;
			std::array<uint8_t, 10> weights;
		};
		const Point points[] = {
			{ { -196.14f, -568.25f }, { 255, 255, 0, 0, 0, 0, 0, 0, 0, 0 } },
			{ { -167.76f, -475.19f }, { 255, 255, 0, 0, 0, 0, 0, 0, 0, 0 } },
			{ { -12.3f, 51.7f }, { 255, 255, 151, 0, 220, 82, 0, 0, 3, 0 } },
			{ { -150.0f, -800.0f }, { 255, 0, 0, 0, 0, 0, 0, 0, 0, 143 } },
			{ { 500.0f, 200.0f }, { 255, 255, 0, 0, 0, 255, 0, 0, 0, 0 } },
			{ { -700.0f, 300.0f }, { 255, 0, 0, 0, 0, 0, 0, 0, 0, 0 } },
		};
		for (const Point &p : points) {
			core::res::TerrainRegion region;
			region.texel = 0.01f;
			region.origin = { p.at[0] - 0.005f, p.at[1] - 0.005f };
			region.width = region.height = 1;
			std::vector<std::vector<uint8_t>> layers;
			const core::res::ResError e = tree.mask_grids(region, mask.max_level(), fetch, layers);
			REQUIRE_MESSAGE(e.ok(), e.message());
			REQUIRE(layers.size() == 10);
			std::string got;
			for (size_t l = 0; l < 10; ++l) {
				got += std::to_string(layers[l][0]) + " ";
				CHECK_MESSAGE(
						int(layers[l][0]) == int(p.weights[l]), "layer " << l << " at " << p.at[0] << ", " << p.at[1]);
			}
			std::printf("render.terrain mask at (%.2f, %.2f): %s\n", double(p.at[0]), double(p.at[1]), got.c_str());
		}

		std::unique_ptr<LookResolver> resolver = LookResolver::open(*mount);
		std::vector<std::pair<std::string, std::string>> failures;
		std::vector<std::string> textures;
		const std::vector<SceneTerrainCombo> combos = plan_terrain_combos(
				*mount, resolver->shaders(), asset,
				[&](const std::string &p_name) {
					textures.push_back(p_name);
					return int32_t(textures.size() - 1);
				},
				failures);
		for (const auto &[item, error] : failures) {
			std::printf("render.terrain failure %s: %s\n", item.c_str(), error.c_str());
		}
		CHECK(failures.empty());
		CHECK(combos.size() >= 40);
		std::printf("render.terrain combinations %zu, texture bindings %zu\n", combos.size(), textures.size());
		Result<std::vector<std::vector<uint8_t>>> programs = load_terrain_programs(*mount, combos);
		REQUIRE_MESSAGE(programs.ok(), (programs.ok() ? std::string() : programs.error().message));

		core::res::HeightGrid heights;
		const core::res::ResError he = tree.height_grid(5, fetch, heights);
		REQUIRE_MESSAGE(he.ok(), he.message());
		TerrainLookInput input;
		input.tree = &tree;
		input.heights = &heights;
		input.region = { -1024.0f, -1024.0f, 1024.0f, 1024.0f };
		input.combos = combos;
		input.programs = *programs;
		input.fetch = fetch;
		Result<TerrainLook> look = build_terrain_look(input);
		REQUIRE_MESSAGE(look.ok(), (look.ok() ? std::string() : look.error().message));
		const TerrainLookStats &s = look->stats;
		std::printf("render.terrain look: %u patches (%u exact, %u superset, %u opaque, %u misses), %zu materials, "
					"%llu instructions, images %.1f MiB\n",
				s.patches, s.exact, s.superset, s.opaque, s.misses, look->materials.size(),
				static_cast<unsigned long long>(s.instructions), double(s.image_bytes) / (1024.0 * 1024.0));
		CHECK(s.misses == 0);
		CHECK(s.patches == 1024);
		CHECK(look->patch == doctest::Approx(16.0f));
		for (const TerrainMaterial &mat : look->materials) {
			CHECK(mat.code.find("ALBEDO") != std::string::npos);
			CHECK(!mat.textures.empty());
		}
		const auto combo_at = [&](float p_x, float p_z) {
			return combos[look->materials[look->material_at(p_x, p_z)].combo].layers;
		};
		CHECK(combo_at(-12.3f, 51.7f) == std::vector<uint8_t>{ 0, 3, 4, 5, 7, 8 });
		CHECK(combo_at(-700.0f, 300.0f) == std::vector<uint8_t>{ 0 });
		CHECK(combo_at(500.0f, 200.0f) == std::vector<uint8_t>{ 0, 5, 9 });
		const SnapshotDiff diff = diff_snapshots(m.before, m.reader.snapshot());
		std::printf("install.unchanged %s\n", diff.clean() ? "PASS" : diff.summary().c_str());
		CHECK_MESSAGE(diff.clean(), diff.summary());
	}

	TEST_CASE("terrain: XP3_Desert tiles stored in a parent's children chunk agree with the parent's tiles") {
		test::Mounted m;
		if (!test::open_or_skip(m)) {
			return;
		}
		Result<LevelMount> mount = m.install->mount_superbundle("Win32/Levels/XP3_Desert/XP3_Desert");
		REQUIRE(mount.ok());
		REQUIRE(mount->load_all_bundles().ok());
		auto terrain = mount->read_res("levels/xp3_desert/terrain/terrain_8k");
		REQUIRE(terrain.ok());
		std::string tree_name;
		REQUIRE(core::res::terrain_streaming_tree_name(terrain->second, tree_name).ok());
		auto tree_bytes = mount->read_res(tree_name);
		REQUIRE(tree_bytes.ok());
		core::res::TerrainStreamingTree tree;
		const core::res::ResError parsed = core::res::TerrainStreamingTree::parse(tree_bytes->second, tree);
		REQUIRE_MESSAGE(parsed.ok(), parsed.message());
		REQUIRE(tree.mask.has_value());
		REQUIRE(tree.colour.has_value());
		std::map<std::array<uint8_t, 16>, std::vector<uint8_t>> chunks;
		const core::res::TerrainChunkFetch fetch =
				[&](const core::res::ChunkId &p_id) -> std::optional<std::vector<uint8_t>> {
			auto found = chunks.find(p_id.bytes);
			if (found == chunks.end()) {
				Result<std::vector<uint8_t>> c = mount->read_chunk(Guid::from_bytes(p_id.bytes.data()));
				if (!c) {
					return std::nullopt;
				}
				found = chunks.emplace(p_id.bytes, std::move(*c)).first;
			}
			return found->second;
		};
		const auto own_chunk = [&](const core::res::TerrainNodeId &p_id) {
			for (const core::res::TerrainStreamingNode &s : tree.streaming_nodes) {
				if (s.id == p_id) {
					return s.lod0_chunk_size > 0;
				}
			}
			return false;
		};
		const auto region_of = [](const std::array<float, 4> &p_bounds, const core::res::TerrainNodeId &p_id) {
			const float side = (p_bounds[2] - p_bounds[0]) / float(uint32_t(1) << p_id.level);
			core::res::TerrainRegion r;
			r.texel = side / 32.0f;
			r.origin = { p_bounds[0] + side * float(p_id.x), p_bounds[1] + side * float(p_id.y) };
			r.width = r.height = 32;
			return r;
		};
		const auto difference = [](const std::vector<uint8_t> &p_a, const std::vector<uint8_t> &p_b) {
			double sum = 0.0;
			for (size_t i = 0; i < p_a.size(); ++i) {
				sum += std::abs(double(p_a[i]) - double(p_b[i]));
			}
			return p_a.empty() ? 0.0 : sum / double(p_a.size());
		};
		uint32_t colour_nodes = 0, mask_nodes = 0;
		double colour_worst = 0.0, mask_worst = 0.0, colour_sum = 0.0, mask_sum = 0.0;
		uint32_t mask_tiles = 0;
		for (const core::res::TerrainColourNode &node : tree.colour->nodes) {
			if (node.id.level == 0 || !node.can_stream || node.persistent || own_chunk(node.id)) {
				continue;
			}
			const core::res::TerrainRegion region = region_of(tree.colour->bounds, node.id);
			std::vector<uint8_t> parent, child;
			REQUIRE(tree.colour_grid(region, uint8_t(node.id.level - 1), fetch, parent).ok());
			const core::res::ResError e = tree.colour_grid(region, node.id.level, fetch, child);
			REQUIRE_MESSAGE(e.ok(), e.message());
			const double d = difference(parent, child);
			colour_worst = std::max(colour_worst, d);
			colour_sum += d;
			++colour_nodes;
		}
		for (const core::res::TerrainMaskNode &node : tree.mask->nodes) {
			if (node.id.level == 0 || node.tiles == 0 || node.persistent || own_chunk(node.id)) {
				continue;
			}
			const core::res::TerrainRegion region = region_of(tree.mask->bounds, node.id);
			std::vector<std::vector<uint8_t>> parent, child;
			REQUIRE(tree.mask_grids(region, uint8_t(node.id.level - 1), fetch, parent).ok());
			const core::res::ResError e = tree.mask_grids(region, node.id.level, fetch, child);
			REQUIRE_MESSAGE(e.ok(), e.message());
			for (const core::res::TerrainMaskRecord &r : node.records) {
				if (!r.full) {
					const double d = difference(parent[r.layer], child[r.layer]);
					mask_worst = std::max(mask_worst, d);
					mask_sum += d;
					++mask_tiles;
				}
			}
			++mask_nodes;
		}
		const double colour_mean = colour_sum / double(std::max(colour_nodes, 1u));
		const double mask_mean = mask_sum / double(std::max(mask_tiles, 1u));
		std::printf("render.terrain XP3_Desert children-chunk tiles against their parents: %u colour (mean difference "
					"%.2f / 255, worst %.2f), %u mask nodes, %u tiles (mean %.2f, worst %.2f)\n",
				colour_nodes, colour_mean, colour_worst, mask_nodes, mask_tiles, mask_mean, mask_worst);
		CHECK(colour_nodes == 196);
		CHECK(mask_nodes == 257);
		CHECK(colour_mean < 12.0);
		CHECK(mask_mean < 12.0);
		CHECK(colour_worst < 24.0);
		CHECK(mask_worst < 48.0);
	}
}
