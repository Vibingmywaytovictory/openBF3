#include <doctest/doctest.h>

#include "bf3/core/install_snapshot.h"
#include "bf3/core/level/level.h"
#include "bf3/core/level/world.h"
#include "bf3/core/ui/avm1.h"
#include "bf3/core/ui/hud_widgets.h"
#include "bf3/core/ui/minimap.h"
#include "bf3/core/ui/swf_layout.h"
#include "bf3/core/ui/swf_library.h"
#include "bf3/core/util/format.h"
#include "test_support.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
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

const SwfLayoutNode *find_symbol(const SwfLayoutNode &p_node, std::string_view p_symbol) {
	if (p_node.symbol == p_symbol) {
		return &p_node;
	}
	for (const SwfLayoutNode &c : p_node.children) {
		if (const SwfLayoutNode *hit = find_symbol(c, p_symbol)) {
			return hit;
		}
	}
	return nullptr;
}

void add_shape_boxes(const SwfLibrary &p_lib, const SwfLayoutNode &p_node, SwfRect &r_box, uint32_t &r_shapes) {
	if (p_node.kind == SwfLayoutKind::Shape && p_node.symbol.starts_with("#")) {
		Result<std::shared_ptr<const SwfMovie>> movie = p_lib.load(p_node.movie);
		const SwfShape *shape =
				movie.ok() ? (*movie)->find_shape(uint16_t(std::stoul(p_node.symbol.substr(1)))) : nullptr;
		if (shape != nullptr) {
			const SwfMatrix &m = p_node.stage;
			for (const float x : { shape->bounds.x_min, shape->bounds.x_max }) {
				for (const float y : { shape->bounds.y_min, shape->bounds.y_max }) {
					const float sx = m.a * x + m.c * y + m.tx;
					const float sy = m.b * x + m.d * y + m.ty;
					r_box.x_min = std::min(r_box.x_min, sx);
					r_box.x_max = std::max(r_box.x_max, sx);
					r_box.y_min = std::min(r_box.y_min, sy);
					r_box.y_max = std::max(r_box.y_max, sy);
				}
			}
			r_shapes++;
		}
	}
	for (const SwfLayoutNode &c : p_node.children) {
		add_shape_boxes(p_lib, c, r_box, r_shapes);
	}
}

struct OverviewClipBox {
	SwfRect box{ 1e9f, -1e9f, 1e9f, -1e9f };
	uint32_t shapes = 0;
};

