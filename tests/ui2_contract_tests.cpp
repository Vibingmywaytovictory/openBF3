#include <doctest/doctest.h>

#include "bf3/core/ui/hud_icons.h"
#include "bf3/core/ui/hud_model.h"
#include "bf3/core/ui/hud_sources.h"
#include "bf3/core/ui/minimap.h"

#include <cmath>
#include <set>
#include <string>
#include <type_traits>
#include <utility>

namespace ui = bf3::core::ui;

namespace {

constexpr float kPi = 3.14159265358979f;

static_assert(std::size(ui::kHudBindings) == 515);
static_assert(ui::hud_binding_count(ui::HudBindKind::Model) == 275);
static_assert(ui::hud_binding_count(ui::HudBindKind::Client) == 88);
static_assert(ui::hud_binding_count(ui::HudBindKind::Unused) == 152);
static_assert(ui::hud_binding_count("UIWeaponCompData") == 63);
static_assert(ui::hud_binding_count("UISoldierCompData") == 16);
static_assert(ui::hud_binding_count("UICapturepointCompData") == 12);
static_assert(ui::hud_binding_count("UITicketCounterCompData") == 13);
static_assert(ui::hud_binding_count("UIKillCounterCompData") == 26);
static_assert(ui::hud_binding_count("UIMessageCompData") == 22);
static_assert(ui::hud_binding_count("UIMinimapCompData") == 8);
static_assert(ui::hud_binding_count("UISpawnPointCompData") == 6);
static_assert(ui::hud_binding_count("UISpawnLogicCompData") == 4);
static_assert(ui::hud_binding_count("UISquadCompData") == 30);
static_assert(ui::hud_binding_count("UITeamCompData") == 35);
static_assert(ui::hud_binding_count("UIKillCardCompData") == 27);
static_assert(ui::hud_binding_count("UIKitCompData") == 4);
static_assert(ui::hud_binding_count("UICustomizationCompData") == 59);
static_assert(ui::hud_binding_count("UIPreRoundCompData") == 3);
static_assert(ui::hud_binding_count("UIEndOfRoundCompData") == 52);
static_assert(ui::hud_binding_count("UISettingsCompData") == 89);
static_assert(ui::hud_binding_count("UILevelCompData") == 23);
static_assert(ui::hud_binding_count("UIIngameMenuCompData") == 1);
static_assert(ui::hud_binding_count("UIPopupCompData") == 7);
static_assert(ui::hud_binding_count("UserComponentData") == 15);
static_assert(std::is_trivially_copyable_v<ui::HudModel>);
static_assert(std::is_standard_layout_v<ui::HudModel>);

constexpr ui::MinimapMapping kMetro{ 0.0f, -162.0f, 1024.0f };

bool near(float p_a, float p_b, float p_eps) {
	return std::fabs(p_a - p_b) <= p_eps;
}

}

