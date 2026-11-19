#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/install_snapshot.h"
#include "bf3/core/res/terrain.h"
#include "bf3/core/res/terrain_mask_field.h"
#include "bf3/core/res/terrain_visual.h"
#include "bf3/core/superbundle_mount.h"
#include "bf3/core/thread_pool.h"
#include "bf3/core/util/text.h"
#include "bf3/render/look_materials.h"
#include "bf3/render/terrain_scatter.h"
#include "dxbc_test_support.h"
#include "terrain_test_support.h"
#include "test_support.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <vector>

using namespace bf3;
using namespace bf3::core;
using namespace bf3::render;
using namespace dxbct;

namespace {

void put_float(std::vector<uint8_t> &r_out, size_t p_at, float p_value) {
	std::memcpy(r_out.data() + p_at, &p_value, 4);
}

void append_text(std::vector<uint8_t> &r_out, const char *p_text) {
	r_out.insert(r_out.end(), p_text, p_text + std::strlen(p_text) + 1);
}

std::vector<uint8_t> visual_with_one_type() {
	std::vector<uint8_t> out(core::res::kTerrainVisualHeaderBytes, 0);
	put_at(out, 4 * core::res::kTerrainVisualCellInstancesWord, 4096);
	append_text(out, "Levels/T/Shader");
	put(out, 2);
	out.push_back(1);
	put(out, 0);
	out.push_back(0);
	put(out, 1);
	append_text(out, "levels/t/meshes/grass_Mesh");
	std::vector<uint8_t> record(core::res::kTerrainScatterRecordBytes, 0);
	put_float(record, 4, 0.5f);
	put_float(record, 21, 0.2f);
	put_float(record, 25, 0.3f);
	put_float(record, 29, 1.0f);
	put_float(record, 33, 1.5f);
	record[53] = 7;
	out.insert(out.end(), record.begin(), record.end());
	append_text(out, "Levels/T/Terrain.StreamingTree");
	return out;
}

core::res::TerrainStreamingTree quadrant_tree() {
	core::res::TerrainMaskTree mask;
	mask.samples_per_side = 66;
	mask.bounds = { 0.0f, 0.0f, 64.0f, 64.0f };
	core::res::TerrainMaskNode root;
	core::res::TerrainMaskRecord full;
	full.layer = 1;
	full.rect = { 0.0f, 0.0f, 32.0f, 32.0f };
	full.full = true;
	core::res::TerrainMaskRecord ramp;
	ramp.layer = 1;
	ramp.rect = { 32.0f, 0.0f, 64.0f, 32.0f };
	root.records = { full, ramp };
	root.tiles = 1;
	std::vector<uint8_t> tile(66 * 66);
	for (size_t z = 0; z < 66; ++z) {
		for (size_t x = 0; x < 66; ++x) {
			tile[z * 66 + x] = uint8_t(std::min<size_t>(255, x * 4));
		}
	}
	root.persistent = tile;
	mask.nodes.push_back(root);
	core::res::TerrainStreamingTree tree;
	tree.mask = mask;
	return tree;
}

core::res::HeightGrid slope_grid() {
	core::res::HeightGrid grid;
	grid.origin_xz = { 0.0f, 0.0f };
	grid.sample_spacing = { 4.0f, 4.0f };
	grid.width = 17;
	grid.height = 17;
	grid.heights.resize(17 * 17);
	grid.holes.assign(17 * 17, 0);
	for (size_t z = 0; z < 17; ++z) {
		for (size_t x = 0; x < 17; ++x) {
			grid.heights[z * 17 + x] = float(x) * 2.0f + float(z);
		}
	}
	grid.holes[5 * 17 + 5] = 1;
	return grid;
}

std::vector<uint8_t> mask_program(uint32_t p_extra_opcode = 0, bool p_graph_texture = false) {
	std::vector<std::vector<uint32_t>> code = {
		ins(50,
				{ dst(kTemp, 0, 0x3), src(kInput, 1, swz(0, 1, 0, 0)), cb(1, 0, swz(0, 1, 0, 0)),
						cb(1, 0, swz(2, 3, 2, 2)) }),
		ins(69, { dst(kTemp, 0, 0x1), src(kTemp, 0, swz(0, 1, 0, 0)), src(kResource, 3), sampler(0) }),
		ins(49, { dst(kTemp, 0, 0x2), imm(0.5f, 0.5f, 0.5f, 0.5f), src(kTemp, 0, swz(0, 0, 0, 0)) }),
		ins(31, { src(kTemp, 0, swz(1, 1, 1, 1)) }),
		ins(54, { dst(kOutput, 0, 0x1), src(kTemp, 0, swz(0, 0, 0, 0)) }),
		ins(54, { dst(kOutput, 2, 0x1), imm(0.25f, 0.25f, 0.25f, 0.25f) }),
		ins(18, {}),
		ins(54, { dst(kOutput, 0, 0x1), imm(0.0f, 0.0f, 0.0f, 0.0f) }),
		ins(21, {}),
	};
	code[3][0] |= 1u << 18;
	if (p_extra_opcode != 0) {
		code.push_back(ins(p_extra_opcode, { dst(kTemp, 0, 0xF), src(kTemp, 0) }));
	}
	std::vector<BindEntry> bindings = { { "texture_maskMap", 2, 3 }, { "externalConstants", 0, 1 } };
	if (p_graph_texture) {
		bindings.push_back({ "texture_Texture18", 2, 5 });
	}
	return container(
			{ { "RDEF", rdef(bindings, "externalConstants", { { "external_worldToMask0UvTransform", 0, 16 } }) },
					{ "SHDR", code_chunk(0, 1, code) } });
}

struct LevelTypes {
	const char *level;
	const char *asset;
	size_t types;
};

constexpr LevelTypes kLevels[] = {
	{ "MP_001", "levels/mp_001/terrain/mp001_terrain/mp001_terrain", 5 },
	{ "MP_003", "levels/mp_003/newterrain/mp_003_newterrain", 12 },
	{ "MP_007", "levels/mp_007/terrain/mp_007", 23 },
	{ "MP_012", "levels/mp_012/terrain/mp_012", 11 },
	{ "MP_013", "levels/mp_013/terrain/mp013_testterrain", 15 },
	{ "MP_017", "levels/mp_017/terrain/mp_017_terrain", 13 },
	{ "MP_018", "levels/mp_018/terrain/terrain_mp018", 26 },
	{ "MP_Subway", "levels/mp_subway/terrain/mp_subway_terrain", 26 },
	{ "XP1_001", "levels/xp1_001/xpack1_001_terrain", 7 },
	{ "XP1_002", "levels/xp1_002/terrain_2/gulfterrain_03", 17 },
	{ "XP1_003", "levels/xp1_003/xpack1_003_terrain", 7 },
	{ "XP1_004", "levels/xp1_004/xp1_004_terrain8k", 5 },
	{ "XP3_Alborz", "levels/mp_whitepeak/terrain/terrain", 23 },
	{ "XP3_Desert", "levels/xp3_desert/terrain/terrain_8k", 23 },
	{ "XP3_Shield", "levels/xp3_legrandval/terrain/legrandval", 16 },
	{ "XP3_Valley", "levels/mp_canyon/mp_canyon_terrain/mp_canyon_terrain/mp_canyon_terrain", 17 },
	{ "XP4_FD", "levels/xp4_financialdistrict/terrain4k/xp4_fd_terrain/xp4_fd_terrain", 5 },
	{ "XP4_Parl", "levels/xp4_parliament/terrain_parliament/terrain_parliament", 17 },
	{ "XP4_Quake", "levels/xp4_earthquake/terrain/earthquake_terrain/earthquake_terrain", 17 },
	{ "XP4_Rubble", "levels/xp4_rubble/terrain/terrain", 13 },
	{ "XP5_001", "levels/xp5_001/terrain/terrainexample", 11 },
	{ "XP5_002", "levels/xp5_002/xp5_002_terrain/xp5_002_terrain", 14 },
	{ "XP5_003", "levels/xp5_003/xp5_003_terrain/xp5_003_terrain", 16 },
	{ "XP5_004", "levels/xp5_004/xp5_004_terrain/xp5_004_terrain", 13 },
};

using LoadedTree = test::LoadedTerrainTree;

struct BuiltScatter {
	SceneScatterPlan plan;
	std::vector<std::string> meshes;
	std::vector<std::pair<std::string, std::string>> failures;
	Result<std::shared_ptr<const TerrainScatter>> scatter = core::make_error("not built");
};

void build_scatter(
		const LevelMount &p_mount, const std::string &p_asset, const LoadedTree &p_tree, BuiltScatter &r_out) {
	std::unique_ptr<LookResolver> resolver = LookResolver::open(p_mount);
	if (resolver == nullptr) {
		r_out.failures.emplace_back(p_asset, "no look resolver");
		return;
	}
	r_out.plan = plan_terrain_scatter(
			p_mount, resolver->shaders(), p_asset,
			[&](const std::string &p_mesh) {
				r_out.meshes.push_back(p_mesh);
				return int32_t(r_out.meshes.size() - 1);
			},
			r_out.failures);
	test::TerrainChunks chunks(p_mount);
	const core::res::TerrainChunkFetch fetch = chunks.fetch();
	core::res::HeightGrid grid;
	const core::res::ResError heights = p_tree.tree.height_grid(4, fetch, grid);
	if (!heights.ok()) {
		r_out.failures.emplace_back(p_asset, heights.message());
		return;
	}
	TerrainScatter::Input input;
	input.plan = &r_out.plan;
	input.tree = &p_tree.tree;
	input.heights = &grid;
	input.fetch = fetch;
	input.image = [&](const std::string &p_name) { return load_scatter_image(p_mount, p_name); };
	r_out.scatter = TerrainScatter::build(input, r_out.failures);
}
}

