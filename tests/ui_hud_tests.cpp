#include <doctest/doctest.h>

#include "bf3/core/ids.h"
#include "bf3/core/res/dx_texture.h"
#include "bf3/core/ui/hud_icons.h"
#include "bf3/core/ui/hud_model.h"
#include "bf3/core/ui/hud_sources.h"

#include <algorithm>
#include <string>
#include <vector>

namespace ui = bf3::core::ui;
namespace res = bf3::core::res;

namespace {

res::DxTexture header(res::TextureFormat p_format, uint16_t p_w, uint16_t p_h) {
	res::DxTexture t;
	t.kind = res::TextureKind::Texture2D;
	t.format = p_format;
	t.width = p_w;
	t.height = p_h;
	t.depth = 1;
	t.slice_count = 1;
	t.mip_count = 1;
	t.mip_sizes.push_back(uint32_t(res::texture_image_bytes(p_format, p_w, p_h)));
	t.mip_chain_size = t.mip_sizes.back();
	return t;
}

ui::HudSourceTable table_from_rows() {
	ui::HudSourceTable t;
	t.version = ui::kHudSourceTableVersion;
	t.state_partition = std::string(ui::kInGameStatePartition);
	for (std::string_view c : ui::kHudBoundComponents) {
		ui::HudComponentType type;
		type.type = std::string(c);
		type.partition = "ui/uicomponents/" + bf3::core::to_lower_ascii(c);
		for (const ui::HudBinding &b : ui::kHudBindings) {
			if (b.component == c) {
				type.sources.emplace_back(b.source);
			}
		}
		t.types.push_back(std::move(type));
	}
	ui::HudComponentType extra;
	extra.type = "UIVehicleCompData";
	extra.partition = "ui/uicomponents/uivehiclecomp";
	extra.sources = { "A", "B", "C" };
	t.types.push_back(extra);
	std::sort(t.types.begin(), t.types.end(), [](const ui::HudComponentType &a, const ui::HudComponentType &b) { return a.type < b.type; });
	for (uint32_t i = 0; i < t.types.size(); ++i) {
		t.state_components.push_back(i);
	}
	return t;
}

}

