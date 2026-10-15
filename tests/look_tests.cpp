// L1 look (README "L1 look"): the shader database reader, the look resolver and the visual environment arithmetic.
//
// Suite "render-look-unit": parse_shader_database on a synthetic res (fields, the sampled textures and values of a
// solution's pixel constants), every truncation and single-byte mutations of it (an error or a parse, never a crash
// or an out-of-range read), the work budget against a res whose solutions all bind one large constant block; the
// resolver's name roles, tiers, normal pairing and glass rule on a synthetic ShaderLookup; look_cubic, fog_fit,
// look_area_weight (box and polygon, fade, hostile positions), look_frame (base and area blend) and look_grade. No data.
// Suite "render-look": Metro TDM from the install (read-only; exits 77 = skipped without one): the level shader
// database parsed with its work units against the budget, the resolver opened on the mount, the environment read
// (base, areas, LUTs), and install.unchanged. Prints numbers and EBX names only.
#include <doctest/doctest.h>

#include "bf3/core/ebx.h"
#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/install_snapshot.h"
#include "bf3/core/level/level.h"
#include "bf3/core/level/world.h"
#include "bf3/core/res/res_loader.h"
#include "bf3/render/look_environment.h"
#include "bf3/render/look_materials.h"
#include "bf3/render/shader_database.h"
#include "test_support.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

using namespace bf3::core;
using namespace bf3::render;

namespace {

void put32(std::vector<uint8_t> &r_out, uint32_t p_v) {
	for (int i = 0; i < 4; ++i) {
		r_out.push_back(uint8_t(p_v >> (8 * i)));
	}
}
void put16(std::vector<uint8_t> &r_out, uint16_t p_v) {
	r_out.push_back(uint8_t(p_v));
	r_out.push_back(uint8_t(p_v >> 8));
}
void putf(std::vector<uint8_t> &r_out, float p_v) {
	uint32_t u;
	std::memcpy(&u, &p_v, 4);
	put32(r_out, u);
}
void put_cstr(std::vector<uint8_t> &r_out, const char *p_s) {
	r_out.insert(r_out.end(), p_s, p_s + std::strlen(p_s) + 1);
}
void set64(std::vector<uint8_t> &r_out, size_t p_at, uint64_t p_v) {
	for (int i = 0; i < 8; ++i) {
		r_out[p_at + size_t(i)] = uint8_t(p_v >> (8 * i));
	}
}

constexpr const char *kShaderName = "levels/test/shaders/wallshader";

// One constant block: p_textures texture entries named <name><i>, one value "Tint" = (0.25, 0.5, 0.75, 1).
std::vector<uint8_t> constant_block(uint8_t p_textures, const char *p_texture_name) {
	const size_t header = 0x40, textures_at = header, values_at = textures_at + size_t(p_textures) * 0x98;
	std::vector<uint8_t> b(values_at + 0x3C, 0);
	const uint32_t size = uint32_t(b.size());
	std::memcpy(b.data(), &size, 4);
	set64(b, 0x10, textures_at);
	set64(b, 0x18, values_at);
	b[0x35] = p_textures;
	b[0x36] = 1;
	for (size_t t = 0; t < p_textures; ++t) {
		std::string name = p_texture_name;
		if (p_textures > 1) {
			name += std::to_string(t);
		}
		std::memcpy(b.data() + textures_at + 0x98 * t + 8, name.data(), std::min<size_t>(name.size(), 0x7F));
	}
	std::memcpy(b.data() + values_at, "Tint", 4);
	const float v[4] = { 0.25f, 0.5f, 0.75f, 1.0f };
	std::memcpy(b.data() + values_at + 0x2C, v, 16);
	return b;
}

// A one-path shader database res (version 182) with one constant block, one solution and one shader using it
// p_uses times.
std::vector<uint8_t> synthetic_database(uint8_t p_block_textures = 1, uint32_t p_uses = 1, uint8_t p_solution_flags = 1, uint8_t p_surface = 0) {
	std::vector<uint8_t> body;
	put32(body, kShaderDatabaseVersion);
	put32(body, 0);
	put32(body, 1); // constant blocks
	const std::vector<uint8_t> block = constant_block(p_block_textures, "levels/test/textures/wall_grime_mask");
	body.insert(body.end(), block.begin(), block.end());
	put32(body, 0); // constant lists
	put32(body, 0); // texture lists
	put32(body, 0); // vertex shaders
	put32(body, 0); // pixel shaders
	put32(body, 0); // shader blobs
	put32(body, 1); // solutions
	{
		std::vector<uint8_t> s(64, 0);
		s[0] = 0x11;
		s[8] = p_solution_flags;
		s[9] = p_surface;
		s[10] = 0;
		set64(s, 32, 0);
		set64(s, 56, 0); // pixel constant block 0
		body.insert(body.end(), s.begin(), s.end());
	}
	put32(body, 1); // solution states
	body.insert(body.end(), 32, 0);
	put32(body, 0); // declarations
	put32(body, 1); // shaders
	put32(body, ebx_name_hash(kShaderName));
	put32(body, p_surface);
	body.push_back(0);
	body.insert(body.end(), 35, 0);
	put32(body, 1); // graph textures
	put_cstr(body, "levels/test/textures/wall_d");
	put32(body, 1);
	put32(body, kUsageTexCoord0);
	putf(body, 0.5f);
	put32(body, 1); // texture parameters
	put_cstr(body, "Normal");
	put32(body, 7);
	put32(body, 1);
	put32(body, kUsageTexCoord1);
	putf(body, 1.0f);
	put32(body, p_uses);
	for (uint32_t u = 0; u < p_uses; ++u) {
		put16(body, 0);
	}
	std::vector<uint8_t> res;
	put32(res, 1);
	put32(res, 3); // render path id
	put32(res, uint32_t(body.size()));
	res.insert(res.end(), body.begin(), body.end());
	return res;
}

bool near(float p_a, float p_b, float p_eps = 1e-4f) {
	return std::fabs(p_a - p_b) <= p_eps;
}

SurfaceShader graph_shader(const char *p_name, SurfaceShaderType p_type, std::vector<ShaderTexture> p_textures, std::vector<ShaderTextureParameter> p_parameters = {}) {
	SurfaceShader s;
	s.name_hash = ebx_name_hash(p_name);
	s.surface_type = p_type;
	s.textures = std::move(p_textures);
	s.texture_parameters = std::move(p_parameters);
	return s;
}

} // namespace

