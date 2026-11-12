#include <doctest/doctest.h>

#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/install_snapshot.h"
#include "bf3/core/level/census.h"
#include "bf3/core/level/level.h"
#include "bf3/core/level/world.h"
#include "bf3/core/thread_pool.h"
#include "bf3/core/util/text.h"
#include "bf3/render/dxbc.h"
#include "bf3/render/level_scene.h"
#include "bf3/render/shader_database.h"
#include "bf3/render/shader_translate.h"
#include "dxbc_test_support.h"
#include "test_support.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

using namespace bf3::render;
using namespace dxbct;

namespace {

std::vector<uint8_t> layer_pixel_program() {
	return container({ { "RDEF",
							   rdef({ { "texture_Texture2", 2, 1 }, { "texture_Texture3", 2, 2 },
											{ "texture_Texture6", 2, 3 }, { "externalConstants", 0, 1 } },
									   "externalConstants", { { "external_Brightness", 0 } }) },
			{ "ISGN",
					signature({ { "SV_Position", 0, 1, 0, 0xF }, { "TEXCOORD", 0, 0, 1, 0x7 },
							{ "TEXCOORD", 1, 0, 2, 0x3 } }) },
			{ "OSGN", signature({ { "SV_Target", 0, 0, 0, 0xF }, { "SV_Target", 1, 0, 1, 0xF } }) },
			{ "SHDR",
					code_chunk(0, 3,
							{ ins(69,
									  { dst(kTemp, 0, 0x7), src(kInput, 2, swz(0, 1, 0, 0)), src(kResource, 1),
											  sampler(0) }),
									ins(56, { dst(kTemp, 1, 0x3), src(kInput, 2, swz(0, 1, 0, 0)), imm(8, 8, 0, 0) }),
									ins(69,
											{ dst(kTemp, 2, 0x7), src(kTemp, 1, swz(0, 1, 0, 0)), src(kResource, 2),
													sampler(0) }),
									ins(69,
											{ dst(kTemp, 1, 0x7), src(kTemp, 1, swz(0, 1, 0, 0)), src(kResource, 3),
													sampler(0) }),
									ins(0,
											{ dst(kTemp, 1, 0x7), src(kTemp, 1),
													[] {
														std::vector<uint32_t> n = src(kTemp, 2);
														n[0] |= 1u << 31;
														n.insert(n.begin() + 1, 1u | 1u << 6);
														return n;
													}() }),
									ins(50,
											{ dst(kTemp, 1, 0x7), src(kTemp, 0, swz(1, 1, 1, 1)), src(kTemp, 1),
													src(kTemp, 2) }),
									ins(56, { dst(kTemp, 1, 0x7), src(kTemp, 1), src(kTemp, 0, swz(0, 0, 0, 0)) }),
									ins(56, { dst(kTemp, 1, 0x7), src(kTemp, 1), cb(1, 0, swz(0, 0, 0, 0)) }),
									ins(75, { dst(kOutput, 1, 0x7), src(kTemp, 1) }),
									ins(54, { dst(kOutput, 0, 0xF), src(kInput, 1) }) }) } });
}

std::vector<uint8_t> layer_vertex_program() {
	return container({ { "RDEF", rdef({}, nullptr, {}) },
			{ "ISGN", signature({ { "TEXCOORD", 0, 0, 0, 0xF }, { "TEXCOORD", 1, 0, 1, 0x3 } }) },
			{ "OSGN",
					signature({ { "SV_Position", 0, 1, 0, 0xF }, { "TEXCOORD", 0, 0, 1, 0xF },
							{ "TEXCOORD", 1, 0, 2, 0xF } }) },
			{ "SHDR",
					code_chunk(1, 1,
							{ ins(54, { dst(kOutput, 0, 0xF), src(kInput, 0) }),
									ins(54, { dst(kOutput, 1, 0xF), src(kInput, 0) }),
									ins(54, { dst(kOutput, 2, 0x3), src(kInput, 1, swz(0, 1, 0, 0)) }) }) } });
}

}

