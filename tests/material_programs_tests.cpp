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

std::vector<uint32_t> cb_relative(uint32_t p_slot, uint32_t p_base, uint32_t p_swizzle, uint32_t p_temp) {
	return { 2u | kSwizzleMode << 2 | p_swizzle << 4 | kConstantBuffer << 12 | 2u << 20 | 3u << 25, p_slot, p_base,
		2u | 2u << 2 | kTemp << 12 | 1u << 20, p_temp };
}

std::vector<uint32_t> int_imm(uint32_t p_x, uint32_t p_y, uint32_t p_z, uint32_t p_w) {
	return { 2u | kImmediate << 12, p_x, p_y, p_z, p_w };
}

std::vector<uint8_t> world_pixel_program(uint32_t p_target = 1, uint32_t p_mask = 0x7) {
	return container({ { "RDEF", rdef({ { "texture_Texture", 2, 1 } }, nullptr, {}) },
			{ "ISGN", signature({ { "SV_Position", 0, 1, 0, 0xF }, { "TEXCOORD", 0, 0, 1, 0xF } }) },
			{ "OSGN", signature({ { "SV_Target", 0, 0, 0, 0xF }, { "SV_Target", 1, 0, 1, 0xF } }) },
			{ "SHDR",
					code_chunk(0, 1,
							{ ins(69,
									  { dst(kTemp, 0, p_mask), src(kInput, 1, swz(0, 2, 0, 0)), src(kResource, 1),
											  sampler(0) }),
									ins(75, { dst(kOutput, p_target, p_mask), src(kTemp, 0) }) }) } });
}

std::vector<uint8_t> world_vertex_program() {
	return container({ { "RDEF", rdef({ { "$Globals", 0, 0 } }, "$Globals", { { "worldMatrix", 0, 64 } }) },
			{ "ISGN", signature({ { "TEXCOORD", 0, 0, 0, 0xF } }) },
			{ "OSGN", signature({ { "SV_Position", 0, 1, 0, 0xF }, { "TEXCOORD", 0, 0, 1, 0xF } }) },
			{ "SHDR",
					code_chunk(1, 1,
							{ ins(54, { dst(kTemp, 0, 0x7), src(kInput, 0) }),
									ins(54, { dst(kTemp, 0, 0x8), imm(1, 1, 1, 1) }),
									ins(17, { dst(kOutput, 1, 0x1), src(kTemp, 0), cb(0, 0, kIdentity) }),
									ins(17, { dst(kOutput, 1, 0x2), src(kTemp, 0), cb(0, 1, kIdentity) }),
									ins(17, { dst(kOutput, 1, 0x4), src(kTemp, 0), cb(0, 2, kIdentity) }),
									ins(54, { dst(kOutput, 0, 0xF), src(kTemp, 0) }) }) } });
}