TEST_SUITE("render-look-unit") {
	TEST_CASE("render.look.unit: shader database fields, sampled textures and values") {
		const std::vector<uint8_t> res = synthetic_database();
		uint64_t work = 0;
		Result<std::vector<ShaderDatabase>> db = parse_shader_database(res, &work);
		REQUIRE(db.ok());
		REQUIRE(db->size() == 1);
		CHECK((*db)[0].render_path == 3);
		REQUIRE((*db)[0].shaders.size() == 1);
		const SurfaceShader &s = (*db)[0].shaders[0];
		CHECK(s.name_hash == ebx_name_hash(kShaderName));
		CHECK(s.surface_type == SurfaceShaderType::Opaque);
		REQUIRE(s.textures.size() == 1);
		CHECK(s.textures[0].name == "levels/test/textures/wall_d");
		CHECK(s.textures[0].vertex_usage == kUsageTexCoord0);
		CHECK(near(s.textures[0].factor, 0.5f));
		REQUIRE(s.texture_parameters.size() == 1);
		CHECK(s.parameter("NORMAL") != nullptr);
		CHECK(s.parameter("NORMAL")->vertex_usage == kUsageTexCoord1);
		REQUIRE(s.sampled_textures.size() == 1);
		CHECK(s.sampled_textures[0] == "levels/test/textures/wall_grime_mask");
		REQUIRE(s.value("tint") != nullptr);
		CHECK(near(s.value("tint")->value[2], 0.75f));
		CHECK(s.double_sided());
		CHECK(work == 1 + 1 + 1);

		ShaderLookup lookup((*db)[0]);
		CHECK(lookup.by_name("Levels/Test/Shaders/WallShader") != nullptr);
		CHECK(lookup.by_name("levels/test/shaders/other") == nullptr);
	}

	TEST_CASE("render.look.unit: shader database truncations and mutations fail cleanly") {
		const std::vector<uint8_t> res = synthetic_database(3, 4);
		REQUIRE(parse_shader_database(res).ok());
		// Every proper prefix cuts the one render path short (its size field promises more): always an error.
		size_t truncation_errors = 0;
		for (size_t n = 0; n < res.size(); ++n) {
			std::vector<uint8_t> cut(res.begin(), res.begin() + std::ptrdiff_t(n));
			truncation_errors += parse_shader_database(cut).ok() ? 0 : 1;
		}
		CHECK(truncation_errors == res.size());
		// Single-byte mutations: each byte set to 0x00, 0x7F, 0x80 and 0xFF; a parse or an error, never a crash.
		size_t parsed = 0, failed = 0;
		for (size_t at = 0; at < res.size(); ++at) {
			for (uint8_t v : { uint8_t(0x00), uint8_t(0x7F), uint8_t(0x80), uint8_t(0xFF) }) {
				std::vector<uint8_t> m = res;
				m[at] = v;
				uint64_t work = 0;
				Result<std::vector<ShaderDatabase>> r = parse_shader_database(m, &work);
				(r.ok() ? parsed : failed) += 1;
				CHECK(work <= kShaderWorkBase + kShaderWorkPerByte * m.size());
			}
		}
		std::printf("render.look.unit shader database mutations: %zu bytes x 4 values, %zu parsed, %zu errors\n", res.size(), parsed, failed);
		CHECK(failed > 0);
		// Huge counts: every count field at 0xFFFFFFFF fails without reserving (would be > 100 GiB otherwise).
		std::vector<uint8_t> huge = res;
		std::memset(huge.data(), 0xFF, 4);
		CHECK(!parse_shader_database(huge).ok());
		CHECK(!parse_shader_database({}).ok());
	}

	TEST_CASE("render.look.unit: shader database work budget") {
		// 20,000 uses of one solution whose constant block binds 255 textures: 20,000 x 257 units against a budget of
		// 2^20 + 4 x ~80 KB; the parse must stop with the budget error, not collect.
		const std::vector<uint8_t> res = synthetic_database(255, 20000);
		uint64_t work = 0;
		Result<std::vector<ShaderDatabase>> r = parse_shader_database(res, &work);
		REQUIRE(!r.ok());
		CHECK(r.error().message.find("work budget") != std::string::npos);
		CHECK(work <= kShaderWorkBase + kShaderWorkPerByte * res.size() + 512);
		// The same block used a few times is fine.
		CHECK(parse_shader_database(synthetic_database(255, 3)).ok());
	}

	TEST_CASE("render.look.unit: texture name roles and placeholders") {
		REQUIRE(suffix_role("levels/a/wall_d"));
		CHECK(suffix_role("levels/a/wall_d")->role == 0);
		CHECK(suffix_role("levels/a/wall_d")->quality == 0);
		CHECK(suffix_role("levels/a/wall_n")->role == 1);
		CHECK(suffix_role("levels/a/wall_nm01")->role == 1);
		CHECK(suffix_role("levels/a/wall_m")->quality == 1);
		CHECK(suffix_role("levels/a/wall_s")->role == 2);
		CHECK(!suffix_role("levels/a/wall"));
		REQUIRE(graph_role("levels/a/water_d_01"));
		CHECK(graph_role("levels/a/water_d_01")->role == 0);
		REQUIRE(graph_role("levels/a/concrete"));
		CHECK(graph_role("levels/a/concrete")->quality == 2);
		CHECK(!graph_role("levels/a/wall_dirtmask"));
		CHECK(placeholder_texture("levels/a/defaultnormal"));
		CHECK(placeholder_texture("levels/a/black"));
		CHECK(!placeholder_texture("levels/a/wall_d"));
	}

	TEST_CASE("render.look.unit: resolver tiers, normal pairing, glass and undrawn shaders") {
		using F = res::TextureFormat;
		ShaderDatabase db;
		db.shaders.push_back(graph_shader("levels/t/facade", SurfaceShaderType::Opaque,
				{ { "levels/t/facade_d", 1, kUsageTexCoord1, 0.5f }, { "levels/t/facade_n", 1, kUsageTexCoord1, 0.5f }, { "levels/t/tile_n", 1, kUsageTexCoord0, 0.25f } }));
		db.shaders.push_back(graph_shader("levels/t/uvsplit", SurfaceShaderType::Opaque,
				{ { "levels/t/main_d", 1, kUsageTexCoord1, 1.0f }, { "levels/t/tile_n", 1, kUsageTexCoord0, 0.25f } }));
		db.shaders.push_back(graph_shader("levels/t/window_glass", SurfaceShaderType::Transparent, { { "levels/t/window_d", 1, kUsageTexCoord0, 1.0f } }));
		db.shaders.push_back(graph_shader("levels/t/cutout", SurfaceShaderType::OpaqueAlphaTest, { { "levels/t/leaf_d", 1, kUsageTexCoord0, 1.0f } }));
		std::map<std::string, LookResolver::HeaderInfo> headers;
		headers["levels/t/facade_d"] = { res::TextureKind::Texture2D, F::Dxt1 };
		headers["levels/t/facade_n"] = { res::TextureKind::Texture2D, F::NormalDxn };
		headers["levels/t/tile_n"] = { res::TextureKind::Texture2D, F::NormalDxn };
		headers["levels/t/main_d"] = { res::TextureKind::Texture2D, F::Dxt1 };
		headers["levels/t/window_d"] = { res::TextureKind::Texture2D, F::Dxt1 };
		headers["levels/t/leaf_d"] = { res::TextureKind::Texture2D, F::Dxt5 };
		headers["levels/t/param_d"] = { res::TextureKind::Texture2D, F::Dxt1 };
		LookResolver resolver(ShaderLookup(std::move(db)), headers);

		// No texture parameters: the graph constants give colour and the paired normal (same UV1 and tiling).
		MaterialSlot slot;
		slot.shader = "Levels/T/Facade";
		LookMaterial m = resolver.resolve(slot, "levels/t/mesh");
		CHECK(m.shader_in_database);
		REQUIRE(m.color);
		CHECK(m.color->name == "levels/t/facade_d");
		CHECK(m.color->tier == LookTier::Graph);
		CHECK(m.color->channel == kLookUv1);
		CHECK(near(m.color->scale, 2.0f));
		REQUIRE(m.normal);
		CHECK(m.normal->name == "levels/t/facade_n");
		CHECK(m.blend == LookBlend::Opaque);

		// A slot parameter outranks the graph.
		slot.textures = { { "Diffuse", "levels/t/param_d" } };
		m = resolver.resolve(slot, "levels/t/mesh");
		REQUIRE(m.color);
		CHECK(m.color->name == "levels/t/param_d");
		CHECK(m.color->tier == LookTier::Parameter);

		// No normal on the colour's UV set: the best on a mesh UV set (tile normal on UV0, its own tiling).
		MaterialSlot split;
		split.shader = "levels/t/uvsplit";
		m = resolver.resolve(split, "levels/t/mesh");
		REQUIRE(m.color);
		CHECK(m.color->channel == kLookUv1);
		REQUIRE(m.normal);
		CHECK(m.normal->name == "levels/t/tile_n");
		CHECK(m.normal->channel == kLookUv0);
		CHECK(near(m.normal->scale, 4.0f));

		// Glass: transparent, colour without alpha -> kept as the tint, drawn blended.
		MaterialSlot glass;
		glass.shader = "levels/t/window_glass";
		m = resolver.resolve(glass, "levels/t/mesh");
		CHECK(m.glass);
		CHECK(m.blend == LookBlend::Blend);
		REQUIRE(m.color);
		CHECK(m.color->name == "levels/t/window_d");

		// Alpha test from the database's surface type.
		MaterialSlot cut;
		cut.shader = "levels/t/cutout";
		m = resolver.resolve(cut, "levels/t/mesh");
		CHECK(m.blend == LookBlend::AlphaTest);
		CHECK(!m.glass);

		// Undrawn shader words.
		MaterialSlot hidden;
		hidden.shader = "levels/t/invisible_collision";
		CHECK(!resolver.resolve(hidden, "levels/t/mesh").drawn);

		// A shader the database lacks, with no parameters and no mount: nothing to draw from, no crash.
		MaterialSlot unknown;
		unknown.shader = "levels/t/unknown";
		m = resolver.resolve(unknown, "levels/t/mesh");
		CHECK(!m.shader_in_database);
		CHECK(!m.color);
	}

	TEST_CASE("render.look.unit: look_cubic and fog_fit") {
		CHECK(near(look_cubic({ 1.0f, 2.0f, 3.0f, 4.0f }, 0.5f), 0.125f + 0.5f + 1.5f + 4.0f));
		float density = -1.0f, curve = -1.0f, end = -1.0f;
		// A linear fog ramp: the fit's mean error over the samples stays small.
		const LookCubic linear{ 0.0f, 0.0f, 1.0f, 0.0f };
		fog_fit(linear, 1.0f, density, curve, end);
		CHECK(density > 0.5f);
		CHECK(density <= 1.0f);
		CHECK(curve >= 0.05f);
		CHECK(curve <= 16.0f);
		CHECK(end > 0.0f);
		CHECK(end <= 1.0f);
		double error = 0.0;
		for (int i = 0; i <= 64; ++i) {
			const float t = float(i) / 64.0f;
			const float x = std::clamp(t / end, 0.0f, 1.0f);
			const double got = double(density) * std::pow(double(x) * x * (3.0 - 2.0 * x), double(curve));
			error += std::fabs(got - double(t));
		}
		error /= 65.0;
		std::printf("render.look.unit fog_fit linear: density %.3f curve %.3f end %.3f, mean error %.4f\n", double(density), double(curve), double(end), error);
		// Least squares of density x smoothstep^curve can not follow a straight ramp at both ends (measured 0.054).
		CHECK(error < 0.06);
		// No fog, or a hostile curve: density 0, defaults kept.
		fog_fit(LookCubic{ 0.0f, 0.0f, 0.0f, 0.0f }, 1.0f, density, curve, end);
		CHECK(density == 0.0f);
		const float nan = std::numeric_limits<float>::quiet_NaN();
		fog_fit(LookCubic{ nan, nan, nan, nan }, nan, density, curve, end);
		CHECK(density == 0.0f);
		CHECK(curve == 1.0f);
		fog_fit(LookCubic{ 1e30f, -1e30f, 1e30f, 0.0f }, 1e30f, density, curve, end);
		CHECK(std::isfinite(density));
		CHECK(std::isfinite(curve));
	}

	TEST_CASE("render.look.unit: area weights") {
		LookArea box;
		box.box = true;
		box.box_axes = { LookVec3{ 1, 0, 0 }, LookVec3{ 0, 1, 0 }, LookVec3{ 0, 0, 1 } };
		box.box_center = { 10, 0, 0 };
		box.box_half = { 1, 1, 1 };
		box.fade = 2.0f;
		CHECK(look_area_weight(box, { 10, 0, 0 }) == 1.0f);
		CHECK(near(look_area_weight(box, { 12, 0, 0 }), 0.5f));
		CHECK(look_area_weight(box, { 14, 0, 0 }) == 0.0f);
		const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
		CHECK(look_area_weight(box, { nan, 0, 0 }) == 0.0f);
		CHECK(look_area_weight(box, { inf, 0, 0 }) == 0.0f);
		box.fade = 0.0f;
		CHECK(look_area_weight(box, { 11.5f, 0, 0 }) == 0.0f);

		LookArea poly;
		poly.outline = { { 0, 0 }, { 10, 0 }, { 10, 10 }, { 0, 10 } };
		poly.floor = 0.0f;
		poly.height = 5.0f;
		poly.fade = 4.0f;
		CHECK(look_area_weight(poly, { 5, 2, 5 }) == 1.0f);
		CHECK(near(look_area_weight(poly, { 5, 7, 5 }), 0.5f));
		CHECK(near(look_area_weight(poly, { 12, 2, 5 }), 0.5f));
		CHECK(look_area_weight(poly, { 30, 2, 5 }) == 0.0f);
		CHECK(look_area_weight(poly, { nan, nan, nan }) == 0.0f);
		poly.outline.resize(2);
		CHECK(look_area_weight(poly, { 5, 2, 5 }) == 0.0f);
		poly.outline.clear();
		CHECK(look_area_weight(poly, { 5, 2, 5 }) == 0.0f);
	}

	TEST_CASE("render.look.unit: look_frame base and area blend") {
		LookEnvironment env;
		env.base.light = true;
		env.base.sun_color = { 4.0f, 3.0f, 2.0f };
		env.base.sun_rotation_x = 0.0f;
		env.base.sun_rotation_y = 90.0f;
		env.base.enlighten = true;
		env.base.sky_color = { 0.1f, 0.2f, 0.3f };
		env.base.ground_color = { 0.1f, 0.1f, 0.1f };
		env.base.bounce_scale = 1.0f;
		env.base.tonemap = true;
		env.base.middle_gray = 0.2f;
		env.base.min_exposure = 0.5f;
		env.base.max_exposure = 4.0f;
		LookArea area;
		area.box = true;
		area.box_axes = { LookVec3{ 1, 0, 0 }, LookVec3{ 0, 1, 0 }, LookVec3{ 0, 0, 1 } };
		area.box_half = { 1, 1, 1 };
		area.fade = 2.0f;
		area.state.light = true;
		area.state.sun_color = { 8.0f, 6.0f, 4.0f };
		area.state.sun_rotation_x = 0.0f;
		area.state.sun_rotation_y = 90.0f;
		area.state.correction = true;
		area.state.lut = 0;
		env.areas.push_back(area);

		const LookFrame out = look_frame(env, { 100, 0, 0 });
		CHECK(near(out.sun_energy, 4.0f));
		CHECK(near(out.sun_color[0], 1.0f));
		CHECK(near(out.sun_color[1], 0.75f));
		CHECK(near(out.sun_direction[1], -1.0f));
		// Bounce: SunColor x sin(90) x kBounceAlbedo x BounceScale; up gets a quarter of it over the sky colour.
		CHECK(near(out.ambient_up[0], 0.1f + 0.25f * 4.0f * kBounceAlbedo));
		CHECK(near(out.ambient_down[2], 0.1f + 2.0f * kBounceAlbedo));
		CHECK(out.exposure >= 0.5f);
		CHECK(out.exposure <= 4.0f);
		CHECK(out.lut == -1);
		const LookFrame in = look_frame(env, { 0, 0, 0 });
		CHECK(near(in.sun_energy, 8.0f));
		CHECK(in.lut == 0);
		const LookFrame half = look_frame(env, { 2, 0, 0 });
		CHECK(near(half.sun_energy, 6.0f));
		CHECK(half.lut == 0); // the dominant state's LUT from weight 0.5
		CHECK(!out.fog);

		// A dark sun and hostile numbers stay finite.
		LookEnvState dark;
		dark.sun_color = { 0, 0, 0 };
		const LookFrame d = look_frame_of(dark);
		CHECK(d.sun_energy == 0.0f);
		CHECK(std::isfinite(d.exposure));
	}

	TEST_CASE("render.look.unit: look_grade") {
		LookEnvState identity;
		const LookVec3 c{ 0.2f, 0.5f, 0.9f };
		const LookVec3 plain = look_grade(identity, {}, c);
		CHECK(near(plain[0], 0.2f));
		CHECK(near(plain[2], 0.9f));
		// An identity cube (red fastest, corner-aligned) returns the input to within its 8-bit steps.
		const size_t n = kGradeLutSide;
		std::vector<uint8_t> cube(n * n * n * 4);
		for (size_t b = 0; b < n; ++b) {
			for (size_t g = 0; g < n; ++g) {
				for (size_t r = 0; r < n; ++r) {
					uint8_t *p = cube.data() + ((b * n + g) * n + r) * 4;
					p[0] = uint8_t(std::lround(255.0 * double(r) / double(n - 1)));
					p[1] = uint8_t(std::lround(255.0 * double(g) / double(n - 1)));
					p[2] = uint8_t(std::lround(255.0 * double(b) / double(n - 1)));
					p[3] = 255;
				}
			}
		}
		const LookVec3 graded = look_grade(identity, cube, c);
		for (size_t k = 0; k < 3; ++k) {
			CHECK(near(graded[k], c[k], 1.0f / 255.0f));
		}
		// Contrast 0 -> mid grey; saturation 0 -> Rec.601 luma.
		LookEnvState flat;
		flat.contrast = { 0, 0, 0 };
		CHECK(near(look_grade(flat, {}, c)[1], 0.5f));
		LookEnvState grey;
		grey.saturation = { 0, 0, 0 };
		const float luma = 0.2f * 0.299f + 0.5f * 0.587f + 0.9f * 0.114f;
		CHECK(near(look_grade(grey, {}, c)[0], luma));
		// A cube of the wrong size is ignored.
		CHECK(near(look_grade(identity, std::vector<uint8_t>(17, 0), c)[1], 0.5f));
	}
}