OverviewClipBox overview_clip_box(const SwfLibrary &p_lib, std::string_view p_movie) {
	OverviewClipBox out;
	WorkBudget budget(kSwfLayoutBudgetUnits);
	Result<SwfLayoutNode> tree = read_layout(p_lib, p_movie, SwfLayoutOptions{}, budget);
	REQUIRE_MESSAGE(tree.ok(), (tree.ok() ? std::string() : tree.error().message));
	const SwfLayoutNode *clip = find_symbol(*tree, "OverviewMap");
	REQUIRE(clip != nullptr);
	add_shape_boxes(p_lib, *clip, out.box, out.shapes);
	return out;
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
		CHECK_FALSE(combat_area_bounds(field_with_inside(10, 2, 3, 6, 8), 0.5f, MinimapMapping{ 0.0f, 0.0f, 0.0f })
						.has_value());
	}

	TEST_CASE("minimap_overview_view: north up, centred on the area, the long side times the margin across the short "
			  "box side") {
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
	TEST_CASE("overview: Widget.Image.OverviewMap's onClipLoad sets its own rendererString to the HUD minimap's "
			  "renderer") {
		bf3::test::Mounted *mount = bf3::test::shared_mount_or_skip();
		if (mount == nullptr) {
			return;
		}
		Result<SwfLibrary> lib = SwfLibrary::open(mount->install);
		REQUIRE(lib.ok());
		Result<std::shared_ptr<const SwfMovie>> movie = lib->load("ui/assets/actionscriptlibrary");
		REQUIRE(movie.ok());
		Result<Avm1Movie> avm1 = read_avm1(**movie);
		REQUIRE(avm1.ok());
		const Avm1Class *k = avm1->find_class("Widget.Image.OverviewMap");
		REQUIRE(k != nullptr);
		const auto load = std::find_if(
				k->methods.begin(), k->methods.end(), [](const Avm1Member &m) { return m.name == "onClipLoad"; });
		REQUIRE(load != k->methods.end());
		const Avm1Code &code = avm1->bodies[k->body].code;
		const Avm1Action *fn = code.function_action(load->scope);
		REQUIRE(fn != nullptr);
		const uint16_t preloads = avm1_function_flag::kPreloadArguments | avm1_function_flag::kPreloadSuper |
				avm1_function_flag::kPreloadRoot | avm1_function_flag::kPreloadParent |
				avm1_function_flag::kPreloadGlobal;
		CHECK((fn->function_flags & avm1_function_flag::kPreloadThis) != 0);
		CHECK((fn->function_flags & preloads) == 0);
		std::vector<const Avm1Action *> body;
		for (const Avm1Action &a : code.actions) {
			if (a.scope == load->scope) {
				body.push_back(&a);
			}
		}
		const auto is_register = [](const Avm1Value &p_v, uint32_t p_reg) {
			return p_v.type == Avm1PushType::Register && p_v.index == p_reg;
		};
		uint32_t self_register = 0;
		uint32_t sets = 0;
		std::string renderer;
		for (size_t i = 0; i + 1 < body.size(); ++i) {
			const Avm1Action &a = *body[i];
			if (a.op == avm1_op::kPush && a.values.size() == 1 && is_register(a.values[0], 1) &&
					body[i + 1]->op == avm1_op::kStoreRegister) {
				self_register = body[i + 1]->reg;
			}
			if (a.op == avm1_op::kPush && a.values.size() == 3 && self_register != 0 &&
					is_register(a.values[0], self_register) && a.values[1].is_string() &&
					a.values[1].text == "rendererString" && a.values[2].is_string() &&
					body[i + 1]->op == avm1_op::kSetMember) {
				renderer = a.values[2].text;
				sets++;
			}
		}
		std::printf("ui.overview Widget.Image.OverviewMap.onClipLoad: this (r:1) stored in r:%u, %u rendererString "
					"sets, '%s'\n",
				self_register, sets, renderer.c_str());
		CHECK(self_register != 0);
		CHECK(sets == 1);
		CHECK(renderer == kMinimapRenderer);
	}

	TEST_CASE("overview: the Metro TDM combat area bounds against the gameplay area polygon") {
		bf3::test::Mounted *mount = bf3::test::shared_mount_or_skip();
		if (mount == nullptr) {
			return;
		}
		Result<std::unique_ptr<level::LoadedLevel>> loaded =
				level::load_level(mount->install, "MP_Subway", "TeamDeathMatch0");
		REQUIRE(loaded.ok());
		Result<std::unique_ptr<level::World>> world = level::World::traverse(**loaded);
		REQUIRE(world.ok());
		Result<MinimapLevel> ml = read_minimap_level(**loaded, **world);
		REQUIRE_MESSAGE(ml.ok(), (ml.ok() ? std::string() : ml.error().message));
		Result<std::vector<std::array<float, 2>>> poly =
				read_area_polygon(**loaded, **world, "levels/mp_subway/teamdeathmatch/layer0_teamdeathmatch_logic");
		REQUIRE_MESSAGE(poly.ok(), (poly.ok() ? std::string() : poly.error().message));
		Result<LevelMount> lm = mount_minimap_level(*mount->install, "MP_Subway");
		REQUIRE(lm.ok());
		Result<MinimapImages> images = load_minimap_images(*lm, *ml);
		REQUIRE(images.ok());
		Result<L8Image> field = combat_area_min_field(images->combat_areas);
		REQUIRE_MESSAGE(field.ok(), (field.ok() ? std::string() : field.error().message));
		const MinimapMapping mapping = minimap_mapping(ml->data);
		const std::optional<MinimapWorldRect> area =
				combat_area_bounds(*field, ml->data.combat_area_alpha_threshold, mapping);
		REQUIRE(area.has_value());
		MinimapWorldRect box{ 1e9f, 1e9f, -1e9f, -1e9f };
		for (const std::array<float, 2> &p : *poly) {
			box.x0 = std::min(box.x0, p[0]);
			box.x1 = std::max(box.x1, p[0]);
			box.z0 = std::min(box.z0, p[1]);
			box.z1 = std::max(box.z1, p[1]);
		}
		const float texel = mapping.size / float(field->width);
		std::printf("ui.overview metro bounds x %.1f..%.1f z %.1f..%.1f, polygon bbox x %.1f..%.1f z %.1f..%.1f, texel "
					"%.1f m\n",
				area->x0, area->x1, area->z0, area->z1, box.x0, box.x1, box.z0, box.z1, texel);
		CHECK(std::fabs(area->x0 - box.x0) <= 2.0f * texel);
		CHECK(std::fabs(area->x1 - box.x1) <= 2.0f * texel);
		CHECK(std::fabs(area->z0 - box.z0) <= 2.0f * texel);
		CHECK(std::fabs(area->z1 - box.z1) <= 2.0f * texel);
		Result<SwfLibrary> lib = SwfLibrary::open(mount->install);
		REQUIRE(lib.ok());
		const OverviewClipBox menu_clip = overview_clip_box(*lib, "ui/assets/ingamemenump");
		const OverviewClipBox deploy_clip = overview_clip_box(*lib, "ui/assets/spawnscreenpc");
		const SwfRect &mb = menu_clip.box;
		const SwfRect &db = deploy_clip.box;
		std::printf("ui.overview clip boxes over %u and %u shapes: ingamemenump %.1f..%.1f x %.1f..%.1f, spawnscreenpc "
					"%.1f..%.1f x %.1f..%.1f\n",
				menu_clip.shapes, deploy_clip.shapes, double(mb.x_min), double(mb.x_max), double(mb.y_min),
				double(mb.y_max), double(db.x_min), double(db.x_max), double(db.y_min), double(db.y_max));
		CHECK(menu_clip.shapes == 2);
		CHECK(deploy_clip.shapes == 2);
		CHECK(mb.x_min == doctest::Approx(778.0f).epsilon(0.0005));
		CHECK(mb.x_max == doctest::Approx(1179.0f).epsilon(0.0005));
		CHECK(mb.y_min == doctest::Approx(153.0f).epsilon(0.0005));
		CHECK(mb.y_max == doctest::Approx(554.0f).epsilon(0.0005));
		CHECK(db.x_min == doctest::Approx(769.0f).epsilon(0.0005));
		CHECK(db.x_max == doctest::Approx(1183.3f).epsilon(0.0005));
		CHECK(db.y_min == doctest::Approx(132.0f).epsilon(0.0005));
		CHECK(db.y_max == doctest::Approx(547.8f).epsilon(0.0005));
		const MinimapView menu =
				minimap_overview_view(*area, kMinimapOverviewMargin, mb.x_min, mb.y_min, mb.width(), mb.height());
		const MinimapView deploy =
				minimap_overview_view(*area, kMinimapOverviewMargin, db.x_min, db.y_min, db.width(), db.height());
		std::printf("ui.overview margin %.2f: ingamemenump box %.3f px/m (18.57 footage at 176.5 s 345 px over 200 m, "
					"1.725), spawnscreenpc box %.3f px/m (76.0 s 365 px, 1.825)\n",
				double(kMinimapOverviewMargin), double(menu.pixels_per_metre), double(deploy.pixels_per_metre));
		CHECK(menu.pixels_per_metre == doctest::Approx(1.725f).epsilon(0.02));
		CHECK(deploy.pixels_per_metre == doctest::Approx(1.825f).epsilon(0.02));
		const SnapshotDiff diff = diff_snapshots(mount->before, mount->reader.snapshot());
		std::printf("ui.overview install.unchanged %s\n", diff.clean() ? "yes" : diff.summary().c_str());
		CHECK(diff.clean());
	}
}