TEST_SUITE("render-scatter-unit") {
	TEST_CASE("scatter: a visual terrain's scattering types read, and every shorter file is refused") {
		const std::vector<uint8_t> bytes = visual_with_one_type();
		core::res::TerrainVisual visual;
		const core::res::ResError e = core::res::TerrainVisual::parse(bytes, visual);
		REQUIRE_MESSAGE(e.ok(), e.message());
		CHECK(visual.surface_shader == "Levels/T/Shader");
		CHECK(visual.cell_instances() == 4096);
		CHECK(visual.layer_flags == std::vector<uint8_t>{ 1, 0 });
		REQUIRE(visual.types.size() == 1);
		const core::res::TerrainScatterType &type = visual.types[0];
		CHECK(type.layer == 1);
		CHECK(type.mesh == "levels/t/meshes/grass_Mesh");
		CHECK(type.density == 0.5f);
		CHECK(type.scale_min == std::array<float, 2>{ 0.2f, 0.3f });
		CHECK(type.scale_max == std::array<float, 2>{ 1.0f, 1.5f });
		CHECK(type.tree_level == 7);
		CHECK(visual.streaming_tree == "Levels/T/Terrain.StreamingTree");
		CHECK(core::res::terrain_visual_name("Levels/T/Terrain") == "levels/t/terrain.visual");
		for (size_t len = 0; len < bytes.size(); ++len) {
			core::res::TerrainVisual cut;
			CHECK_FALSE(core::res::TerrainVisual::parse(std::span<const uint8_t>(bytes.data(), len), cut).ok());
		}
		std::vector<uint8_t> deep = bytes;
		deep[deep.size() - std::strlen("Levels/T/Terrain.StreamingTree") - 1 - core::res::kTerrainScatterRecordBytes +
				53] = 17;
		CHECK_FALSE(core::res::TerrainVisual::parse(deep, visual).ok());
	}

	TEST_CASE("scatter: MaskScale names give their layers and the scattered ones") {
		const std::optional<ScatterComboName> c =
				terrain_scatter_combo("Levels/A/T__1MS_5MS_9M__MaskScale2d__0", "levels/a/t");
		REQUIRE(c.has_value());
		CHECK(c->layers == std::vector<uint8_t>{ 1, 5, 9 });
		CHECK(c->scattered == std::vector<uint8_t>{ 1, 5 });
		CHECK(terrain_scatter_combo("levels/a/t__0ms__maskscale2d__0", "Levels/A/T").has_value());
		CHECK_FALSE(terrain_scatter_combo("levels/a/t__1mv_5mv__2d__0", "levels/a/t").has_value());
		CHECK_FALSE(terrain_scatter_combo("levels/a/t__1m_5m__maskscale2d__0", "levels/a/t").has_value());
		CHECK_FALSE(terrain_scatter_combo("levels/a/t__1ms_1m__maskscale2d__0", "levels/a/t").has_value());
		CHECK_FALSE(terrain_scatter_combo("levels/b/t__1ms__maskscale2d__0", "levels/a/t").has_value());
	}

	TEST_CASE("scatter: the mask field reads a record's tile at the data's texel centres and a full record as 255") {
		const core::res::TerrainStreamingTree tree = quadrant_tree();
		core::res::TerrainMaskField field;
		const core::res::ResError e = core::res::TerrainMaskField::build(tree, nullptr, field);
		REQUIRE_MESSAGE(e.ok(), e.message());
		CHECK(field.layers() == 2);
		CHECK(field.records() == 2);
		CHECK(field.coverage(1, 10.0f, 10.0f) == 255.0f);
		CHECK(field.coverage(1, 32.0f + 2.25f, 5.0f) == doctest::Approx(20.0));
		CHECK(field.coverage(1, 32.0f + 2.5f, 5.0f) == doctest::Approx(22.0));
		CHECK(field.coverage(1, 10.0f, 40.0f) == 0.0f);
		CHECK(field.coverage(0, 10.0f, 10.0f) == 0.0f);
		CHECK(field.coverage(1, -1.0f, 10.0f) == 0.0f);
		CHECK(field.coverage(1, 64.0f, 10.0f) == 0.0f);
	}

	TEST_CASE("scatter: the heights give the drawn terrain's triangle surface and nothing over a hole") {
		const ScatterHeights heights = ScatterHeights::from_grid(slope_grid(), std::nullopt);
		const std::optional<float> low = heights.surface(1.0f, 1.0f);
		REQUIRE(low.has_value());
		CHECK(*low == doctest::Approx(0.5 + 0.25).epsilon(0.001));
		const std::optional<float> high = heights.surface(3.0f, 3.5f);
		REQUIRE(high.has_value());
		CHECK(*high == doctest::Approx(1.5 + 0.875).epsilon(0.001));
		CHECK_FALSE(heights.surface(18.0f, 18.0f).has_value());
		CHECK_FALSE(heights.surface(21.0f, 21.0f).has_value());
		CHECK(heights.surface(26.0f, 26.0f).has_value());
		CHECK_FALSE(heights.surface(-0.5f, 4.0f).has_value());
		CHECK_FALSE(heights.surface(64.0f, 4.0f).has_value());
		const ScatterHeights cropped =
				ScatterHeights::from_grid(slope_grid(), std::array<float, 4>{ 8.0f, 8.0f, 16.0f, 16.0f });
		CHECK_FALSE(cropped.surface(4.0f, 10.0f).has_value());
		CHECK(cropped.surface(10.0f, 10.0f).has_value());
		CHECK(heights.bilinear(0.5f / 17.0f, 0.5f / 17.0f) == doctest::Approx(0.0).epsilon(0.001));
		CHECK(heights.bilinear(2.0f / 17.0f, 0.5f / 17.0f) == doctest::Approx(3.0).epsilon(0.001));
	}

	TEST_CASE("scatter: an instance stands on the drawn triangle's normal, turned by its yaw and scaled") {
		const ScatterHeights heights = ScatterHeights::from_grid(slope_grid(), std::nullopt);
		const std::optional<std::array<float, 3>> n = heights.normal(1.0f, 1.0f);
		REQUIRE(n.has_value());
		const std::optional<float> h0 = heights.surface(1.0f, 1.0f), hx = heights.surface(1.5f, 1.0f),
								   hz = heights.surface(1.0f, 1.5f);
		REQUIRE((h0 && hx && hz));
		CHECK((*n)[0] * 0.5f + (*n)[1] * (*hx - *h0) == doctest::Approx(0.0).epsilon(0.0001));
		CHECK((*n)[2] * 0.5f + (*n)[1] * (*hz - *h0) == doctest::Approx(0.0).epsilon(0.0001));
		ScatterInstance inst{ { 1.0f, *h0, 1.0f }, 0.7f, { 2.0f, 3.0f } };
		const std::array<float, 12> upright = scatter_instance_rows(inst);
		CHECK(upright[5] == doctest::Approx(3.0f));
		CHECK(upright[0] == doctest::Approx(2.0f * std::cos(0.7f)));
		CHECK(upright[2] == doctest::Approx(2.0f * std::sin(0.7f)));
		inst.up = *n;
		const std::array<float, 12> tilted = scatter_instance_rows(inst);
		for (size_t r = 0; r < 3; ++r) {
			CHECK(tilted[r * 4 + 1] == doctest::Approx(3.0f * (*n)[r]));
			CHECK(tilted[r * 4 + 3] == inst.position[r]);
		}
		CHECK(tilted[0] * tilted[1] + tilted[4] * tilted[5] + tilted[8] * tilted[9] ==
				doctest::Approx(0.0).epsilon(0.0001));
		CHECK(std::sqrt(tilted[0] * tilted[0] + tilted[4] * tilted[4] + tilted[8] * tilted[8]) ==
				doctest::Approx(2.0f));
	}

	TEST_CASE("scatter: a MaskScale program runs on the CPU with its branch, mask sample and constants, a texture the "
			  "data binds none to as zero") {
		const core::res::TerrainStreamingTree tree = quadrant_tree();
		core::res::TerrainMaskField field;
		REQUIRE(core::res::TerrainMaskField::build(tree, nullptr, field).ok());
		const ScatterHeights heights = ScatterHeights::from_grid(slope_grid(), std::nullopt);
		const std::vector<uint8_t> layers = { 1 };
		Result<MaskScaleProgram> program =
				MaskScaleProgram::build(mask_program(), layers, field, heights, [](uint32_t) { return -1; });
		REQUIRE_MESSAGE(program.ok(), (program.ok() ? std::string() : program.error().message));
		const ScatterSamplers samplers{ &field, &heights, {} };
		program->measure(samplers);
		const MaskScaleOutput inside = program->run(samplers, 10.0f, 10.0f, 1.0f);
		CHECK(inside.mask[0] == doctest::Approx(1.0));
		CHECK(inside.scale[0] == doctest::Approx(0.25));
		const MaskScaleOutput ramp = program->run(samplers, 32.0f + 20.25f, 5.0f, 1.0f);
		CHECK(ramp.mask[0] == doctest::Approx(164.0 / 255.0));
		CHECK(ramp.scale[0] == doctest::Approx(0.25));
		const MaskScaleOutput thin = program->run(samplers, 32.0f + 2.25f, 5.0f, 1.0f);
		CHECK(thin.mask[0] == 0.0f);
		CHECK(thin.scale[0] == 0.0f);
		const MaskScaleOutput outside = program->run(samplers, 10.0f, 40.0f, 1.0f);
		CHECK(outside.mask[0] == 0.0f);
		Result<MaskScaleProgram> refused =
				MaskScaleProgram::build(mask_program(73), layers, field, heights, [](uint32_t) { return -1; });
		REQUIRE_FALSE(refused.ok());
		Result<MaskScaleProgram> unbound = MaskScaleProgram::build(mask_program(0, true), layers, field, heights,
				[](uint32_t) -> std::optional<int32_t> { return std::nullopt; });
		REQUIRE_MESSAGE(unbound.ok(), (unbound.ok() ? std::string() : unbound.error().message));
		CHECK(unbound->run(samplers, 10.0f, 10.0f, 1.0f).mask[0] == doctest::Approx(1.0));
		CHECK_FALSE(MaskScaleProgram::build(mask_program(0, true), layers, field, heights, [](uint32_t) {
			return -1;
		}).ok());
		Result<MaskScaleProgram> no_layer = MaskScaleProgram::build(
				mask_program(), std::vector<uint8_t>{ 0 }, field, heights, [](uint32_t) { return -1; });
		CHECK_FALSE(no_layer.ok());
	}
}

