#include <doctest/doctest.h>

#include "bf3/render/dxbc.h"
#include "bf3/render/shader_database.h"
#include "bf3/render/shader_translate.h"
#include "dxbc_test_support.h"

#include <cstdint>
#include <string>
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
