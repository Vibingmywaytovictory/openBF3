// UI ui2 minimap readers on synthetic data (suite "ui-minimap-unit", ctest ui.minimap.unit; docs/design/
// ui-minimap-icons.md §13): l8_mip_chain sizes and box filter, combat_area_union / combat_area_min_field, the mask IoUs
// (identity 1, mirrored 0, refused past the caps), decode_l8_texture's refusals on synthetic DxTexture headers. No data.
#include <doctest/doctest.h>

#include "bf3/core/res/dx_texture.h"
#include "bf3/core/ui/minimap.h"

#include <cmath>
#include <vector>

namespace ui = bf3::core::ui;
namespace res = bf3::core::res;

namespace {

ui::L8Image make_image(uint32_t p_w, uint32_t p_h, uint8_t p_fill) {
	ui::L8Image img;
	img.width = p_w;
	img.height = p_h;
	img.texels.assign(size_t(p_w) * p_h, p_fill);
	return img;
}

res::DxTexture l8_header(uint16_t p_w, uint16_t p_h, uint8_t p_mips = 1) {
	res::DxTexture t;
	t.kind = res::TextureKind::Texture2D;
	t.format = res::TextureFormat::L8;
	t.width = p_w;
	t.height = p_h;
	t.depth = 1;
	t.slice_count = 1;
	t.mip_count = p_mips;
	for (uint8_t m = 0; m < p_mips; ++m) {
		uint32_t w = 0, h = 0, d = 0;
		t.mip_dimensions(m, w, h, d);
		t.mip_sizes.push_back(uint32_t(res::texture_image_bytes(t.format, w, h)));
		t.mip_chain_size += t.mip_sizes.back();
	}
	return t;
}

// A side x side mask whose texel (i, j) is inside when p_inside(world x, world z) of its centre under variant 0.
std::vector<bool> raster(uint32_t p_side, const ui::MinimapMapping &p_m, bool (*p_inside)(float, float)) {
	std::vector<bool> out(size_t(p_side) * p_side);
	for (uint32_t j = 0; j < p_side; ++j) {
		for (uint32_t i = 0; i < p_side; ++i) {
			const ui::MinimapUv uv{ (float(i) + 0.5f) / float(p_side), (float(j) + 0.5f) / float(p_side) };
			const std::array<float, 2> w = ui::minimap_uv_to_world(p_m, uv);
			out[size_t(j) * p_side + i] = p_inside(w[0], w[1]);
		}
	}
	return out;
}

constexpr ui::MinimapMapping kMap{ 0.0f, 0.0f, 128.0f };

bool in_corner_box(float p_x, float p_z) {
	return p_x >= 10.0f && p_x <= 40.0f && p_z >= -60.0f && p_z <= -50.0f;
}

} // namespace

