#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/install_snapshot.h"
#include "bf3/core/res/terrain.h"
#include "bf3/core/superbundle_mount.h"
#include "bf3/render/dxbc.h"
#include "bf3/render/look_materials.h"
#include "bf3/render/terrain_look.h"
#include "test_support.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace bf3;
using namespace bf3::core;
using namespace bf3::render;

namespace {

std::vector<uint8_t> container(const std::vector<uint32_t> &p_shader) {
	std::vector<uint8_t> out(32 + 4 + 8);
	std::memcpy(out.data(), "DXBC", 4);
	const uint32_t chunks = 1, at = 36, fourcc = 0x52444853u;
	std::memcpy(out.data() + 28, &chunks, 4);
	std::memcpy(out.data() + 32, &at, 4);
	std::memcpy(out.data() + 36, &fourcc, 4);
	const uint32_t size = uint32_t(p_shader.size() * 4);
	std::memcpy(out.data() + 40, &size, 4);
	out.resize(out.size() + size);
	std::memcpy(out.data() + 44, p_shader.data(), size);
	const uint32_t total = uint32_t(out.size());
	std::memcpy(out.data() + 24, &total, 4);
	return out;
}

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
		Result<DxbcProgram> program = parse_dxbc(container(tokens));
		REQUIRE(program.ok());
		Result<PixelTranslation> t = translate_pixel_program(*program);
		REQUIRE_MESSAGE(t.ok(), (t.ok() ? std::string() : t.error().message));
		CHECK(t->outputs == 1);
		CHECK(t->instructions == 2);
		CHECK(t->body.find("o0.xyzw = vec4(1.0, 0.0, (-0.5), 1.0);") != std::string::npos);
		const auto with_and = [&](uint32_t p_mask) {
			std::vector<uint32_t> and_tokens(shader.begin(), shader.begin() + 5);
			and_tokens.insert(and_tokens.end(),
					{ (1u | (13u << 24)), 0x001020F2u, 0u, 0x00004002u, 0x3F800000u, 0x3F800000u, 0x3F800000u,
							0x3F800000u, 0x00004002u, p_mask, p_mask, p_mask, p_mask, (62u | (1u << 24)) });
			and_tokens[1] = uint32_t(and_tokens.size());
			Result<DxbcProgram> p = parse_dxbc(container(and_tokens));
			REQUIRE(p.ok());
			return translate_pixel_program(*p);
		};
		CHECK(with_and(0x3F800000u).ok());
		CHECK_FALSE(with_and(0x000000FFu).ok());
		tokens[5] = 87u | (8u << 24);
		program = parse_dxbc(container(tokens));
		REQUIRE(program.ok());
		CHECK_FALSE(translate_pixel_program(*program).ok());
		CHECK_FALSE(parse_dxbc(std::vector<uint8_t>(16, 0)).ok());
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