TEST_SUITE("render-look") {
	TEST_CASE("render.look: Metro shader database, resolver and visual environment") {
		InstallReader reader;
		if (!reader.locate({})) {
			bf3::test::skip_install_case(reader.describe().c_str());
			return;
		}
		const InstallSnapshot before = reader.snapshot();
		Result<std::shared_ptr<Install>> install = Install::open(reader);
		REQUIRE(install.ok());
		Result<std::unique_ptr<level::LoadedLevel>> loaded = level::load_level(*install, "MP_Subway", "TeamDeathMatch0");
		REQUIRE(loaded.ok());
		const level::LoadedLevel &lvl = **loaded;

		using Clock = std::chrono::steady_clock;
		Clock::time_point t0 = Clock::now();
		Result<res::RawRes> raw = res::read_raw_res(lvl.mount(), "levels/mp_subway/mp_subway/shaderdb");
		REQUIRE(raw.ok());
		CHECK(raw->res_type == kShaderDatabaseResType);
		uint64_t work = 0;
		Result<std::vector<ShaderDatabase>> db = parse_shader_database(raw->bytes(), &work);
		REQUIRE_MESSAGE(db.ok(), (db.ok() ? std::string() : db.error().message));
		const double seconds = std::chrono::duration<double>(Clock::now() - t0).count();
		size_t shaders = 0, textures = 0, sampled = 0;
		for (const ShaderDatabase &d : *db) {
			shaders += d.shaders.size();
			for (const SurfaceShader &s : d.shaders) {
				textures += s.textures.size();
				sampled += s.sampled_textures.size();
			}
		}
		const uint64_t limit = kShaderWorkBase + kShaderWorkPerByte * raw->bytes().size();
		std::printf("render.look shaderdb: %zu bytes, %zu render paths, %zu shaders, %zu graph textures, %zu sampled textures; work %llu units of a "
					"budget of %llu (%.1f %%); %.3f s\n",
				raw->bytes().size(), db->size(), shaders, textures, sampled, (unsigned long long)work, (unsigned long long)limit,
				100.0 * double(work) / double(limit), seconds);
		CHECK(!db->empty());
		CHECK(shaders > 0);
		CHECK(work < limit);

		std::unique_ptr<LookResolver> resolver = LookResolver::open(lvl.mount());
		REQUIRE(resolver != nullptr);
		const LookStats stats = resolver->stats();
		std::printf("render.look resolver: %zu shader databases, %zu shaders, work %llu, %zu failures\n", stats.shader_databases, stats.shaders,
				(unsigned long long)stats.shader_work, resolver->failures().size());
		CHECK(stats.shader_databases >= 1);
		CHECK(resolver->failures().empty());

		Result<std::unique_ptr<level::World>> world = level::World::traverse(lvl);
		REQUIRE(world.ok());
		Result<LookEnvironment> env = read_look_environment(lvl.db(), **world);
		REQUIRE(env.ok());
		decode_look_textures(lvl.mount(), *env);
		std::printf("render.look environment: from_data %d, base %s, %zu areas, %zu LUTs, horizon decoded %d\n", int(env->from_data),
				env->base.source.c_str(), env->areas.size(), env->luts.size(), int(env->base.horizon_decoded));
		for (const LookArea &a : env->areas) {
			std::printf("render.look area %s: priority %lld, %s, fade %.1f\n", a.name.c_str(), (long long)a.priority, a.box ? "box" : "polygon", double(a.fade));
		}
		for (const std::string &note : env->notes) {
			std::printf("render.look note: %s\n", note.c_str());
		}
		CHECK(env->from_data);
		CHECK(env->base.light);
		CHECK(env->areas.size() == 3);
		CHECK(env->luts.size() >= 1);
		const SnapshotDiff diff = diff_snapshots(before, reader.snapshot());
		std::printf("install.unchanged %s\n", diff.clean() ? "PASS" : diff.summary().c_str());
		CHECK_MESSAGE(diff.clean(), diff.summary());
	}
}