TEST_SUITE("ui-minimap-unit") {
	TEST_CASE("l8_mip_chain: sizes down to 1x1 and a 2x2 box filter") {
		ui::L8Image img = make_image(4, 2, 0);
		img.texels = { 0, 10, 20, 30, 40, 50, 60, 70 };
		const std::vector<ui::L8Image> chain = ui::l8_mip_chain(img);
		REQUIRE(chain.size() == 3);
		CHECK(chain[1].width == 2);
		CHECK(chain[1].height == 1);
		CHECK(chain[1].texels[0] == 25); // (0 + 10 + 40 + 50) / 4
		CHECK(chain[1].texels[1] == 45); // (20 + 30 + 60 + 70) / 4
		CHECK(chain[2].width == 1);
		CHECK(chain[2].height == 1);
		CHECK(chain[2].texels[0] == 35);
		const std::vector<ui::L8Image> big = ui::l8_mip_chain(make_image(2048, 2048, 7));
		CHECK(big.size() == 12);
		size_t bytes = 0;
		for (const ui::L8Image &m : big) {
			bytes += m.texels.size();
			CHECK(m.texels.front() == 7);
		}
		CHECK(bytes == 5592405); // 4^12 - 1 over 3: the 5.6 MB of design §5.6
		CHECK(ui::l8_mip_chain(ui::L8Image{}).empty());
		ui::L8Image short_buffer = make_image(4, 4, 1);
		short_buffer.texels.resize(3);
		CHECK(ui::l8_mip_chain(short_buffer).empty());
	}

	TEST_CASE("combat_area_union and combat_area_min_field") {
		ui::L8Image a = make_image(2, 2, 255);
		ui::L8Image b = make_image(2, 2, 255);
		a.texels[0] = 10; // inside a
		b.texels[3] = 100; // inside b (100 / 255 < 0.496)
		b.texels[2] = 127; // 127 / 255 = 0.498: outside
		const std::vector<ui::L8Image> masks = { a, ui::L8Image{}, b };
		bf3::core::Result<std::vector<bool>> u = ui::combat_area_union(masks, 0.496f);
		REQUIRE(u.ok());
		CHECK(*u == std::vector<bool>{ true, false, false, true });
		bf3::core::Result<ui::L8Image> m = ui::combat_area_min_field(masks);
		REQUIRE(m.ok());
		CHECK(m->texels == std::vector<uint8_t>{ 10, 255, 127, 100 });
		const std::vector<ui::L8Image> mixed = { a, make_image(4, 4, 0) };
		CHECK_FALSE(ui::combat_area_union(mixed, 0.496f).ok());
		CHECK_FALSE(ui::combat_area_min_field(mixed).ok());
		const std::vector<ui::L8Image> none = { ui::L8Image{} };
		CHECK_FALSE(ui::combat_area_union(none, 0.496f).ok());
		CHECK_FALSE(ui::combat_area_min_field(none).ok());
	}

	TEST_CASE("mask_box_iou / mask_polygon_iou: identity 1, the mirrored variants 0, refused past the caps") {
		const uint32_t side = 128;
		const std::vector<bool> mask = raster(side, kMap, in_corner_box);
		const std::array<float, 4> box{ 10.0f, 40.0f, -60.0f, -50.0f };
		CHECK(ui::mask_box_iou(mask, side, kMap, 0, box) == doctest::Approx(1.0));
		const std::vector<std::array<float, 2>> poly = { { 10.0f, -60.0f }, { 40.0f, -60.0f }, { 40.0f, -50.0f }, { 10.0f, -50.0f } };
		CHECK(ui::mask_polygon_iou(mask, side, kMap, 0, poly) == doctest::Approx(1.0));
		for (uint8_t v = 1; v < ui::kMinimapMappingVariants; ++v) {
			// The box is off both diagonals and its x and z ranges are disjoint, so every sign flip or transpose moves it off
			// itself.
			CHECK(ui::mask_box_iou(mask, side, kMap, v, box) == doctest::Approx(0.0));
			CHECK(ui::mask_polygon_iou(mask, side, kMap, v, poly) == doctest::Approx(0.0));
		}
		CHECK(std::isnan(ui::mask_box_iou(mask, side + 1, kMap, 0, box))); // size mismatch
		const std::vector<bool> huge(size_t(ui::kMaxIouSide + 1) * (ui::kMaxIouSide + 1), false);
		CHECK(std::isnan(ui::mask_box_iou(huge, ui::kMaxIouSide + 1, kMap, 0, box)));
		std::vector<std::array<float, 2>> many(ui::kMaxAreaPolygonPoints + 1, { 0.0f, 0.0f });
		CHECK(std::isnan(ui::mask_polygon_iou(mask, side, kMap, 0, many)));
		const std::vector<std::array<float, 2>> two = { { 0.0f, 0.0f }, { 1.0f, 1.0f } };
		CHECK(std::isnan(ui::mask_polygon_iou(mask, side, kMap, 0, two)));
		const std::vector<std::array<float, 2>> nan_poly = { { 0.0f, 0.0f }, { std::nanf(""), 1.0f }, { 1.0f, 0.0f } };
		CHECK(std::isnan(ui::mask_polygon_iou(mask, side, kMap, 0, nan_poly)));
		CHECK(std::isnan(ui::mask_box_iou(mask, side, ui::MinimapMapping{ 0.0f, 0.0f, 0.0f }, 0, box)));
	}

	TEST_CASE("decode_l8_texture: a good header decodes; wrong format, cube, side 8192 and a short payload are refused") {
		res::DxTexture t = l8_header(8, 4, 2);
		std::vector<uint8_t> chunk(t.mip_chain_size);
		for (size_t i = 0; i < chunk.size(); ++i) {
			chunk[i] = uint8_t(i);
		}
		bf3::core::Result<ui::L8Image> ok = ui::decode_l8_texture(t, chunk, "synthetic");
		REQUIRE(ok.ok());
		CHECK(ok->width == 8);
		CHECK(ok->height == 4);
		CHECK(ok->texels[9] == 9);
		res::DxTexture wrong = t;
		wrong.format = res::TextureFormat::Dxt1;
		CHECK_FALSE(ui::decode_l8_texture(wrong, chunk, "synthetic").ok());
		res::DxTexture cube = t;
		cube.kind = res::TextureKind::Cube;
		CHECK_FALSE(ui::decode_l8_texture(cube, chunk, "synthetic").ok());
		res::DxTexture big = l8_header(8192, 8192, 1);
		std::vector<uint8_t> big_chunk(16);
		CHECK_FALSE(ui::decode_l8_texture(big, big_chunk, "synthetic").ok());
		const std::vector<uint8_t> short_chunk(chunk.begin(), chunk.begin() + 20);
		CHECK_FALSE(ui::decode_l8_texture(t, short_chunk, "synthetic").ok());
		CHECK_FALSE(ui::decode_l8_texture(t, std::vector<uint8_t>{}, "synthetic").ok());
		res::DxTexture zero = t;
		zero.width = 0;
		CHECK_FALSE(ui::decode_l8_texture(zero, chunk, "synthetic").ok());
	}

	TEST_CASE("at_uv and sdf_inside on a decoded mask") {
		ui::L8Image img = make_image(2, 2, 200);
		img.texels[1] = 3;
		CHECK(img.at_uv({ 0.75f, 0.25f }) == 3);
		CHECK(img.at_uv({ 5.0f, -1.0f }) == 3); // clamped to the top-right texel
		CHECK(img.at_uv({ std::nanf(""), 0.0f }) == 255);
		CHECK(ui::sdf_inside(img.at_uv({ 0.75f, 0.25f }), 0.496f));
		CHECK_FALSE(ui::sdf_inside(img.at_uv({ 0.25f, 0.25f }), 0.496f));
	}
}
