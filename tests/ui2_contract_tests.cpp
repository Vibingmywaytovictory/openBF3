// UI ui2 interface contract (docs/design/ui-minimap-icons.md §13): the minimap / HUD icon / HUD data-model headers
// compile together, the binding rows keep the counts the install had on 2026-10-15 (design §7.2), and the inline
// helpers both the readers and the Godot side use (the minimap mapping, the view, the icon rects) behave as the
// design says. No data, no Godot; nothing here calls a function the implementation wave has yet to write.
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

// The binding table is constexpr: the counts are compile-time facts of the header.
static_assert(std::size(ui::kHudBindings) == 235);
static_assert(ui::hud_binding_count(ui::HudBindKind::Model) == 150);
static_assert(ui::hud_binding_count(ui::HudBindKind::Client) == 35);
static_assert(ui::hud_binding_count(ui::HudBindKind::Unused) == 50);
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
static_assert(std::is_trivially_copyable_v<ui::HudModel>);
static_assert(std::is_standard_layout_v<ui::HudModel>);

// The research mapping on Metro's UIMinimapData (design §4.1).
constexpr ui::MinimapMapping kMetro{ 0.0f, -162.0f, 1024.0f };

bool near(float p_a, float p_b, float p_eps) {
	return std::fabs(p_a - p_b) <= p_eps;
}

} // namespace

TEST_SUITE("ui2-contract") {
	TEST_CASE("binding rows: every bound component, no duplicate, Model fields are snake_case of the source") {
		std::set<std::string_view> components(std::begin(ui::kHudBoundComponents), std::end(ui::kHudBoundComponents));
		CHECK(components.size() == 11);
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
		CHECK(ui::hud_snake_case("ActiveWeaponAmmo", buf, 4) == 0); // does not fit
	}

	TEST_CASE("minimap mapping: research formula, inverse, variants") {
		// The centre lands on the image centre; +x goes left, +z goes up (both axes mirrored).
		const ui::MinimapUv c = ui::world_to_minimap_uv(kMetro, 0.0f, -162.0f);
		CHECK(near(c.u, 0.5f, 1e-6f));
		CHECK(near(c.v, 0.5f, 1e-6f));
		const ui::MinimapUv px = ui::world_to_minimap_uv(kMetro, 100.0f, -162.0f);
		CHECK(px.u < 0.5f);
		const ui::MinimapUv pz = ui::world_to_minimap_uv(kMetro, 0.0f, -62.0f);
		CHECK(pz.v < 0.5f);
		// Metro's US HQ (bf3dump level MP_Subway ConquestSmall0): u = 0.5 + 108.03/1024, v = 0.5 - (-147.5 + 162)/1024.
		const ui::MinimapUv hq = ui::world_to_minimap_uv(kMetro, -108.03f, -147.5f);
		CHECK(near(hq.u, 0.605498f, 1e-5f));
		CHECK(near(hq.v, 0.485840f, 1e-5f));
		const std::array<float, 2> back = ui::minimap_uv_to_world(kMetro, hq);
		CHECK(near(back[0], -108.03f, 1e-3f));
		CHECK(near(back[1], -147.5f, 1e-3f));
		// Variant 0 is the research mapping; the 8 variants are distinct off the diagonal.
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

		// North-up: +Z is up, +X is left (the mirrored image); the marker turns by -heading.
		view.rotate = false;
		view.heading = kPi / 2.0f; // looking down +X
		const ui::ScreenPoint north = ui::minimap_project(view, 10.0f, 30.0f);
		CHECK(near(north.x, 128.0f, 1e-4f));
		CHECK(near(north.y, 108.0f, 1e-4f));
		const ui::ScreenPoint plus_x = ui::minimap_project(view, 20.0f, 20.0f);
		CHECK(near(plus_x.x, 108.0f, 1e-4f));
		CHECK(near(plus_x.y, 128.0f, 1e-4f));
		CHECK(near(ui::minimap_marker_angle(view), -kPi / 2.0f, 1e-6f));

		// Rotating: whatever the heading, the point straight ahead is straight up and the marker does not turn.
		view.rotate = true;
		for (float heading : { 0.0f, kPi / 2.0f, kPi, -kPi / 3.0f, 2.5f }) {
			view.heading = heading;
			const float ahead_x = view.player_x + 10.0f * std::sin(heading);
			const float ahead_z = view.player_z + 10.0f * std::cos(heading);
			const ui::ScreenPoint p = ui::minimap_project(view, ahead_x, ahead_z);
			CHECK(near(p.x, 128.0f, 1e-3f));
			CHECK(near(p.y, 108.0f, 1e-3f));
			// A higher yaw is a turn to the left (input_mapper.h: the mouse to the right lowers the yaw), so the point at
			// heading + 90 degrees is to the left of the marker.
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
		// A world point's screen offset from the player equals its image offset from the player's texel, scaled.
		ui::MinimapView view;
		view.rotate = false;
		view.pixels_per_metre = 0.25f;
		view.player_x = -108.03f;
		view.player_z = -147.5f;
		const float image_px = kMetro.size * view.pixels_per_metre; // image side on screen
		const ui::MinimapUv a = ui::world_to_minimap_uv(kMetro, view.player_x, view.player_z);
		const ui::MinimapUv b = ui::world_to_minimap_uv(kMetro, -193.69f, -610.25f);
		const ui::ScreenPoint s = ui::minimap_project(view, -193.69f, -610.25f);
		CHECK(near(s.x, (b.u - a.u) * image_px, 1e-2f));
		CHECK(near(s.y, (b.v - a.v) * image_px, 1e-2f));
	}

	TEST_CASE("sdf_inside and L8Image::at_uv") {
		CHECK(ui::sdf_inside(0, ui::kResearchCombatAreaThreshold));
		CHECK(ui::sdf_inside(126, ui::kResearchCombatAreaThreshold)); // 0.494
		CHECK_FALSE(ui::sdf_inside(127, ui::kResearchCombatAreaThreshold)); // 0.498
		CHECK_FALSE(ui::sdf_inside(255, ui::kResearchCombatAreaThreshold));
		ui::L8Image img;
		img.width = 2;
		img.height = 2;
		img.texels = { 1, 2, 3, 4 };
		CHECK(img.at_uv({ 0.0f, 0.0f }) == 1);
		CHECK(img.at_uv({ 0.99f, 0.0f }) == 2);
		CHECK(img.at_uv({ 0.0f, 0.99f }) == 3);
		CHECK(img.at_uv({ 2.0f, 2.0f }) == 4); // clamped
		CHECK(img.at_uv({ -1.0f, -1.0f }) == 1);
		CHECK(ui::L8Image{}.at_uv({ 0.5f, 0.5f }) == 255); // empty: outside
	}

	TEST_CASE("hud icon rects and frames") {
		// UIHudIcon_LocalPlayer Default on the 256 x 1024 atlas (design §6.1).
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