std::vector<uint8_t> instanced_vertex_program() {
	return container({ { "RDEF",
							   rdef({ { "instanceVectorBuffer", 2, 0 }, { "$Globals", 0, 0 } }, "$Globals",
									   { { "dxVectorBufferOffsets", 0, 16 }, { "boneVectors", 16, 2880 } }) },
			{ "ISGN",
					signature({ { "TEXCOORD", 0, 0, 0, 0xF }, { "TEXCOORD", 1, 0, 1, 0xF },
							{ "SV_InstanceID", 0, 8, 2, 0x1 } }) },
			{ "OSGN", signature({ { "SV_Position", 0, 1, 0, 0xF }, { "TEXCOORD", 0, 0, 1, 0xF } }) },
			{ "SHDR",
					code_chunk(1, 6,
							{ ins(54, { dst(kTemp, 0, 0x1), src(kInput, 1, swz(0, 0, 0, 0)) }),
									ins(49,
											{ dst(kTemp, 0, 0x1), imm(0, 0, 0, 0),
													cb_relative(0, 1, swz(0, 0, 0, 0), 0) }),
									ins(31 | 1u << 18, { src(kTemp, 0, swz(0, 0, 0, 0)) }),
									ins(86, { dst(kTemp, 1, 0x1), src(kInput, 2, swz(0, 0, 0, 0)) }),
									ins(50,
											{ dst(kTemp, 1, 0x1), src(kTemp, 1, swz(0, 0, 0, 0)),
													cb(0, 0, swz(1, 1, 1, 1)), cb(0, 0, swz(0, 0, 0, 0)) }),
									ins(28, { dst(kTemp, 1, 0x1), src(kTemp, 1, swz(0, 0, 0, 0)) }),
									ins(30,
											{ dst(kTemp, 1, 0x6), src(kTemp, 1, swz(0, 0, 0, 0)),
													int_imm(0, 1, 2, 0) }),
									ins(45, { dst(kTemp, 2, 0xF), src(kTemp, 1, swz(0, 0, 0, 0)), src(kResource, 0) }),
									ins(45, { dst(kTemp, 3, 0xF), src(kTemp, 1, swz(1, 1, 1, 1)), src(kResource, 0) }),
									ins(45, { dst(kTemp, 4, 0xF), src(kTemp, 1, swz(2, 2, 2, 2)), src(kResource, 0) }),
									ins(54, { dst(kTemp, 5, 0x7), src(kInput, 0) }),
									ins(54, { dst(kTemp, 5, 0x8), imm(1, 1, 1, 1) }),
									ins(17, { dst(kOutput, 1, 0x1), src(kTemp, 5), src(kTemp, 2) }),
									ins(17, { dst(kOutput, 1, 0x2), src(kTemp, 5), src(kTemp, 3) }),
									ins(17, { dst(kOutput, 1, 0x4), src(kTemp, 5), src(kTemp, 4) }),
									ins(54, { dst(kOutput, 0, 0xF), src(kTemp, 5) }), ins(18, {}),
									ins(54, { dst(kOutput, 1, 0xF), imm(0, 0, 0, 0) }),
									ins(54, { dst(kOutput, 0, 0xF), imm(0, 0, 0, 0) }), ins(21, {}) }) } });
}