TEST_SUITE("render-programs-unit") {
	TEST_CASE("render.programs.unit: a DXBC program decodes with its signatures, bindings and constants") {
		const std::vector<uint8_t> blob = layer_pixel_program();
		bf3::core::Result<dxbc::Program> p = dxbc::parse(blob);
		REQUIRE(p.ok());
		CHECK(p->kind == dxbc::kPixelProgram);
		CHECK(p->temps == 3);
		REQUIRE(p->code.size() == 11);
		CHECK(dxbc::format_instruction(p->code[0]) == "sample r0.xyz, v2.xyxx, t1.xyzw, s0");
		CHECK(dxbc::format_instruction(p->code[4]) == "add r1.xyz, r1.xyzw, -r2.xyzw");
		CHECK(dxbc::format_instruction(p->code[7]) == "mul r1.xyz, r1.xyzw, cb1[0].xxxx");
		REQUIRE(p->inputs.size() == 3);
		CHECK(p->inputs[1].name == "TEXCOORD");
		CHECK(p->inputs[0].system_value == 1);
		REQUIRE(p->constant_buffer(1) != nullptr);
		CHECK(p->constant_buffer(1)->variables.front().name == "external_Brightness");
		const dxbc::Binding *t2 = p->binding(dxbc::BindingType::Texture, 2);
		REQUIRE(t2 != nullptr);
		CHECK(t2->name == "texture_Texture3");
	}

	TEST_CASE("render.programs.unit: truncated and corrupt programs fail cleanly") {
		const std::vector<uint8_t> blob = layer_pixel_program();
		size_t failed = 0;
		for (size_t n = 0; n < blob.size(); n += 7) {
			failed += dxbc::parse(std::vector<uint8_t>(blob.begin(), blob.begin() + long(n))).ok() ? 0 : 1;
		}
		CHECK(failed > 0);
		std::vector<uint8_t> bad = blob;
		bad[0] = 'X';
		CHECK(!dxbc::parse(bad).ok());
		for (size_t at = 32; at < blob.size(); at += 3) {
			std::vector<uint8_t> m = blob;
			m[at] ^= 0xA5;
			(void)dxbc::parse(m);
		}
	}

	TEST_CASE("render.programs.unit: the albedo slice of a layer blend becomes a Godot program") {
		const std::vector<uint8_t> ps_blob = layer_pixel_program(), vs_blob = layer_vertex_program();
		bf3::core::Result<dxbc::Program> ps = dxbc::parse(ps_blob);
		bf3::core::Result<dxbc::Program> vs = dxbc::parse(vs_blob);
		REQUIRE(ps.ok());
		REQUIRE(vs.ok());
		const std::vector<uint32_t> formats = { 10, kDxgiR16G16Float };
		const std::vector<uint32_t> bound = { 1, 2, 3 };
		ProgramRequest request;
		request.pixel = &*ps;
		request.vertex = &*vs;
		request.vertex_formats = formats;
		request.bound_textures = bound;
		request.srgb = [](uint32_t p_reg) { return p_reg != 1; };
		bf3::core::Result<TranslatedProgram> t = translate_program(request);
		REQUIRE_MESSAGE(t.ok(), (t.ok() ? std::string() : t.error().message));
		CHECK(t->instructions == 9);
		CHECK(t->textures == std::vector<uint32_t>{ 1, 2, 3 });
		REQUIRE(t->constants.size() == 1);
		CHECK(t->constants[0].uniform == "x1_0");
		CHECK(t->constants[0].lanes[0].value == "Brightness");
		const std::string &code = t->code;
		CHECK(code.find("uniform sampler2D t1 : filter") != std::string::npos);
		CHECK(code.find("uniform sampler2D t2 : source_color,") != std::string::npos);
		CHECK(code.find("vec4 v2 = vec4((UV.x * 1.0 + 0.0), (UV.y * 1.0 + 0.0), 0.0, 0.0);") != std::string::npos);
		CHECK(code.find("r2.xyz = (texture(t2, (r1.xyxx).xy).xyzw).xyz;") != std::string::npos);
		CHECK(code.find("r1.xyz = ((r1.xyzw + (-r2.xyzw))).xyz;") != std::string::npos);
		CHECK(code.find("vec4 cb1_0 = vec4(x1_0.x, 0.0, 0.0, 0.0);") != std::string::npos);
		CHECK(code.find("o1.xyz = (sqrt(r1.xyzw)).xyz;") != std::string::npos);
		CHECK(code.find("ALBEDO = o1.xyz * o1.xyz;") != std::string::npos);
		CHECK(code.find("o0.") == std::string::npos);
		CHECK(code.find("v1") == std::string::npos);
		CHECK(code.find("//") == std::string::npos);
	}

	TEST_CASE("render.programs.unit: a program reading what it cannot map is refused with the reason") {
		const std::vector<uint8_t> ps_blob = layer_pixel_program(), vs_blob = layer_vertex_program();
		bf3::core::Result<dxbc::Program> ps = dxbc::parse(ps_blob);
		bf3::core::Result<dxbc::Program> vs = dxbc::parse(vs_blob);
		REQUIRE(ps.ok());
		REQUIRE(vs.ok());
		const std::vector<uint32_t> formats = { 10, kDxgiR16G16Float };
		const std::vector<uint32_t> two = { 1, 2 };
		ProgramRequest request;
		request.pixel = &*ps;
		request.vertex = &*vs;
		request.vertex_formats = formats;
		request.bound_textures = two;
		ProgramAnalysis analysis;
		request.analysis = &analysis;
		bf3::core::Result<TranslatedProgram> unbound = translate_program(request);
		REQUIRE(!unbound.ok());
		CHECK(unbound.error().message.find("t3") != std::string::npos);
		CHECK(analysis.sliced);
		CHECK(analysis.mask_only == std::vector<uint32_t>{ 1 });
		request.analysis = nullptr;
		const std::vector<uint32_t> bound = { 1, 2, 3 };
		const std::vector<uint32_t> packed = { 10, 10 };
		request.bound_textures = bound;
		request.vertex_formats = packed;
		bf3::core::Result<TranslatedProgram> computed = translate_program(request);
		REQUIRE(!computed.ok());
		CHECK(computed.error().message.find("v2.x") != std::string::npos);
		request.vertex_formats = formats;
		request.target = ProgramTarget::Forward;
		bf3::core::Result<TranslatedProgram> forward = translate_program(request);
		REQUIRE(!forward.ok());
		CHECK(forward.error().message.find("v1.x reads vertex element 0") != std::string::npos);
	}

	TEST_CASE("render.programs.unit: a texture read only by single channels is a mask, and texture usage picks the UV "
			  "set") {
		const std::vector<uint8_t> ps_blob = layer_pixel_program(), vs_blob = layer_vertex_program();
		bf3::core::Result<dxbc::Program> ps = dxbc::parse(ps_blob);
		bf3::core::Result<dxbc::Program> vs = dxbc::parse(vs_blob);
		REQUIRE(ps.ok());
		REQUIRE(vs.ok());
		const std::vector<uint32_t> formats = { 10, kDxgiR16G16B16A16Float };
		const std::vector<uint32_t> bound = { 1, 2, 3 };
		ProgramAnalysis analysis;
		ProgramRequest request;
		request.pixel = &*ps;
		request.vertex = &*vs;
		request.vertex_formats = formats;
		request.bound_textures = bound;
		request.texture_usage = [](uint32_t) { return kUsageTexCoord1; };
		request.analysis = &analysis;
		bf3::core::Result<TranslatedProgram> t = translate_program(request);
		REQUIRE_MESSAGE(t.ok(), (t.ok() ? std::string() : t.error().message));
		CHECK(t->code.find("vec4 v2 = vec4((UV2.x * 1.0 + 0.0), (UV2.y * 1.0 + 0.0), 0.0, 0.0);") != std::string::npos);
		CHECK(analysis.sliced);
		CHECK(analysis.textures == std::vector<uint32_t>{ 1, 2, 3 });
		CHECK(analysis.mask_only == std::vector<uint32_t>{ 1 });
		const std::vector<uint32_t> colour = { 10, kDxgiB8G8R8A8Unorm };
		request.vertex_formats = colour;
		request.texture_usage = nullptr;
		bf3::core::Result<TranslatedProgram> c = translate_program(request);
		REQUIRE(c.ok());
		CHECK(c->code.find("vec4 v2 = vec4((COLOR.z * 1.0 + 0.0), (COLOR.y * 1.0 + 0.0), 0.0, 0.0);") !=
				std::string::npos);
	}
}