TEST_SUITE("render-scatter") {
	TEST_CASE("scatter: every MP terrain's scattering types read as the data has them, 352 on 24 terrains, and all "
			  "their mask programs build") {
		test::Mounted m;
		if (!test::open_or_skip(m)) {
			return;
		}
		struct LevelRead {
			std::string error;
			uint32_t res_type = 0;
			size_t types = 0;
			size_t layer_flags = 0;
			size_t mask_layers = 0;
			bool tree_named = false;
			size_t named = 0;
			size_t unfit = 0;
			BuiltScatter built;
		};
		const std::vector<LevelRead> reads = parallel_map<LevelRead>(std::size(kLevels), [&](size_t p_i) {
			const LevelTypes &level = kLevels[p_i];
			LevelRead out;
			Result<LevelMount> mount =
					m.install->mount_superbundle(std::string("Win32/Levels/") + level.level + "/" + level.level);
			if (!mount || !mount->load_all_bundles().ok()) {
				out.error = "mount";
				return out;
			}
			LoadedTree tree;
			if (!test::load_terrain_tree(*mount, level.asset, tree) || !tree.tree.mask) {
				out.error = "streaming tree";
				return out;
			}
			auto res = mount->read_res(core::res::terrain_visual_name(level.asset));
			if (!res) {
				out.error = "no visual terrain";
				return out;
			}
			out.res_type = res->first.res_type;
			core::res::TerrainVisual visual;
			const core::res::ResError e = core::res::TerrainVisual::parse(res->second, visual);
			if (!e.ok()) {
				out.error = e.message();
				return out;
			}
			out.types = visual.types.size();
			out.layer_flags = visual.layer_flags.size();
			out.mask_layers = tree.tree.mask->layer_count();
			out.tree_named = to_lower_ascii(visual.streaming_tree) == to_lower_ascii(tree.name);
			for (const core::res::TerrainScatterType &type : visual.types) {
				out.unfit += type.density > 0.0f && type.tree_level >= 1 && type.tree_level <= 13 ? 0 : 1;
				out.named += mount->read_res(to_lower_ascii(type.mesh)).ok() ? 1 : 0;
			}
			build_scatter(*mount, level.asset, tree, out.built);
			return out;
		});
		size_t total = 0;
		for (size_t i = 0; i < reads.size(); ++i) {
			const LevelTypes &level = kLevels[i];
			const LevelRead &read = reads[i];
			CAPTURE(level.level);
			REQUIRE_MESSAGE(read.error.empty(), read.error);
			CHECK(read.res_type == core::res::kResTypeVisualTerrain);
			CHECK(read.types == level.types);
			CHECK(read.layer_flags == read.mask_layers);
			CHECK(read.tree_named);
			CHECK(read.unfit == 0);
			CHECK(read.named == read.types);
			total += read.types;
			for (const auto &[item, error] : read.built.failures) {
				MESSAGE(item << ": " << error);
			}
			CHECK(read.built.failures.empty());
			REQUIRE(read.built.scatter.ok());
			CHECK((*read.built.scatter)->types().size() == level.types);
			CHECK((*read.built.scatter)->programs() == read.built.plan.combos.size());
			CHECK_FALSE((*read.built.scatter)->levels().empty());
		}
		CHECK(total == 352);
		const SnapshotDiff diff = diff_snapshots(m.before, m.reader.snapshot());
		std::printf("install.unchanged %s\n", diff.clean() ? "PASS" : diff.summary().c_str());
		CHECK_MESSAGE(diff.clean(), diff.summary());
	}

	TEST_CASE("scatter: MP_007 places every type its masks cover, as many as the masks sum to, the same on every run") {
		test::Mounted m;
		if (!test::open_or_skip(m)) {
			return;
		}
		Result<LevelMount> mount = m.install->mount_superbundle("Win32/Levels/MP_007/MP_007");
		REQUIRE(mount.ok());
		REQUIRE(mount->load_all_bundles().ok());
		const std::string asset = "levels/mp_007/terrain/mp_007";
		LoadedTree tree;
		REQUIRE(test::load_terrain_tree(*mount, asset, tree));
		BuiltScatter built;
		build_scatter(*mount, asset, tree, built);
		const SceneScatterPlan &plan = built.plan;
		CHECK(plan.types.size() == 23);
		CHECK(built.meshes.size() == 17);
		CHECK(plan.combos.size() == 115);
		CHECK(plan.cell_instances == 262144);
		REQUIRE_MESSAGE(built.scatter.ok(), (built.scatter.ok() ? std::string() : built.scatter.error().message));
		for (const auto &[item, error] : built.failures) {
			MESSAGE(item << ": " << error);
		}
		CHECK(built.failures.empty());
		const TerrainScatter &scatter = **built.scatter;
		CHECK(scatter.programs() == 115);
		CHECK(scatter.levels() == std::vector<uint8_t>{ 1, 2, 7, 8, 9, 10 });
		CHECK(scatter.cell_size(7) == 64.0f);
		CHECK(scatter.masks().coverage(1, -196.14f, -568.25f) == 255.0f);
		CHECK(scatter.masks().coverage(4, -12.3f, 51.7f) == doctest::Approx(220.0).epsilon(0.02));
		CHECK(scatter.masks().coverage(5, 500.0f, 200.0f) == 255.0f);

		const std::array<float, 2> places[] = { { -236.9f, -87.7f }, { -12.3f, 51.7f }, { -383.1f, 245.3f },
			{ 500.0f, 200.0f } };
		std::vector<uint64_t> instances(scatter.types().size(), 0);
		std::vector<double> coverage(scatter.types().size(), 0.0);
		uint64_t cells = 0, total = 0, unmatched = 0;
		std::map<std::tuple<uint8_t, int32_t, int32_t>, bool> seen;
		for (const std::array<float, 2> &at : places) {
			for (uint8_t level : scatter.levels()) {
				for (const ScatterCellId &cell :
						scatter.cells_within(level, at[0], at[1], scatter.view_distance(level))) {
					if (!seen.emplace(std::make_tuple(cell.level, cell.x, cell.z), true).second) {
						continue;
					}
					ScatterCellStats stats;
					const std::vector<ScatterBatch> batches = scatter.generate(cell, &stats);
					const std::array<float, 4> rect = scatter.cell_rect(cell);
					++cells;
					unmatched += stats.unmatched;
					for (const ScatterBatch &batch : batches) {
						instances[batch.type] += batch.instances.size();
						coverage[batch.type] += batch.coverage;
						total += batch.instances.size();
						const ScatterType &type = scatter.types()[batch.type];
						bool placed = true;
						for (const ScatterInstance &inst : batch.instances) {
							const std::optional<float> y =
									scatter.heights().surface(inst.position[0], inst.position[2]);
							placed = placed && inst.position[0] >= rect[0] && inst.position[0] <= rect[2] &&
									inst.position[2] >= rect[1] && inst.position[2] <= rect[3] && y.has_value() &&
									*y == inst.position[1] && inst.scale[0] > 0.0f &&
									inst.scale[0] <= type.scale_max[0] * 1.0001f && inst.scale[1] > 0.0f &&
									inst.scale[1] <= type.scale_max[1] * 1.0001f;
						}
						CHECK(placed);
					}
					if (cells <= 6) {
						const std::vector<ScatterBatch> again = scatter.generate(cell);
						REQUIRE(again.size() == batches.size());
						for (size_t b = 0; b < batches.size(); ++b) {
							REQUIRE(again[b].instances.size() == batches[b].instances.size());
							bool same = true;
							for (size_t i = 0; i < batches[b].instances.size(); ++i) {
								same = same && again[b].instances[i].position == batches[b].instances[i].position &&
										again[b].instances[i].yaw == batches[b].instances[i].yaw &&
										again[b].instances[i].scale == batches[b].instances[i].scale;
							}
							CHECK(same);
						}
					}
				}
			}
		}
		CHECK(unmatched == 0);
		size_t covered = 0, drawn = 0;
		for (size_t t = 0; t < instances.size(); ++t) {
			const ScatterType &type = scatter.types()[t];
			std::printf("  scatter type %2zu layer %u level %2u %-34s density %-7g candidates %6u: mask sum %9.1f, "
						"%7llu instances\n",
					t, unsigned(type.layer), unsigned(type.tree_level),
					plan.types[type.source].data.mesh.substr(plan.types[type.source].data.mesh.rfind('/') + 1).c_str(),
					double(type.density), type.candidates, coverage[t], static_cast<unsigned long long>(instances[t]));
			CAPTURE(t);
			if (coverage[t] >= 8.0) {
				++covered;
				CHECK(instances[t] > 0);
			}
			CHECK(std::fabs(double(instances[t]) - coverage[t]) <= 5.0 * std::sqrt(coverage[t]) + 3.0);
			drawn += instances[t] > 0 ? 1 : 0;
		}
		std::printf("  scatter MP_007: %llu cells, %llu instances, %zu of %zu types drawn, %zu covered, %.1f MiB\n",
				static_cast<unsigned long long>(cells), static_cast<unsigned long long>(total), drawn, instances.size(),
				covered, double(scatter.bytes()) / (1024.0 * 1024.0));
		CHECK(covered >= 18);
		CHECK(drawn >= covered);

		std::mt19937 rng(7);
		size_t bare = 0, silent = 0;
		for (size_t i = 0; i < 20000; ++i) {
			const float x = -1024.0f + 2048.0f * float(rng() >> 8) / 16777216.0f;
			const float z = -1024.0f + 2048.0f * float(rng() >> 8) / 16777216.0f;
			for (uint32_t t = 0; t < scatter.types().size(); ++t) {
				const ScatterType &type = scatter.types()[t];
				if (type.layer == 0 || scatter.masks().coverage(type.layer, x, z) > 0.0f) {
					continue;
				}
				++bare;
				const std::optional<std::array<float, 2>> result = scatter.mask_scale(t, x, z);
				silent += !result.has_value() || (*result)[0] == 0.0f ? 1 : 0;
			}
		}
		CHECK(bare > 100000);
		CHECK(silent == bare);
		const SnapshotDiff diff = diff_snapshots(m.before, m.reader.snapshot());
		std::printf("install.unchanged %s\n", diff.clean() ? "PASS" : diff.summary().c_str());
		CHECK_MESSAGE(diff.clean(), diff.summary());
	}
}