std::vector<uint8_t> cutout_pixel_program(uint32_t p_mask_reg) {
	return container({ { "RDEF", rdef({ { "texture_Texture", 2, 1 }, { "texture_Texture2", 2, 2 } }, nullptr, {}) },
			{ "ISGN", signature({ { "SV_Position", 0, 1, 0, 0xF }, { "TEXCOORD", 1, 0, 2, 0x3 } }) },
			{ "OSGN", signature({ { "SV_Target", 0, 0, 0, 0xF }, { "SV_Target", 1, 0, 1, 0xF } }) },
			{ "SHDR",
					code_chunk(0, 2,
							{ ins(69,
									  { dst(kTemp, 0, 0x1), src(kInput, 2, swz(0, 1, 0, 0)), src(kResource, 1),
											  sampler(0) }),
									ins(49,
											{ dst(kTemp, p_mask_reg, 0x1), imm(0.9f, 0.9f, 0.9f, 0.9f),
													src(kTemp, 0) }),
									ins(32,
											{ dst(kTemp, 0, 0x1), src(kTemp, 0, swz(0, 0, 0, 0)),
													int_imm(0, 0, 0, 0) }),
									ins(13 | 1u << 18, { src(kTemp, 0, swz(0, 0, 0, 0)) }),
									ins(69,
											{ dst(kTemp, 1, 0x7), src(kInput, 2, swz(0, 1, 0, 0)), src(kResource, 2),
													sampler(0) }),
									ins(75, { dst(kOutput, 1, 0x7), src(kTemp, 1) }) }) } });
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
		CHECK(computed.error().message.find("vertex input v1 has no element usage") != std::string::npos);
		request.vertex_formats = formats;
		request.target = ProgramTarget::Forward;
		bf3::core::Result<TranslatedProgram> forward = translate_program(request);
		REQUIRE(!forward.ok());
		CHECK(forward.error().message.find("interpolant v1.w is not declared") != std::string::npos);
	}

	TEST_CASE("render.programs.unit: an interpolant the vertex program computes is a varying of the translated vertex "
			  "stage") {
		const std::vector<uint8_t> ps_blob = world_pixel_program(), vs_blob = world_vertex_program();
		bf3::core::Result<dxbc::Program> ps = dxbc::parse(ps_blob);
		bf3::core::Result<dxbc::Program> vs = dxbc::parse(vs_blob);
		REQUIRE(ps.ok());
		REQUIRE(vs.ok());
		const std::vector<uint32_t> formats = { 10 };
		const std::vector<VertexInputLanes> lanes = { { { { 1, 0 }, { 1, 1 }, { 1, 2 }, { 9, 0 } } } };
		const std::vector<uint32_t> bound = { 1 };
		ProgramRequest request;
		request.pixel = &*ps;
		request.vertex = &*vs;
		request.vertex_formats = formats;
		request.vertex_lanes = lanes;
		request.bound_textures = bound;
		bf3::core::Result<TranslatedProgram> t = translate_program(request);
		REQUIRE_MESSAGE(t.ok(), (t.ok() ? std::string() : t.error().message));
		const std::string &code = t->code;
		CHECK(code.find("varying vec4 vso1;\nvoid vertex() {\n") != std::string::npos);
		CHECK(code.find("vec4 v0 = vec4(VERTEX.x, VERTEX.y, VERTEX.z, 0.0);") != std::string::npos);
		CHECK(code.find("vec4 cb0_2 = vec4(MODEL_MATRIX[0][2], MODEL_MATRIX[1][2], MODEL_MATRIX[2][2], "
						"MODEL_MATRIX[3][2]);") != std::string::npos);
		CHECK(code.find("o1.z = (vec4(dot((r0.xyzw), (cb0_2.xyzw)))).z;") != std::string::npos);
		CHECK(code.find("\tvso1 = o1;\n}\n") != std::string::npos);
		CHECK(code.find("vec4 v1 = vec4(vso1.x, 0.0, vso1.z, 0.0);") != std::string::npos);
		CHECK(code.find("o0.") == std::string::npos);
		const std::vector<uint8_t> forward_blob = world_pixel_program(0, 0xF);
		bf3::core::Result<dxbc::Program> forward_ps = dxbc::parse(forward_blob);
		REQUIRE(forward_ps.ok());
		ProgramRequest forward = request;
		forward.pixel = &*forward_ps;
		forward.target = ProgramTarget::Forward;
		bf3::core::Result<TranslatedProgram> f = translate_program(forward);
		REQUIRE_MESSAGE(f.ok(), (f.ok() ? std::string() : f.error().message));
		CHECK(f->code.find("depth_draw_never, unshaded") != std::string::npos);
		CHECK(f->code.find("varying vec4 vso1;\nvoid vertex() {\n") != std::string::npos);
		CHECK(f->code.find("o1.z = (vec4(dot((r0.xyzw), (cb0_2.xyzw)))).z;") != std::string::npos);
		CHECK(f->code.find("vec4 v1 = vec4(vso1.x, 0.0, vso1.z, 0.0);") != std::string::npos);
		CHECK(f->code.find("ALBEDO = o0.rgb;") != std::string::npos);
		const std::vector<VertexInputLanes> unknown(1);
		request.vertex_lanes = unknown;
		bf3::core::Result<TranslatedProgram> refused = translate_program(request);
		REQUIRE(!refused.ok());
		CHECK(refused.error().message.find("vertex input v0.x reads usage 0") != std::string::npos);
	}

	TEST_CASE("render.programs.unit: instance vectors are the world rows, and a part's shown branch is the vertex "
			  "stage") {
		const std::vector<uint8_t> ps_blob = world_pixel_program(), vs_blob = instanced_vertex_program();
		bf3::core::Result<dxbc::Program> ps = dxbc::parse(ps_blob);
		bf3::core::Result<dxbc::Program> vs = dxbc::parse(vs_blob);
		REQUIRE(ps.ok());
		REQUIRE(vs.ok());
		CHECK(dxbc::format_instruction(vs->code[1]) ==
				"lt r0.x, l(0.000000, 0.000000, 0.000000, 0.000000), cb0[1+rel].xxxx");
		CHECK(dxbc::format_instruction(vs->code[7]) == "ld r2.xyzw, r1.xxxx, t0.xyzw");
		const std::vector<uint32_t> formats = { 10, 10 };
		const std::vector<VertexInputLanes> lanes = { { { { 1, 0 }, { 1, 1 }, { 1, 2 }, { 9, 0 } } },
			{ { { 50, 0 }, { 50, 1 }, { 50, 2 }, { 50, 3 } } } };
		const std::vector<uint32_t> bound = { 1 };
		ProgramRequest request;
		request.pixel = &*ps;
		request.vertex = &*vs;
		request.vertex_formats = formats;
		request.vertex_lanes = lanes;
		request.bound_textures = bound;
		bf3::core::Result<TranslatedProgram> t = translate_program(request);
		REQUIRE_MESSAGE(t.ok(), (t.ok() ? std::string() : t.error().message));
		const std::string &code = t->code;
		CHECK(code.find("r4.xyzw = (vec4(MODEL_MATRIX[0][2], MODEL_MATRIX[1][2], MODEL_MATRIX[2][2], "
						"MODEL_MATRIX[3][2]).xyzw).xyzw;") != std::string::npos);
		CHECK(code.find("o1.z = (vec4(dot((r5.xyzw), (r4.xyzw)))).z;") != std::string::npos);
		const size_t vertex_at = code.find("void vertex()");
		REQUIRE(vertex_at != std::string::npos);
		const std::string stage = code.substr(vertex_at, code.find("void fragment()") - vertex_at);
		CHECK(stage.find("if") == std::string::npos);
		CHECK(stage.find("r3.") == std::string::npos);
		CHECK(code.find("vec4(0.0, 0.0, 0.0, 0.0)") == std::string::npos);
		CHECK(code.find("cb0_") == std::string::npos);
		std::vector<uint8_t> hidden = vs_blob;
		const std::vector<uint8_t> lt =
				words(ins(49, { dst(kTemp, 0, 0x1), imm(0, 0, 0, 0), cb_relative(0, 1, 0, 0) }));
		const std::vector<uint8_t> ge =
				words(ins(29, { dst(kTemp, 0, 0x1), imm(0, 0, 0, 0), cb_relative(0, 1, 0, 0) }));
		const auto at = std::search(hidden.begin(), hidden.end(), lt.begin(), lt.end());
		REQUIRE(at != hidden.end());
		std::copy(ge.begin(), ge.end(), at);
		bf3::core::Result<dxbc::Program> other = dxbc::parse(hidden);
		REQUIRE(other.ok());
		request.vertex = &*other;
		bf3::core::Result<TranslatedProgram> refused = translate_program(request);
		REQUIRE(!refused.ok());
		CHECK(refused.error().message.find("vertex program branches on if_nz r0") != std::string::npos);
	}

	TEST_CASE(
			"render.programs.unit: ieq of a comparison mask and zero is its negation; of anything else it is refused") {
		const std::vector<uint8_t> vs_blob = layer_vertex_program();
		bf3::core::Result<dxbc::Program> vs = dxbc::parse(vs_blob);
		REQUIRE(vs.ok());
		const std::vector<uint32_t> formats = { 10, kDxgiR16G16Float };
		const std::vector<uint32_t> bound = { 1, 2 };
		for (uint32_t mask_reg : { 0u, 1u }) {
			const std::vector<uint8_t> ps_blob = cutout_pixel_program(mask_reg);
			bf3::core::Result<dxbc::Program> ps = dxbc::parse(ps_blob);
			REQUIRE(ps.ok());
			CHECK(dxbc::format_instruction(ps->code[2]) ==
					"ieq r0.x, r0.xxxx, l(0.000000, 0.000000, 0.000000, 0.000000)");
			ProgramRequest request;
			request.pixel = &*ps;
			request.vertex = &*vs;
			request.vertex_formats = formats;
			request.bound_textures = bound;
			bf3::core::Result<TranslatedProgram> t = translate_program(request);
			if (mask_reg == 0) {
				REQUIRE_MESSAGE(t.ok(), (t.ok() ? std::string() : t.error().message));
				CHECK(t->code.find("r0.x = (vec4(equal(r0.xxxx, vec4(0.0)))).x;") != std::string::npos);
				CHECK(t->code.find("if ((r0.xxxx).x != 0.0) { discard; }") != std::string::npos);
			} else {
				REQUIRE(!t.ok());
				CHECK(t.error().message.find("ieq of what is not a comparison mask") != std::string::npos);
			}
		}
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
	"architecture/shaders/facadepresetinterior_destrvolume_colorcontrol",
	"levels/xp5_001/objects/forestledge_01_top_alpha_normalfix",
	"levels/xp5_002/architecture/warehouse_02_xp5_002/ss_warehouse_02_destrvolume_xp5_002",
	"xp2/objects/skybarsigns/signs_shader",
};
constexpr std::pair<const char *, std::string_view> kDrawnThroughPrograms[] = {
	{ "MP_018", "levels/mp_018/objects/rock01cluster_mp_018_shader" },
	{ "MP_018", "levels/mp_018/objects/rock_01b_mp_018_shader" },
	{ "MP_018", "levels/mp_018/objects/rock_01c_mp_018_shader" },
	{ "MP_018", "levels/mp_018/objects/rock_01d_mp_018_shader" },
	{ "MP_018", "levels/mp_018/objects/rock_01e_mp_018_shader" },
	{ "XP3_Alborz", "levels/mp_whitepeak/objects/rock_01_snow" },
	{ "XP3_Alborz", "levels/mp_whitepeak/objects/rock_01_wall_snow" },
	{ "XP3_Alborz", "levels/mp_whitepeak/objects/rock_01_wall_snow_lod" },
	{ "XP3_Desert", "levels/xp3_desert/objects/ss_backdropocean_fade" },
};
constexpr std::string_view kHiddenLedger[] = {
	"levels/mp_007/props/m_mp007_waterwash_01",
	"levels/mp_018/terrain/mp018_wave",
	"levels/sp_tank/props/backdrop_wires_main",
	"levels/xp1_002/objects/xp1_002_wave",
	"levels/xp5_001/objects/hover_jetflame",
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
	std::set<std::string> through_programs;
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
			out.through_programs.insert(
					to_lower_ascii(mat.slot.variation_shader.empty() ? mat.slot.shader : mat.slot.variation_shader));
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
		for (const auto &[level, shader] : kDrawnThroughPrograms) {
			const size_t at =
					size_t(std::find_if(std::begin(kCensusLevels), std::end(kCensusLevels), [&](const char *p_level) {
						return std::string_view(p_level) == level;
					}) - std::begin(kCensusLevels));
			REQUIRE(at < kLevels);
			CHECK_MESSAGE(results[at].through_programs.count(std::string(shader)) == 1, level, " does not draw ",
					shader, " through its program");
		}
		CHECK(programs > 0);
		const bf3::core::SnapshotDiff diff = bf3::core::diff_snapshots(before, reader.snapshot());
		std::printf("install.unchanged %s\n", diff.clean() ? "PASS" : diff.summary().c_str());
		CHECK_MESSAGE(diff.clean(), diff.summary());
	}
}