namespace {

constexpr const char *kCensusLevels[] = { "MP_001", "MP_003", "MP_007", "MP_011", "MP_012", "MP_013", "MP_017",
	"MP_018", "MP_Subway", "XP1_001", "XP1_002", "XP1_003", "XP1_004", "XP2_Factory", "XP2_Office", "XP2_Palace",
	"XP2_Skybar", "XP3_Alborz", "XP3_Desert", "XP3_Shield", "XP3_Valley", "XP4_FD", "XP4_Parl", "XP4_Quake",
	"XP4_Rubble", "XP5_001", "XP5_002", "XP5_003", "XP5_004" };
constexpr const char *kCensusMode = "TeamDeathMatchC0";
constexpr unsigned kCensusLanes = 4;
constexpr std::string_view kMaskColourLedger[] = {
	"architecture/crane_01/crane_metal_shader",
	"architecture/shaders/facadepresetinterior_destrvolume_colorcontrol",
	"architecture/vehicleshed/ss_vehicleshed_metalplates_01",
	"architecture/warehouse_02/ss_warehouse_01",
	"levels/mp_011/paving/shaders/mp_011_pavementasphalt02",
	"levels/mp_012/objects/pipeline_01_variations/pipeline_01_mp012shader",
	"levels/mp_018/objects/rock01cluster_mp_018_shader",
	"levels/mp_018/objects/rock_01b_mp_018_shader",
	"levels/mp_018/objects/rock_01c_mp_018_shader",
	"levels/mp_018/objects/rock_01d_mp_018_shader",
	"levels/mp_018/objects/rock_01e_mp_018_shader",
	"levels/mp_018/objects/wetasphalt_01__mp018",
	"levels/mp_canyon/props/railwaybridge_01/ss_railwaybridge_01_gravel",
	"levels/mp_subway/objects/road/shaders/mp_subway_asphalt_02",
	"levels/mp_whitepeak/objects/rock_01_snow",
	"levels/mp_whitepeak/objects/rock_01_wall_snow",
	"levels/mp_whitepeak/objects/rock_01_wall_snow_lod",
	"levels/sp_paris/architecture/office/shaders/carpet_01",
	"levels/sp_paris/architecture/office/shaders/carpet_02",
	"levels/sp_paris/architecture/office/shaders/carpet_03",
	"levels/sp_paris/architecture/streets/shaders/sp_paris_asphalt_02",
	"levels/sp_valley/props/rock_01c_main_copy",
	"levels/xp2_office/objects/scaffoldingplasticwrap_01_shader",
	"levels/xp2_palace/objects/backdrop_01/backdrop_01_roofshader",
	"levels/xp2_skybar/shaders/floor_outside_pool",
	"levels/xp2_skybar/shaders/floor_outside_wooddark_shader",
	"levels/xp3_alborz/props/breakableice/breakableice_01_ss",
	"levels/xp3_desert/objects/ss_groundimpact_01_bandar",
	"levels/xp3_legrandval/objectvariations/ss_legrandval_stonebridge_01_road",
	"levels/xp4_earthquake/objects/doorframebroken_ss",
	"levels/xp4_earthquake/objects/earthquakeside_ss",
	"levels/xp4_earthquake/objects/ss_constrhouse_wrap_01_xp4",
	"levels/xp4_parliament/objects/signs/wallsign_01_shader",
	"levels/xp4_rubble/objects/tarmacpatch_01/ss_tarmacpatches_01",
	"levels/xp5_001/objects/forestledge_01_top_alpha_normalfix",
	"levels/xp5_002/architecture/warehouse_02_xp5_002/ss_warehouse_02_destrvolume_xp5_002",
	"levels/xp5_002/objects/fencemetalbarbwire_02_xp5_002/fencemetalbarbwire_02_cloth_lod_xp5_002",
	"levels/xp5_002/objects/fencemetalbarbwire_02_xp5_002/fencemetalbarbwire_02_cloth_ss_xp5_002",
	"levels/xp5_004/objects/ss_xp5_004_guardtower_01",
	"levels/xp5_004/objects/ss_xp5_004_securitybooth_01",
	"objects/beammetal/beamshader01",
	"objects/fencemetalbarbwire_02/fencemetalbarbwire_02_cloth_lod",
	"objects/fencemetalbarbwire_02/fencemetalbarbwire_02_cloth_ss",
	"objects/fencemetalbarbwire_02/fencemetalbarbwire_02_yellowcloth_lod",
	"objects/interiordetails/officedetails_01/officedetails_01_metal_01",
	"objects/tarmacpatches_01/ss_tarmacpatch_03_main",
	"props/streetprops/citybus_03/citybus_03_ruin_interior",
	"props/streetprops/electricitytower_01/electricitytower_01_cableshader",
	"props/streetprops/parasol_01/parasol_02_shader",
	"props/streetprops/parasol_01/parasol_02_v2",
	"props/vehicles/civiliancar_04/civiliancar_04_variations",
	"shaders/presets/awningpreset",
	"xp2/commontextures/shaders/ss_metaltile_01",
	"xp2/commontextures/shaders/ss_woodplanks_brown_01_caustics",
	"xp2/commontextures/ss_brickwallwhite_01",
	"xp2/objects/officedivider_01/officedivider_01_shader",
	"xp2/objects/scaffolding_01_tarpaulin/shaders/scaffolding_01_tarpaulin_1",
	"xp2/objects/scaffolding_01_tarpaulin/shaders/scaffolding_01_tarpaulin_3",
	"xp2/objects/scaffolding_01_tarpaulin/shaders/scaffolding_01_tarpaulin_4",
	"xp2/objects/skybararmchair_02/ss_wood",
	"xp2/objects/skybarsigns/signs_shader",
	"xp2/objects/skybarsofa_02/ss_skybarsofa_02_alpha",
	"xp2/objects/wallsquares_01/ss_wallsquares_01",
	"xp3/objects/bridgeconcrete_01/ss_bridgeconcrete_01_gravel",
	"xp3/objects/constrhouse_02_xp3/ss_constrhouse_wrap_01_xp3",
	"xp3/objects/icefloe_01/ss_icefloe_01",
	"xp4/objects/flags/xp4_flag_01_shader",
	"xp_raw/architecture/constrhouse_01/constrhouse_wrap_01shader",
	"xp_raw/architecture/construction_building_01/construction_buildingtarp_01_alpha_lodshader",
	"xp_raw/architecture/construction_building_01/construction_buildingtarp_01_alpha_shader",
	"xp_raw/architecture/construction_building_01/construction_buildingtarp_01_lodshader",
	"xp_raw/architecture/construction_building_01/construction_buildingtarp_01_shader",
	"xp_raw/architecture/hangar_01/roof_edgedetails_01",
	"xp_raw/architecture/resort_bungalow_01/plasticwrap_01_shader",
	"xp_raw/ground/groundimpact_01/groundimpact_01_main",
	"xp_raw/props/fence_wakeisland/fence_wakeisland_01_shader",
};
constexpr std::string_view kHiddenLedger[] = {
	"levels/mp_007/props/m_mp007_waterwash_01",
	"levels/mp_011/decals/shaders/mp_011_meshdecal_dirt01",
	"levels/mp_018/terrain/mp018_wave",
	"levels/sp_tank/props/backdrop_wires_main",
	"levels/xp1_002/objects/xp1_002_wave",
	"levels/xp5_001/meshdecal_dirt01_xp5_shader",
	"levels/xp5_001/objects/hover_jetflame",
	"leveltestranges/oldvenice/level_02/ground/decalasphaltcracks_01",
	"objects/busstopbig_01/busstopbig_crackedglass",
	"xp2/objects/cellarpoolarea_01/shaders/poolwater_1",
	"xp2/objects/palacecourtyardground_01/ss_palacecourtyardground_01_water",
	"xp2/objects/wallsquares_01/ss_wallsquares_02",
};

std::string ledger_name(const std::string &p_entry) {
	return p_entry.substr(0, p_entry.find(": "));
}

template <size_t N> bool in_ledger(const std::string_view (&p_ledger)[N], const std::string &p_name) {
	return std::find(std::begin(p_ledger), std::end(p_ledger), p_name) != std::end(p_ledger);
}

struct LevelCensus {
	std::string error;
	size_t drawn = 0, programs = 0, plain = 0, forward = 0, unclassified = 0;
	std::vector<std::string> mask_colours;
	std::vector<std::string> hidden;
};

LevelCensus level_census(const std::shared_ptr<bf3::core::Install> &p_install, const char *p_level) {
	using namespace bf3::core;
	LevelCensus out;
	Result<std::unique_ptr<level::LoadedLevel>> loaded = level::load_level(p_install, p_level, kCensusMode);
	if (!loaded) {
		out.error = loaded.error().message;
		return out;
	}
	Result<std::unique_ptr<level::World>> world = level::World::traverse(**loaded);
	if (!world) {
		out.error = world.error().message;
		return out;
	}
	Result<level::MeshCensus> census = level::mesh_census(**loaded, **world);
	if (!census) {
		out.error = census.error().message;
		return out;
	}
	Result<LevelScenePlan> plan = plan_level_scene(**loaded, **world, *census);
	if (!plan) {
		out.error = plan.error().message;
		return out;
	}
	std::vector<bool> used(plan->materials.size(), false);
	for (const SceneMeshPlan &m : plan->meshes) {
		for (uint32_t s : m.surface_materials) {
			if (s < used.size()) {
				used[s] = true;
			}
		}
	}
	for (size_t i = 0; i < plan->materials.size(); ++i) {
		const SceneMaterialPlan &mat = plan->materials[i];
		if (!used[i] || !mat.look) {
			continue;
		}
		++out.drawn;
		if (mat.program >= 0) {
			++out.programs;
			out.forward += mat.blend ? 1 : 0;
			continue;
		}
		out.plain += mat.look->graph_plain ? 1 : 0;
		out.unclassified +=
				!mat.blend && !mat.glass && !mat.look->graph_refusal.empty() && !mat.look->graph_mask_colour ? 1 : 0;
		if (!mat.blend && !mat.glass && mat.look->graph_mask_colour) {
			out.mask_colours.push_back(
					to_lower_ascii(mat.slot.variation_shader.empty() ? mat.slot.shader : mat.slot.variation_shader) +
					": " + mat.look->graph_refusal);
		}
	}
	for (const SceneUndrawnMaterial &u : plan->undrawn) {
		if (!u.refusal.empty()) {
			out.hidden.push_back(to_lower_ascii(u.slot.shader) + ": " + u.refusal);
		}
	}
	return out;
}

}