TEST_SUITE("ui-hud-unit") {
	TEST_CASE("icon state names round-trip; unknown names are Unknown") {
		for (ui::HudIconState s : { ui::HudIconState::Default, ui::HudIconState::Friendly, ui::HudIconState::Enemy, ui::HudIconState::Squad,
					 ui::HudIconState::Neutral, ui::HudIconState::Blink }) {
			CHECK(ui::parse_hud_icon_state(ui::hud_icon_state_name(s)) == s);
		}
		CHECK(ui::hud_icon_state_name(ui::HudIconState::Friendly) == "UIIconState_Friendly");
		CHECK(ui::parse_hud_icon_state("UIIconState_Sparkly") == ui::HudIconState::Unknown);
		CHECK(ui::parse_hud_icon_state("") == ui::HudIconState::Unknown);
		CHECK(ui::parse_hud_icon_state("uiiconstate_friendly") == ui::HudIconState::Unknown);
	}

	TEST_CASE("hud_icon_rect: whole texels, mirrored frames keep a positive size") {
		const ui::HudIconRect r = ui::hud_icon_rect({ 0.13671875f, 0.61816406f, 0.203125f, 0.63671875f }, 256, 1024);
		CHECK(r.x == 35);
		CHECK(r.y == 633);
		CHECK(r.width == 17);
		CHECK(r.height == 19);
		CHECK_FALSE(r.flip_x);
		CHECK_FALSE(r.flip_y);

		const ui::HudIconRect b = ui::hud_icon_rect({ 0.64453125f, 0.10253906f, 0.63671875f, 0.10058594f }, 256, 1024);
		CHECK(b.x == 163);
		CHECK(b.y == 103);
		CHECK(b.width == 2);
		CHECK(b.height == 2);
		CHECK(b.flip_x);
		CHECK(b.flip_y);
	}

	TEST_CASE("HudIconAtlas::find prefers the duplicate entry with states") {
		ui::HudIconAtlas atlas;
		atlas.icons.push_back({ "UIHudIcon_LocalPlayerBigIcon", {} });
		ui::HudIcon second{ "UIHudIcon_LocalPlayerBigIcon", {} };
		second.states.push_back({ ui::HudIconState::Default, 30.0f, true, { { 0, 0, 0.5f, 0.5f } } });
		atlas.icons.push_back(second);
		atlas.icons.push_back({ "UIHudIcon_RadarSweepComponent", {} });
		atlas.icons.push_back({ "UIHudIcon_RadarSweepComponent", {} });
		CHECK(atlas.find("UIHudIcon_LocalPlayerBigIcon") == &atlas.icons[1]);
		CHECK(atlas.find("UIHudIcon_RadarSweepComponent") == &atlas.icons[2]);
		CHECK(atlas.find("UIHudIcon_Nope") == nullptr);
		CHECK(atlas.with_states() == 1);
		CHECK(atlas.icons[1].state(ui::HudIconState::Default) != nullptr);
		CHECK(atlas.icons[1].state(ui::HudIconState::Enemy) == nullptr);
	}

	TEST_CASE("decode_rgba8_texture and decode_item_icon: good headers decode, hostile ones are refused") {
		res::DxTexture t = header(res::TextureFormat::Argb8888, 4, 2);
		std::vector<uint8_t> chunk(t.mip_chain_size);
		for (size_t i = 0; i < chunk.size(); ++i) {
			chunk[i] = uint8_t(i);
		}
		bf3::core::Result<ui::Rgba8Image> ok = ui::decode_rgba8_texture(t, chunk, "synthetic");
		REQUIRE(ok.ok());
		CHECK(ok->width == 4);
		CHECK(ok->rgba.size() == 32);
		CHECK(ok->rgba[17] == 17);
		res::DxTexture l8 = header(res::TextureFormat::L8, 4, 2);
		CHECK_FALSE(ui::decode_rgba8_texture(l8, chunk, "synthetic").ok());
		res::DxTexture big = header(res::TextureFormat::Argb8888, 8192, 2);
		CHECK_FALSE(ui::decode_rgba8_texture(big, chunk, "synthetic").ok());
		CHECK_FALSE(ui::decode_rgba8_texture(t, std::vector<uint8_t>(chunk.begin(), chunk.begin() + 31), "synthetic").ok());
		res::DxTexture arr = t;
		arr.kind = res::TextureKind::Array2D;
		arr.slice_count = 3;
		CHECK_FALSE(ui::decode_rgba8_texture(arr, chunk, "synthetic").ok());

		res::DxTexture dxt5 = header(res::TextureFormat::Dxt5, 64, 16);
		CHECK(dxt5.mip_chain_size == 64 * 16);
		std::vector<uint8_t> blocks(dxt5.mip_chain_size, 0xAB);
		bf3::core::Result<ui::ItemIconImage> icon = ui::decode_item_icon(dxt5, blocks, "synthetic");
		REQUIRE(icon.ok());
		CHECK(icon->format == res::TextureFormat::Dxt5);
		CHECK(icon->data.size() == 1024);
		CHECK_FALSE(ui::decode_item_icon(dxt5, std::vector<uint8_t>(blocks.begin(), blocks.begin() + 1000), "synthetic").ok());
		res::DxTexture odd = dxt5;
		odd.format = res::TextureFormat(200);
		CHECK_FALSE(ui::decode_item_icon(odd, blocks, "synthetic").ok());
	}

	TEST_CASE("check_hud_bindings: exact table, a missing source, an extra source, unbound components") {
		const ui::HudSourceTable exact = table_from_rows();
		const ui::HudBindingReport r = ui::check_hud_bindings(exact);
		CHECK(r.ok());
		CHECK(r.rows == 515);
		CHECK(r.matched == 515);
		CHECK(r.kinds[0] == 226);
		CHECK(r.kinds[1] == 131);
		CHECK(r.kinds[2] == 158);
		REQUIRE(r.unbound_components.size() == 1);
		CHECK(r.unbound_components[0] == "UIVehicleCompData");
		CHECK(r.unbound_sources == 3);

		ui::HudSourceTable missing = exact;
		ui::HudComponentType *weapon = const_cast<ui::HudComponentType *>(missing.find("UIWeaponCompData"));
		REQUIRE(weapon != nullptr);
		weapon->sources.erase(std::find(weapon->sources.begin(), weapon->sources.end(), "ActiveWeaponAmmo"));
		weapon->sources.push_back("ActiveWeaponLaserPointer");
		const ui::HudBindingReport m = ui::check_hud_bindings(missing);
		CHECK_FALSE(m.ok());
		REQUIRE(m.not_in_install.size() == 1);
		CHECK(m.not_in_install[0] == "UIWeaponCompData.ActiveWeaponAmmo");
		REQUIRE(m.not_in_rows.size() == 1);
		CHECK(m.not_in_rows[0] == "UIWeaponCompData.ActiveWeaponLaserPointer");
		CHECK(m.matched == 514);

		ui::HudSourceTable gone = exact;
		gone.types.erase(std::find_if(gone.types.begin(), gone.types.end(), [](const ui::HudComponentType &t) { return t.type == "UISquadCompData"; }));
		const ui::HudBindingReport g = ui::check_hud_bindings(gone);
		CHECK(g.not_in_install.size() == 30);
	}

	TEST_CASE("format_hud_source_table: header, one row per type, the in_state flag") {
		ui::HudSourceTable t;
		t.version = 1;
		t.state_partition = "ui/flow/state/ingamestatemp";
		t.types.push_back({ "UIAType", "ui/uicomponents/a", "UIUpdateType_Even", 10, { "X", "Y" }, {} });
		t.types.push_back({ "UIBType", "ui/uicomponents/b", "UIUpdateType_Always", -1, {}, {} });
		t.state_components = { 1 };
		const std::string s = ui::format_hud_source_table(t);
		CHECK(s.find("2 types, 1 state components, 2 sources") != std::string::npos);
		CHECK(s.find("UIAType\tui/uicomponents/a\tUIUpdateType_Even\t10\t0\t2\tX,Y\n") != std::string::npos);
		CHECK(s.find("UIBType\tui/uicomponents/b\tUIUpdateType_Always\t-1\t1\t0\t\n") != std::string::npos);
		CHECK(t.find("UIBType") == &t.types[1]);
		CHECK(t.find("UICType") == nullptr);
		CHECK(t.source_count() == 2);
	}
}
