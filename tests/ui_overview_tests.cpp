#include <doctest/doctest.h>

#include "bf3/core/install_snapshot.h"
#include "bf3/core/level/level.h"
#include "bf3/core/level/world.h"
#include "bf3/core/ui/minimap.h"
#include "bf3/core/util/format.h"
#include "test_support.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace bf3::core;
using namespace bf3::core::ui;

namespace {

L8Image field_with_inside(uint32_t p_side, uint32_t p_u0, uint32_t p_v0, uint32_t p_u1, uint32_t p_v1) {
	L8Image img;
	img.width = p_side;
	img.height = p_side;
	img.texels.assign(size_t(p_side) * p_side, 255);
	for (uint32_t v = p_v0; v < p_v1; ++v) {
		for (uint32_t u = p_u0; u < p_u1; ++u) {
			img.texels[size_t(v) * p_side + u] = 0;
		}
	}
	return img;
}

}

TEST_SUITE("ui-overview-unit") {
	TEST_CASE("combat_area_bounds: the world rect of the inside texels") {
		const MinimapMapping m{ 0.0f, 0.0f, 100.0f };
		const std::optional<MinimapWorldRect> r = combat_area_bounds(field_with_inside(10, 2, 3, 6, 8), 0.5f, m);
		REQUIRE(r.has_value());
		CHECK(r->x0 == doctest::Approx(-10.0f));
		CHECK(r->x1 == doctest::Approx(30.0f));
		CHECK(r->z0 == doctest::Approx(-30.0f));
		CHECK(r->z1 == doctest::Approx(20.0f));
		CHECK_FALSE(combat_area_bounds(field_with_inside(10, 0, 0, 0, 0), 0.5f, m).has_value());
		CHECK_FALSE(combat_area_bounds(L8Image{}, 0.5f, m).has_value());
		CHECK_FALSE(combat_area_bounds(field_with_inside(10, 2, 3, 6, 8), 0.5f, MinimapMapping{ 0.0f, 0.0f, 0.0f }).has_value());
	}

	TEST_CASE("minimap_overview_view: north up, centred on the area, the long side times the margin across the short box side") {
		const MinimapWorldRect area{ -10.0f, -30.0f, 30.0f, 20.0f };
		const MinimapView v = minimap_overview_view(area, 1.25f, 100.0f, 50.0f, 400.0f, 300.0f);
		CHECK_FALSE(v.rotate);
		CHECK(v.heading == 0.0f);
		CHECK(v.player_x == doctest::Approx(10.0f));
		CHECK(v.player_z == doctest::Approx(-5.0f));
		CHECK(v.center_x == doctest::Approx(300.0f));
		CHECK(v.center_y == doctest::Approx(200.0f));
		CHECK(v.pixels_per_metre == doctest::Approx(300.0f / (50.0f * 1.25f)));
		const ScreenPoint c = minimap_project(v, 10.0f, -5.0f);
		CHECK(c.x == doctest::Approx(300.0f));
		CHECK(c.y == doctest::Approx(200.0f));
		CHECK(minimap_overview_view(MinimapWorldRect{}, 1.0f, 0.0f, 0.0f, 10.0f, 10.0f).pixels_per_metre == 0.0f);
	}
}

TEST_SUITE("ui-overview") {
	TEST_CASE("overview: the Metro TDM combat area bounds against the gameplay area polygon") {
		bf3::test::Mounted *mount = bf3::test::shared_mount_or_skip();
		if (mount == nullptr) {
			return;
		}
		Result<std::unique_ptr<level::LoadedLevel>> loaded = level::load_level(mount->install, "MP_Subway", "TeamDeathMatch0");
		REQUIRE(loaded.ok());
		Result<std::unique_ptr<level::World>> world = level::World::traverse(**loaded);
		REQUIRE(world.ok());
		Result<MinimapLevel> ml = read_minimap_level(**loaded, **world);
		REQUIRE_MESSAGE(ml.ok(), (ml.ok() ? std::string() : ml.error().message));
		Result<std::vector<std::array<float, 2>>> poly = read_area_polygon(**loaded, **world, "levels/mp_subway/teamdeathmatch/layer0_teamdeathmatch_logic");
		REQUIRE_MESSAGE(poly.ok(), (poly.ok() ? std::string() : poly.error().message));
		Result<LevelMount> lm = mount_minimap_level(*mount->install, "MP_Subway");
		REQUIRE(lm.ok());
		Result<MinimapImages> images = load_minimap_images(*lm, *ml);
		REQUIRE(images.ok());
		Result<L8Image> field = combat_area_min_field(images->combat_areas);
		REQUIRE_MESSAGE(field.ok(), (field.ok() ? std::string() : field.error().message));
		const MinimapMapping mapping = minimap_mapping(ml->data);
		const std::optional<MinimapWorldRect> area = combat_area_bounds(*field, ml->data.combat_area_alpha_threshold, mapping);
		REQUIRE(area.has_value());
		MinimapWorldRect box{ 1e9f, 1e9f, -1e9f, -1e9f };
		for (const std::array<float, 2> &p : *poly) {
			box.x0 = std::min(box.x0, p[0]);
			box.x1 = std::max(box.x1, p[0]);
			box.z0 = std::min(box.z0, p[1]);
			box.z1 = std::max(box.z1, p[1]);
		}
		const float texel = mapping.size / float(field->width);
		std::printf("ui.overview metro bounds x %.1f..%.1f z %.1f..%.1f, polygon bbox x %.1f..%.1f z %.1f..%.1f, texel %.1f m\n", area->x0, area->x1, area->z0,
				area->z1, box.x0, box.x1, box.z0, box.z1, texel);
		CHECK(std::fabs(area->x0 - box.x0) <= 2.0f * texel);
		CHECK(std::fabs(area->x1 - box.x1) <= 2.0f * texel);
		CHECK(std::fabs(area->z0 - box.z0) <= 2.0f * texel);
		CHECK(std::fabs(area->z1 - box.z1) <= 2.0f * texel);
		const MinimapView menu = minimap_overview_view(*area, kMinimapOverviewMargin, 783.0f, 153.0f, 390.0f, 392.0f);
		const MinimapView deploy = minimap_overview_view(*area, kMinimapOverviewMargin, 769.0f, 132.0f, 414.0f, 415.0f);
		std::printf("ui.overview margin %.2f: menu box %.3f px/m (18.57 footage at 176.5 s 345 px over 200 m, 1.725), deploy box %.3f px/m (76.0 s 365 px, 1.825)\n",
				double(kMinimapOverviewMargin), double(menu.pixels_per_metre), double(deploy.pixels_per_metre));
		CHECK(menu.pixels_per_metre == doctest::Approx(1.725f).epsilon(0.02));
		CHECK(deploy.pixels_per_metre == doctest::Approx(1.825f).epsilon(0.02));
		const SnapshotDiff diff = diff_snapshots(mount->before, mount->reader.snapshot());
		std::printf("ui.overview install.unchanged %s\n", diff.clean() ? "yes" : diff.summary().c_str());
		CHECK(diff.clean());
	}
}