TEST_SUITE("render-programs") {
	TEST_CASE("render.programs: no level draws a material's mask as its colour or hides a mask graph outside the "
			  "ledger of "
			  "programs the translator refuses") {
		bf3::core::InstallReader reader;
		if (!reader.locate({})) {
			bf3::test::skip_install_case(reader.describe().c_str());
			return;
		}
		const bf3::core::InstallSnapshot before = reader.snapshot();
		bf3::core::Result<std::shared_ptr<bf3::core::Install>> install = bf3::core::Install::open(reader);
		REQUIRE(install.ok());
		constexpr size_t kLevels = std::size(kCensusLevels);
		std::vector<LevelCensus> results(kLevels);
		bf3::core::parallel_for(
				kLevels, [&](size_t i) { results[i] = level_census(*install, kCensusLevels[i]); }, kCensusLanes);
		size_t drawn = 0, programs = 0, plain = 0, forward = 0, mask_colours = 0, unclassified = 0, hidden = 0;
		std::map<std::string, size_t> reasons;
		std::set<std::string> seen_mask, seen_hidden;
		for (size_t i = 0; i < kLevels; ++i) {
			const LevelCensus &c = results[i];
			REQUIRE_MESSAGE(c.error.empty(), kCensusLevels[i], ": ", c.error);
			drawn += c.drawn;
			programs += c.programs;
			plain += c.plain;
			forward += c.forward;
			mask_colours += c.mask_colours.size();
			unclassified += c.unclassified;
			hidden += c.hidden.size();
			for (const std::string &r : c.mask_colours) {
				seen_mask.insert(ledger_name(r));
				CHECK_MESSAGE(in_ledger(kMaskColourLedger, ledger_name(r)), kCensusLevels[i], " draws the mask of ", r);
				++reasons[r.substr(r.find(": ") + 2)];
			}
			for (const std::string &h : c.hidden) {
				seen_hidden.insert(ledger_name(h));
				CHECK_MESSAGE(in_ledger(kHiddenLedger, ledger_name(h)), kCensusLevels[i], " hides ", h);
			}
			std::printf("render.programs %s: %zu drawn materials, %zu through their programs (%zu forward), %zu plain, "
						"%zu draw a mask as "
						"colour, %zu refused unclassified, %zu hidden\n",
					kCensusLevels[i], c.drawn, c.programs, c.forward, c.plain, c.mask_colours.size(), c.unclassified,
					c.hidden.size());
		}
		std::printf("render.programs: %zu levels, %zu drawn materials, %zu through their programs (%zu forward), %zu "
					"plain, %zu draw a mask "
					"as colour, %zu refused unclassified, %zu hidden\n",
				kLevels, drawn, programs, forward, plain, mask_colours, unclassified, hidden);
		for (const auto &[why, n] : reasons) {
			std::printf("render.programs mask colour %zu: %s\n", n, why.c_str());
		}
		for (std::string_view name : kMaskColourLedger) {
			CHECK_MESSAGE(seen_mask.count(std::string(name)) == 1, name,
					" is in the mask colour ledger but no longer draws its mask as colour");
		}
		for (std::string_view name : kHiddenLedger) {
			CHECK_MESSAGE(seen_hidden.count(std::string(name)) == 1, name, " is in the hidden ledger but is drawn");
		}
		CHECK(programs > 0);
		const bf3::core::SnapshotDiff diff = bf3::core::diff_snapshots(before, reader.snapshot());
		std::printf("install.unchanged %s\n", diff.clean() ? "PASS" : diff.summary().c_str());
		CHECK_MESSAGE(diff.clean(), diff.summary());
	}
}