TEST_SUITE("ui2-contract") {
	TEST_CASE("binding rows: every bound component, no duplicate, Model fields are snake_case of the source") {
		std::set<std::string_view> components(std::begin(ui::kHudBoundComponents), std::end(ui::kHudBoundComponents));
		CHECK(components.size() == 21);
		std::set<std::pair<std::string_view, std::string_view>> seen;
		size_t sum = 0;
		for (std::string_view c : ui::kHudBoundComponents) {
			sum += ui::hud_binding_count(c);
		}
		CHECK(sum == std::size(ui::kHudBindings));
		for (const ui::HudBinding &b : ui::hud_bindings()) {
			const std::string label = std::string(b.component) + "." + std::string(b.source);
			INFO(label);
			CHECK(components.count(b.component) == 1);
			CHECK(seen.insert({ b.component, b.source }).second);
			CHECK(!b.source.empty());
			if (b.kind == ui::HudBindKind::Model) {
				char buf[128] = {};
				const size_t n = ui::hud_snake_case(b.source, buf, sizeof(buf));
				REQUIRE(n > 0);
				CHECK(std::string_view(buf, n) == b.field);
				CHECK(b.wave != ui::HudWave::None);
			} else {
				CHECK(b.field.empty());
			}
		}
	}

	TEST_CASE("hud_snake_case") {
		char buf[64] = {};
		const auto snake = [&](std::string_view p_s) { return std::string(buf, ui::hud_snake_case(p_s, buf, sizeof(buf))); };
		CHECK(snake("ActiveWeaponClipCapacity") == "active_weapon_clip_capacity");
		CHECK(snake("AATracking") == "aa_tracking");
		CHECK(snake("Squad1Name") == "squad1_name");
		CHECK(snake("X") == "x");
		CHECK(ui::hud_snake_case("ActiveWeaponAmmo", buf, 4) == 0);
	}

	TEST_CASE("minimap mapping: research formula, inverse, variants") {

		const ui::MinimapUv c = ui::world_to_minimap_uv(kMetro, 0.0f, -162.0f);
		CHECK(near(c.u, 0.5f, 1e-6f));
		CHECK(near(c.v, 0.5f, 1e-6f));
		const ui::MinimapUv px = ui::world_to_minimap_uv(kMetro, 100.0f, -162.0f);
		CHECK(px.u < 0.5f);
		const ui::MinimapUv pz = ui::world_to_minimap_uv(kMetro, 0.0f, -62.0f);
		CHECK(pz.v < 0.5f);

		const ui::MinimapUv hq = ui::world_to_minimap_uv(kMetro, -108.03f, -147.5f);
		CHECK(near(hq.u, 0.605498f, 1e-5f));
		CHECK(near(hq.v, 0.485840f, 1e-5f));
		const std::array<float, 2> back = ui::minimap_uv_to_world(kMetro, hq);
		CHECK(near(back[0], -108.03f, 1e-3f));
		CHECK(near(back[1], -147.5f, 1e-3f));

		std::set<std::pair<float, float>> distinct;
		for (uint8_t v = 0; v < ui::kMinimapMappingVariants; ++v) {
			const ui::MinimapUv a = ui::world_to_minimap_uv_variant(kMetro, -108.03f, -500.0f, v);
			distinct.insert({ a.u, a.v });
			if (v == 0) {
				const ui::MinimapUv r = ui::world_to_minimap_uv(kMetro, -108.03f, -500.0f);
				CHECK(a.u == r.u);
				CHECK(a.v == r.v);
			}
		}
		CHECK(distinct.size() == 8);
	}

	TEST_CASE("minimap view: north-up and rotating orientation, round trip") {
		ui::MinimapView view;
		view.center_x = 128.0f;
		view.center_y = 128.0f;
		view.pixels_per_metre = 2.0f;
		view.player_x = 10.0f;
		view.player_z = 20.0f;

		view.rotate = false;
		view.heading = kPi / 2.0f;
		const ui::ScreenPoint north = ui::minimap_project(view, 10.0f, 30.0f);
		CHECK(near(north.x, 128.0f, 1e-4f));
		CHECK(near(north.y, 108.0f, 1e-4f));
		const ui::ScreenPoint plus_x = ui::minimap_project(view, 20.0f, 20.0f);
		CHECK(near(plus_x.x, 108.0f, 1e-4f));
		CHECK(near(plus_x.y, 128.0f, 1e-4f));
		CHECK(near(ui::minimap_marker_angle(view), -kPi / 2.0f, 1e-6f));

		view.rotate = true;
		for (float heading : { 0.0f, kPi / 2.0f, kPi, -kPi / 3.0f, 2.5f }) {
			view.heading = heading;
			const float ahead_x = view.player_x + 10.0f * std::sin(heading);
			const float ahead_z = view.player_z + 10.0f * std::cos(heading);
			const ui::ScreenPoint p = ui::minimap_project(view, ahead_x, ahead_z);
			CHECK(near(p.x, 128.0f, 1e-3f));
			CHECK(near(p.y, 108.0f, 1e-3f));

			const float left_x = view.player_x + 10.0f * std::sin(heading + kPi / 2.0f);
			const float left_z = view.player_z + 10.0f * std::cos(heading + kPi / 2.0f);
			const ui::ScreenPoint l = ui::minimap_project(view, left_x, left_z);
			CHECK(near(l.x, 108.0f, 1e-3f));
			CHECK(near(l.y, 128.0f, 1e-3f));
			const std::array<float, 2> w = ui::minimap_unproject(view, p);
			CHECK(near(w[0], ahead_x, 1e-3f));
			CHECK(near(w[1], ahead_z, 1e-3f));
			CHECK(ui::minimap_marker_angle(view) == 0.0f);
		}
	}

	TEST_CASE("view agrees with the image mapping in north-up") {

		ui::MinimapView view;
		view.rotate = false;
		view.pixels_per_metre = 0.25f;
		view.player_x = -108.03f;
		view.player_z = -147.5f;
		const float image_px = kMetro.size * view.pixels_per_metre;
		const ui::MinimapUv a = ui::world_to_minimap_uv(kMetro, view.player_x, view.player_z);
		const ui::MinimapUv b = ui::world_to_minimap_uv(kMetro, -193.69f, -610.25f);
		const ui::ScreenPoint s = ui::minimap_project(view, -193.69f, -610.25f);
		CHECK(near(s.x, (b.u - a.u) * image_px, 1e-2f));
		CHECK(near(s.y, (b.v - a.v) * image_px, 1e-2f));
	}

	TEST_CASE("sdf_inside and L8Image::at_uv") {
		CHECK(ui::sdf_inside(0, ui::kResearchCombatAreaThreshold));
		CHECK(ui::sdf_inside(126, ui::kResearchCombatAreaThreshold));
		CHECK_FALSE(ui::sdf_inside(127, ui::kResearchCombatAreaThreshold));
		CHECK_FALSE(ui::sdf_inside(255, ui::kResearchCombatAreaThreshold));
		ui::L8Image img;
		img.width = 2;
		img.height = 2;
		img.texels = { 1, 2, 3, 4 };
		CHECK(img.at_uv({ 0.0f, 0.0f }) == 1);
		CHECK(img.at_uv({ 0.99f, 0.0f }) == 2);
		CHECK(img.at_uv({ 0.0f, 0.99f }) == 3);
		CHECK(img.at_uv({ 2.0f, 2.0f }) == 4);
		CHECK(img.at_uv({ -1.0f, -1.0f }) == 1);
		CHECK(ui::L8Image{}.at_uv({ 0.5f, 0.5f }) == 255);
		CHECK(img.at_uv({ std::nanf(""), 0.5f }) == 255);
		CHECK(img.at_uv({ 0.5f, INFINITY }) == 255);
	}

	TEST_CASE("debug UV packing: round trip within half a step, edges, hostile values") {
		for (float u : { 0.0f, 1.0f, 0.5f, 0.605498f, 0.123456f, 0.999f }) {
			for (float v : { 0.0f, 1.0f, 0.485840f, 0.25f, 0.0001f }) {
				const ui::MinimapUv d = ui::decode_minimap_debug_uv(ui::encode_minimap_debug_uv({ u, v }));
				CHECK(near(d.u, u, 0.5f / ui::kMinimapDebugUvSteps + 1e-6f));
				CHECK(near(d.v, v, 0.5f / ui::kMinimapDebugUvSteps + 1e-6f));

				CHECK(ui::minimap_uv_error_texels(d, { u, v }, 2048) <= 0.26f);
			}
		}
		const ui::MinimapDebugUvPixel top = ui::encode_minimap_debug_uv({ 1.0f, 1.0f });
		CHECK(top.r == 255);
		CHECK(top.g == 255);
		CHECK(top.b == 255);
		const ui::MinimapDebugUvPixel bad = ui::encode_minimap_debug_uv({ std::nanf(""), -3.0f });
		CHECK(bad.r == 0);
		CHECK(bad.g == 0);
		CHECK(bad.b == 0);
	}

	TEST_CASE("expected UV under a screen point: centre = the player's texel, mirrored variants miss") {
		ui::MinimapView view;
		view.center_x = 128.0f;
		view.center_y = 128.0f;
		view.pixels_per_metre = 0.85f;
		view.player_x = -108.03f;
		view.player_z = -147.5f;
		for (bool rotate : { false, true }) {
			view.rotate = rotate;
			view.heading = rotate ? kPi / 2.0f : 0.3f;
			const ui::MinimapUv c = ui::minimap_expected_uv(view, kMetro, { 128.0f, 128.0f });
			const ui::MinimapUv own = ui::world_to_minimap_uv(kMetro, view.player_x, view.player_z);
			CHECK(ui::minimap_uv_error_texels(c, own, 2048) < 1e-2f);

			for (uint8_t v = 1; v < ui::kMinimapMappingVariants; ++v) {
				INFO(int(v));
				float worst = 0.0f;
				for (int i = 0; i < 16; ++i) {
					const float a = 2.0f * kPi * float(i) / 16.0f;
					const ui::ScreenPoint ring{ 128.0f + 100.0f * std::cos(a), 128.0f + 100.0f * std::sin(a) };
					const ui::MinimapUv r0 = ui::minimap_expected_uv(view, kMetro, ring);
					const float e = ui::minimap_uv_error_texels(ui::minimap_expected_uv(view, kMetro, ring, v), r0, 2048);
					worst = e > worst ? e : worst;
				}
				CHECK(worst > ui::kMinimapUvMirrorTexels);
			}
		}

		view.rotate = true;
		view.heading = kPi / 2.0f;
		const std::array<float, 2> ahead = ui::minimap_unproject(view, { 128.0f, 128.0f - 85.0f });
		CHECK(near(ahead[0], view.player_x + 100.0f, 1e-2f));
		CHECK(near(ahead[1], view.player_z, 1e-2f));
	}

	TEST_CASE("icon choice relative to the local team") {
		using S = ui::HudIconState;
		CHECK(ui::objective_icon(false, 0, 1).icon == "UIHudIcon_NeutralFlag");
		CHECK(ui::objective_icon(false, 0, 1).state == S::Neutral);
		CHECK(ui::objective_icon(false, 1, 1).icon == "UIHudIcon_FriendlyFlag");
		CHECK(ui::objective_icon(false, 1, 1).state == S::Friendly);
		CHECK(ui::objective_icon(false, 2, 1).icon == "UIHudIcon_EnemyFlag");
		CHECK(ui::objective_icon(false, 2, 1).state == S::Enemy);
		CHECK(ui::objective_icon(true, 1, 1).icon == "UIHudIcon_Base");
		CHECK(ui::objective_icon(true, 1, 1).state == S::Friendly);
		CHECK(ui::objective_icon(true, 2, 1).state == S::Enemy);
		CHECK(ui::soldier_icon(true, 1, 1).icon == "UIHudIcon_LocalPlayer");
		CHECK(ui::soldier_icon(true, 1, 1).state == S::Default);
		CHECK(ui::soldier_icon(false, 2, 1).state == S::Enemy);
		CHECK(ui::soldier_icon(false, 1, 1).state == S::Friendly);
		CHECK(ui::soldier_icon(false, 0, 1).state == S::Neutral);
		CHECK(ui::local_player_icon(false).icon == "UIHudIcon_LocalPlayer");
		CHECK(ui::local_player_icon(false).state == S::Default);
		CHECK(ui::local_player_icon(true).icon == "UIHudIcon_LocalPlayer");
		CHECK(ui::local_player_icon(true).state == S::Squad);
	}

	TEST_CASE("hud icon rects and frames") {

		const ui::HudIconRect r = ui::hud_icon_rect({ 0.13671875f, 0.61816406f, 0.203125f, 0.63671875f }, 256, 1024);
		CHECK(r.x == 35);
		CHECK(r.y == 633);
		CHECK(r.width == 17);
		CHECK(r.height == 19);
		ui::HudIconStateFrames s;
		s.frame_rate = 10.0f;
		s.frames.resize(3);
		CHECK(ui::hud_icon_frame(s, 0.0) == 0);
		CHECK(ui::hud_icon_frame(s, 0.15) == 1);
		CHECK(ui::hud_icon_frame(s, 0.25) == 2);
		CHECK(ui::hud_icon_frame(s, 0.35) == 0);
		CHECK(ui::hud_icon_frame(s, -1.0) == 0);
		s.frame_rate = 0.0f;
		CHECK(ui::hud_icon_frame(s, 5.0) == 0);
		ui::HudIcon icon;
		icon.type = "UIHudIcon_Player";
		icon.states.push_back({ ui::HudIconState::Friendly, 0.0f, true, {} });
		CHECK(icon.state(ui::HudIconState::Friendly) != nullptr);
		CHECK(icon.state(ui::HudIconState::Enemy) == nullptr);
	}

	TEST_CASE("versions are non-zero") {
		CHECK(ui::kHudSourceTableVersion > 0);
		CHECK(ui::kHudModelVersion > 0);
	}
}
