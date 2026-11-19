#include <doctest/doctest.h>

#include "bf3/core/install.h"
#include "bf3/render/dxbc.h"
#include "bf3/render/light_pass.h"
#include "bf3/render/program_database.h"
#include "test_support.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace bf3;

namespace {

void put32(std::vector<uint8_t> &r_out, uint32_t p_v) {
	for (int i = 0; i < 4; ++i) {
		r_out.push_back(uint8_t(p_v >> (8 * i)));
	}
}

void put_program(std::vector<uint8_t> &r_out, std::string_view p_name, uint32_t p_size) {
	r_out.insert(r_out.end(), { 0, 0, 0, 0 });
	r_out.insert(r_out.end(), p_name.begin(), p_name.end());
	r_out.push_back(0);
	put32(r_out, p_size);
	const size_t at = r_out.size();
	r_out.insert(r_out.end(), { 'D', 'X', 'B', 'C' });
	r_out.resize(at + p_size, 0);
	std::memcpy(r_out.data() + at + 24, &p_size, 4);
}

std::vector<uint8_t> synthetic_database() {
	std::vector<uint8_t> out(12, 0);
	put_program(out, "Dx11/First", 40);
	put_program(out, "Dx11/Second_a=1,b=0", 32);
	out.insert(out.end(), 7, 0);
	return out;
}

struct Source {
	std::string resource;
	uint8_t lane = 0;
};

std::optional<Source> written_from(const render::dxbc::Program &p_program, uint32_t p_output, uint8_t p_lane) {
	using render::dxbc::Op;
	using render::dxbc::OperandType;
	for (size_t i = 0; i < p_program.code.size(); ++i) {
		const render::dxbc::Instruction &mov = p_program.code[i];
		if (!mov.is(Op::Mov) || !mov.operands[0].is(OperandType::Output) || mov.operands[0].index[0] != p_output ||
				((mov.operands[0].write_mask() >> p_lane) & 1u) == 0 || !mov.operands[1].is(OperandType::Temp)) {
			continue;
		}
		const uint32_t reg = mov.operands[1].index[0];
		const uint8_t lane = mov.operands[1].lane(p_lane);
		for (size_t j = i; j-- > 0;) {
			const render::dxbc::Instruction &ld = p_program.code[j];
			if (!ld.is(Op::Ld) || !ld.operands[0].is(OperandType::Temp) || ld.operands[0].index[0] != reg ||
					((ld.operands[0].write_mask() >> lane) & 1u) == 0) {
				continue;
			}
			const render::dxbc::Binding *b =
					p_program.binding(render::dxbc::BindingType::Texture, ld.operands[2].index[0]);
			return Source{ b != nullptr ? b->name : std::string(), ld.operands[2].lane(lane) };
		}
	}
	return std::nullopt;
}

}

TEST_SUITE("render-program-database-unit") {
	TEST_CASE("render.program_database.unit: names, blobs, truncations and broken records") {
		const std::vector<uint8_t> db = synthetic_database();
		core::Result<std::vector<render::DatabaseProgram>> parsed = render::parse_program_database(db);
		REQUIRE(parsed.ok());
		REQUIRE(parsed->size() == 2);
		CHECK((*parsed)[0].name == "Dx11/First");
		CHECK((*parsed)[0].bytes.size() == 40);
		CHECK((*parsed)[1].name == "Dx11/Second_a=1,b=0");
		CHECK((*parsed)[1].bytes.size() == 32);

		size_t errors = 0, partial = 0;
		for (size_t n = 0; n < db.size(); ++n) {
			core::Result<std::vector<render::DatabaseProgram>> cut =
					render::parse_program_database(std::span<const uint8_t>(db.data(), n));
			errors += cut.ok() ? 0 : 1;
			partial += cut.ok() && cut->size() < 2 ? 1 : 0;
		}
		CHECK(errors + partial == db.size() - 7);

		std::vector<uint8_t> bad_size = db;
		const size_t second =
				std::string_view(reinterpret_cast<const char *>(bad_size.data()), bad_size.size()).rfind("DXBC");
		bad_size[second - 4] ^= 0x01;
		CHECK_FALSE(render::parse_program_database(bad_size).ok());

		std::vector<uint8_t> no_terminator = db;
		no_terminator[second - 5] = 'x';
		CHECK_FALSE(render::parse_program_database(no_terminator).ok());

		std::vector<uint8_t> no_platform = db;
		std::memcpy(no_platform.data() + 16 + 4, "_", 1);
		CHECK_FALSE(render::parse_program_database(no_platform).ok());

		CHECK_FALSE(render::parse_program_database({}).ok());
		CHECK_FALSE(render::parse_program_database(std::vector<uint8_t>(64, 0)).ok());
	}
}

TEST_SUITE("render-light-pass") {
	TEST_CASE("render.light_pass: the deferred outdoor light's specular lobe and the terrain's specular lane") {
		test::Mounted m;
		if (!test::open_or_skip(m)) {
			return;
		}
		core::Result<render::ProgramDatabase> db = render::read_program_database(*m.install);
		REQUIRE_MESSAGE(db.ok(), (db.ok() ? std::string() : db.error().message));
		CHECK(db->programs.size() == 2415);

		core::Result<render::dxbc::Program> outdoor =
				db->program(render::kOutdoorLightProgram, render::dxbc::kPixelProgram);
		REQUIRE_MESSAGE(outdoor.ok(), (outdoor.ok() ? std::string() : outdoor.error().message));
		CHECK(outdoor->binding(render::dxbc::BindingType::Texture, "g_gbufferTexture1") != nullptr);
		CHECK(outdoor->variable("$Globals", "g_keyColor") != nullptr);

		size_t measured = 0, lit = 0;
		for (const render::DatabaseProgram &p : db->programs) {
			const bool outdoor_light = p.name.find("/DeferredOutdoorLight_") != std::string::npos ||
					(p.name.find("/DeferredOutdoorLightTile_") != std::string::npos &&
							p.name.find("key=1") != std::string::npos &&
							p.name.find("specular=1") != std::string::npos);
			if (!outdoor_light) {
				continue;
			}
			core::Result<render::dxbc::Program> program = render::dxbc::parse(p.bytes);
			REQUIRE_MESSAGE(program.ok(), p.name);
			if (program->kind != render::dxbc::kPixelProgram) {
				continue;
			}
			++lit;
			core::Result<render::LightPassLobe> lobe = render::measure_light_pass(*program);
			CHECK_MESSAGE(lobe.ok(), p.name, ": ", (lobe.ok() ? std::string() : lobe.error().message));
			if (!lobe) {
				continue;
			}
			++measured;
			CHECK_MESSAGE(lobe->power_scale == render::kSpecularPowerScale, p.name);
			CHECK_MESSAGE(lobe->power_bias == render::kSpecularPowerBias, p.name);
			CHECK_MESSAGE(lobe->norm_add == render::kSpecularNormAdd, p.name);
			CHECK_MESSAGE(lobe->norm_scale == render::kSpecularNormScale, p.name);
			CHECK_MESSAGE(lobe->envmap_mip_scale == render::kEnvmapMipScale, p.name);
		}
		std::printf(
				"render.light_pass: %zu programs; %zu outdoor light pixel programs with a specular term, %zu measured: "
				"power exp2(%g s + %g), normalisation (power + %g) x %g, envmap mip (1 - s) x %g\n",
				db->programs.size(), lit, measured, double(render::kSpecularPowerScale),
				double(render::kSpecularPowerBias), double(render::kSpecularNormAdd),
				double(render::kSpecularNormScale), double(render::kEnvmapMipScale));
		CHECK(lit >= 96);
		CHECK(measured == lit);

		core::Result<render::dxbc::Program> page_a =
				db->program("Dx11/TerrainEncodeSurface_A", render::dxbc::kPixelProgram);
		core::Result<render::dxbc::Program> page_b =
				db->program("Dx11/TerrainEncodeSurface_B", render::dxbc::kPixelProgram);
		REQUIRE((page_a.ok() && page_b.ok()));
		const std::optional<Source> specular = written_from(*page_b, 0, 0);
		REQUIRE(specular.has_value());
		CHECK(specular->resource == "g_source1");
		CHECK(specular->lane == 1);
		const std::optional<Source> normal_x = written_from(*page_a, 0, 3);
		const std::optional<Source> normal_y = written_from(*page_b, 0, 1);
		const std::optional<Source> normal_z = written_from(*page_b, 0, 3);
		REQUIRE((normal_x && normal_y && normal_z));
		CHECK(normal_x->resource == "g_source0");
		CHECK(normal_x->lane == 0);
		CHECK(normal_y->resource == "g_source1");
		CHECK(normal_y->lane == 2);
		CHECK(normal_z->resource == "g_source1");
		CHECK(normal_z->lane == 0);
		std::printf(
				"render.light_pass: terrain atlas page 1 x (the surface shader's specular) is %s.%c, normal (%s.%c, "
				"%s.%c, %s.%c)\n",
				specular->resource.c_str(), "xyzw"[specular->lane], normal_x->resource.c_str(), "xyzw"[normal_x->lane],
				normal_y->resource.c_str(), "xyzw"[normal_y->lane], normal_z->resource.c_str(), "xyzw"[normal_z->lane]);
	}
}
