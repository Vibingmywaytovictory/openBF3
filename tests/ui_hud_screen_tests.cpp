#include <doctest/doctest.h>

#include "bf3/core/ebx_db.h"
#include "bf3/core/install.h"
#include "bf3/core/install_snapshot.h"
#include "bf3/core/ui/as2_clip.h"
#include "bf3/core/ui/hud_screen.h"
#include "bf3/core/ui/hud_widgets.h"
#include "bf3/core/ui/swf_library.h"
#include "bf3/core/ui/swf_scene.h"
#include "bf3/core/ui/swf_text.h"
#include "bf3/core/ui/ui_fonts.h"
#include "test_support.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace bf3::core;
using namespace bf3::core::ui;

namespace {

struct Retail {
	bf3::test::Mounted *mount = nullptr;
	std::unique_ptr<SwfLibrary> lib;
	std::unique_ptr<SwfFontSet> fonts;
	std::unique_ptr<EbxLibrary> level;
	std::unique_ptr<EbxLibrary> ui;
	std::string error;
};

Retail *retail_or_skip() {
	static Retail r;
	static bool tried = false;
	if (!tried) {
		tried = true;
		r.mount = bf3::test::shared_mount_or_skip();
		if (r.mount == nullptr) {
			return nullptr;
		}
		Result<SwfLibrary> lib = SwfLibrary::open(r.mount->install);
		if (!lib) {
			r.error = lib.error().message;
			return &r;
		}
		r.lib = std::make_unique<SwfLibrary>(std::move(*lib));
		Result<std::unique_ptr<EbxLibrary>> ui = EbxLibrary::open(r.mount->install, { "Win32/UI" });
		if (!ui) {
			r.error = ui.error().message;
			return &r;
		}
		r.ui = std::move(*ui);
		Result<FontMappingCollection> mapping = font_mapping_for_text_database(*r.ui, "localization/us_loc");
		if (!mapping) {
			r.error = mapping.error().message;
			return &r;
		}
		WorkBudget fb(kUiFontConvertBudgetUnits);
		Result<SwfFontSet> fonts = SwfFontSet::load(*r.lib, *mapping, fb);
		if (!fonts) {
			r.error = fonts.error().message;
			return &r;
		}
		r.fonts = std::make_unique<SwfFontSet>(std::move(*fonts));
		Result<std::unique_ptr<EbxLibrary>> level = EbxLibrary::open(r.mount->install, { "Win32/Levels/MP_Subway/MP_Subway" });
		if (!level) {
			r.error = level.error().message;
			return &r;
		}
		r.level = std::move(*level);
	} else if (r.mount == nullptr) {
		bf3::test::shared_mount_or_skip();
		return nullptr;
	}
	REQUIRE_MESSAGE(r.error.empty(), r.error);
	return &r;
}

struct Screen {
	std::unique_ptr<SwfScene> scene;
	std::unique_ptr<HudScreen> hud;
	HudScreenAsset asset;

	const SwfSceneNode &at(std::string_view p_path) const {
		const SwfSceneNode *n = scene->find_node(p_path);
		REQUIRE_MESSAGE(n != nullptr, std::string(p_path));
		return *n;
	}
	As2Clip clip(std::string_view p_path) const { return hud->clip(p_path); }
	uint32_t label(std::string_view p_path, std::string_view p_label) const {
		const SwfSceneNode &n = at(p_path);
		REQUIRE(n.timeline != nullptr);
		const std::optional<uint32_t> f = n.timeline->label_frame(p_label);
		const std::string what = std::string(p_path) + " " + std::string(p_label);
		REQUIRE_MESSAGE(f.has_value(), what);
		return *f;
	}
	bool has_tilt(std::string_view p_path, float p_x, float p_y) const {
		for (const HudTilt &t : hud->tilts()) {
			if (t.path == p_path) {
				return t.x_rotation == p_x && t.y_rotation == p_y && t.perspective_fov == kHudPerspectiveFov;
			}
		}
		return false;
	}
	std::string renderer(std::string_view p_path) const {
		for (const HudRendererString &r : hud->renderer_strings()) {
			if (r.path == p_path) {
				return r.renderer;
			}
		}
		return {};
	}
};

HudClientState test_client() {
	HudClientState c;
	c.localise_hash = [](uint32_t p_hash) { return p_hash == 1u ? std::string("US") : std::string("RU"); };
	c.vertical_fov_deg = 55.0f;
	return c;
}

HudModel moment_m02() {
	HudModel m{};
	m.soldier.health = 100.0f;
	m.soldier.is_alive = true;
	m.soldier.compass_angle = 212.0f;
	m.weapon.active_weapon_ammo = 25;
	m.weapon.active_weapon_ammo_in_bag = 217;
	m.weapon.secondary_weapon_ammo = 1;
	m.weapon.active_weapon_show_ammo = true;
	m.weapon.firing_mode = 2;
	m.weapon.firing_mode_types = (1u << 0) | (1u << 2);
	m.weapon.active_weapon_crosshair = 1;
	m.team.friendly_team_name = 1;
	m.team.enemy_team1_name = 2;
	m.kill_counter.team1_kills = 9;
	m.kill_counter.team2_kills = 12;
	m.kill_counter.player_team = 1;
	m.kill_counter.team1_max_kills = 100;
	return m;
}

Screen open_screen(Retail &r_r, std::string_view p_partition, HudClientState p_client = test_client()) {
	Screen s;
	Result<HudScreenAsset> asset = read_hud_screen_asset(*r_r.level, p_partition);
	REQUIRE_MESSAGE(asset.ok(), (asset.ok() ? std::string() : asset.error().message));
	s.asset = *asset;
	Result<SwfScene> scene = SwfScene::open(*r_r.lib, asset->movie);
	REQUIRE_MESSAGE(scene.ok(), (scene.ok() ? std::string() : scene.error().message));
	s.scene = std::make_unique<SwfScene>(std::move(*scene));
	Result<std::unique_ptr<HudScreen>> hud = HudScreen::open(*s.scene, *r_r.lib, s.asset, r_r.fonts.get(), std::move(p_client));
	REQUIRE_MESSAGE(hud.ok(), (hud.ok() ? std::string() : hud.error().message));
	s.hud = std::move(*hud);
	return s;
}

Screen initialized(Retail &r_r, std::string_view p_partition, const HudModel &p_model, HudClientState p_client = test_client()) {
	Screen s = open_screen(r_r, p_partition, std::move(p_client));
	s.hud->initialize(p_model);
	CHECK(s.hud->stats().widgets_missing == 0);
	CHECK(s.hud->stats().as2.missing_paths == 0);
	CHECK_MESSAGE(s.hud->stats().as2.failed_ops == 0, s.hud->stats().as2.last_failure);
	return s;
}

void row(const char *p_name, const std::string &p_detail) {
	std::printf("HUD SCREEN ROW %s %s\n", p_name, p_detail.c_str());
	std::fflush(stdout);
}

}

TEST_SUITE("ui-hud-screen-unit") {
	TEST_CASE("hud screen unit: compass frame, crosshair dispersion, AS2 values, alignment and easing") {
		CHECK(hud_compass_frame(0.0f) == 0);
		CHECK(hud_compass_frame(212.0f) == 212);
		CHECK(hud_compass_frame(359.6f) == 0);
		CHECK(hud_compass_frame(-1.0f) == 359);
		CHECK(hud_compass_frame(std::nanf("")) == 0);
		CHECK(hud_crosshair_dispersion(0.0f, 55.0f, 720.0f) == 0.0);
		constexpr double kPi = 3.14159265358979323846;
		CHECK(hud_crosshair_dispersion(1.0f, 55.0f, 720.0f) == doctest::Approx(360.0 * std::tan(kPi / 180.0) / std::tan(27.5 * kPi / 180.0) / 100.0));
		CHECK(hud_crosshair_dispersion(1.0f, 0.0f, 720.0f) == 0.0);
		const double nan_value = As2Value::of("").to_number();
		CHECK(std::isnan(nan_value));
		CHECK(As2Value::of("12").to_number() == 12.0);
		CHECK(As2Value::of(12.0).to_string() == "12");
		CHECK(As2Value::of(0.5).to_string() == "0.5");
		CHECK(As2Value::of(true).to_string() == "true");
		CHECK_FALSE(As2Value::undefined().defined());
		CHECK_FALSE(As2Value::of("").truthy());
		CHECK(As2Value::of("0").truthy());
		CHECK_FALSE(As2Value::of(0.0).truthy());
		CHECK(as2_number_string(std::nan("")) == "NaN");
		CHECK(as2_number_string(-3.0) == "-3");
		CHECK(hud_align_axis(HudAxisAlign::Near, -2.8f, 0.0f, 1280.0f, 1280.0f, kHudFootageSafeX) == 47.0f);
		CHECK(hud_align_axis(HudAxisAlign::Far, 1193.0f, 0.0f, 1280.0f, 1280.0f, kHudFootageSafeX) == 1143.0f);
		CHECK(hud_align_axis(HudAxisAlign::Far, 1193.0f, -160.0f, 1600.0f, 1280.0f, kHudFootageSafeX) == 1303.0f);
		CHECK(hud_align_axis(HudAxisAlign::Centre, 640.5f, 0.0f, 1600.0f, 1280.0f, kHudFootageSafeX) == 640.5f);
		CHECK(hud_tween_ease(HudTweenEase::Linear, 0.25) == 0.25);
		CHECK(hud_tween_ease(HudTweenEase::EaseOutBack, 0.0) == doctest::Approx(0.0));
		CHECK(hud_tween_ease(HudTweenEase::EaseOutBack, 1.0) == doctest::Approx(1.0));
		CHECK(hud_tween_ease(HudTweenEase::EaseOutBack, 0.5) == doctest::Approx(1.0876975));
		CHECK(hud_tween_ease(HudTweenEase::EaseOutBack, 2.0) == doctest::Approx(1.0));
		CHECK(kAs2FiringModes[2] == "auto");
		CHECK(kAs2FiringModes[3] == "burst");
		CHECK(kAs2FiringModes[1] == "single");
	}
}

TEST_SUITE("ui-hud-screen") {
	TEST_CASE("hud screen: the MP_Subway screen assets and their widget classes") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		Result<HudScreenAsset> mp = read_hud_screen_asset(*r->level, kHudMpScreenPartition);
		Result<HudScreenAsset> tdm = read_hud_screen_asset(*r->level, kHudTdmScreenPartition);
		Result<HudScreenAsset> xh = read_hud_screen_asset(*r->level, kCrosshairDefaultPartition);
		REQUIRE(mp.ok());
		REQUIRE(tdm.ok());
		REQUIRE(xh.ok());
		CHECK(mp->movie == "ui/assets/hudmpscreen");
		CHECK(tdm->movie == "ui/assets/hudtdmscreen");
		CHECK(xh->movie == "ui/assets/crosshairdefault");
		CHECK(mp->widgets.size() == 14);
		CHECK(tdm->widgets.size() == 7);
		CHECK(xh->widgets.size() == 1);
		const HudWidgetNode *ammo = mp->find("Ammo_01");
		REQUIRE(ammo != nullptr);
		CHECK(ammo->horizontal == HudAxisAlign::Far);
		CHECK(ammo->vertical == HudAxisAlign::Far);
		const HudWidgetNode *hbw = tdm->find("HudBackgroundWidget_01");
		REQUIRE(hbw != nullptr);
		CHECK(hbw->horizontal == HudAxisAlign::Near);
		const HudWidgetNode *cross = xh->find("Crosshair_01");
		REQUIRE(cross != nullptr);
		CHECK(cross->horizontal == HudAxisAlign::Centre);
		bool hide_on_zoom = false;
		for (const HudWidgetProperty &p : cross->properties) {
			hide_on_zoom = hide_on_zoom || (p.name == "HideOnZoom" && p.value == "true");
		}
		CHECK(hide_on_zoom);
		Screen s = open_screen(*r, kHudMpScreenPartition);
		CHECK(s.hud->stats().unknown_classes == 5);
		for (const std::string &u : s.hud->stats().unknown_class_paths) {
			CHECK_MESSAGE((u.find("Widget.Image.ImageManager") != std::string::npos || u.find("SupportIconTag") != std::string::npos), u);
		}
		Screen t = open_screen(*r, kHudTdmScreenPartition);
		CHECK(t.hud->stats().unknown_classes == 1);
		CHECK(t.hud->widget("instance1.Compass_01.multiplayerCompass.bg") != nullptr);
		row("assets", "hudmpscreen 14 widgets, hudtdmscreen 7, crosshairdefault 1; " + std::to_string(s.hud->stats().controllers) + " + " +
						std::to_string(t.hud->stats().controllers) + " AS2 instances driven");
	}

	TEST_CASE("hud screen: TicketCounter in TDM") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		HudModel m = moment_m02();
		Screen s = initialized(*r, kHudTdmScreenPartition, m);
		const std::string tc = "instance1.TicketCounter_01";
		CHECK(s.at(tc).visible);
		CHECK(s.has_tilt(tc, -15.0f, -15.0f));
		CHECK(s.renderer(tc) == "t2");
		CHECK_FALSE(s.at(tc + ".tickets2").visible);
		CHECK_FALSE(s.at(tc + ".tickets3").visible);
		CHECK(s.at(tc + ".tickets0").frame == s.label(tc + ".tickets0", "enemy"));
		CHECK(s.at(tc + ".tickets1").frame == 0);
		CHECK(s.at(tc + ".tickets1.team").text == "US");
		CHECK(s.at(tc + ".tickets0.team").text == "RU");
		CHECK(s.at(tc + ".tickets1.tickets").text == "9");
		CHECK(s.at(tc + ".tickets0.tickets").text == "12");
		CHECK(s.at(tc + ".tickets1.meter.meter").frame == 0);
		CHECK(s.at(tc + ".tickets0.meter.meter").frame == 1);
		CHECK(s.clip(tc + ".tickets1.meter.meter").xscale() == doctest::Approx(9.0f));
		CHECK(s.clip(tc + ".tickets0.meter.meter").xscale() == doctest::Approx(12.0f));
		CHECK(s.at(tc + ".tickets1.meter.rightEdge").visible);
		CHECK_FALSE(s.at(tc + ".tickets0.meter.rightEdge").visible);
		CHECK_FALSE(s.at(tc + ".tickets0.meter.leftEdge").visible);
		const As2Clip friendly = s.clip(tc + ".tickets1");
		const As2Clip enemy = s.clip(tc + ".tickets0");
		const float name_w = std::max(friendly.child("team").text_width(), enemy.child("team").text_width());
		const float tick_w = std::max(friendly.child("tickets").text_width(), enemy.child("tickets").text_width());
		const float right = std::clamp(8.0f + name_w + 4.0f + tick_w, 80.0f, 105.0f);
		const float bar_x = std::max(right - 4.0f, 80.0f);
		for (const As2Clip &rw : { friendly, enemy }) {
			CHECK(rw.child("tickets").x() == doctest::Approx(right - tick_w - 20.0f));
			CHECK(rw.child("tickets").width() == doctest::Approx(tick_w + 20.0f));
			CHECK(rw.child("meter").x() == doctest::Approx(bar_x));
			CHECK(rw.child("meter").xscale() == doctest::Approx((170.0f - bar_x) / 90.0f * 100.0f));
		}
		m.kill_counter.team2_kills = 61;
		m.kill_counter.team1_kills = 50;
		s.hud->refresh(m);
		CHECK(s.at(tc + ".tickets1.tickets").text == "50");
		CHECK(s.at(tc + ".tickets0.tickets").text == "61");
		CHECK(s.clip(tc + ".tickets0.meter.meter").xscale() == doctest::Approx(61.0f));
		CHECK(s.clip(tc + ".tickets0.meter.rightEdge").x() ==
				doctest::Approx(s.clip(tc + ".tickets0.meter.meter").x() + s.clip(tc + ".tickets0.meter.meter").width()));
		m.kill_counter.player_team = 2;
		s.hud->refresh(m);
		CHECK(s.at(tc + ".tickets1.tickets").text == "61");
		CHECK(s.at(tc + ".tickets0.tickets").text == "50");
		HudClientState big = test_client();
		big.minimap_small = false;
		s.hud->set_client(big);
		s.hud->refresh(m);
		CHECK_FALSE(s.at(tc).visible);
		row("ticket_counter", "TDM rows tickets1 friendly / tickets0 enemy, name width " + std::to_string(name_w) + " -> right bound " + std::to_string(right));
	}

	TEST_CASE("hud screen: Ammo") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		HudModel m = moment_m02();
		Screen s = initialized(*r, kHudMpScreenPartition, m);
		const std::string a = "instance1.Ammo_01";
		const std::string am = a + ".Ammo";
		CHECK(s.at(a).visible);
		CHECK(s.at(a).matrix.tx == 1144.0f);
		CHECK(s.at(a).matrix.ty == 641.0f);
		CHECK(s.has_tilt(a, -15.0f, 15.0f));
		CHECK(s.renderer(a) == "t2");
		CHECK(s.at(am).frame == s.label(am, "us"));
		CHECK(s.at(am + ".clip.cltxt.txtDisplay").text == "25");
		CHECK(s.at(am + ".amholder.amtxt.txtDisplay").text == "217");
		CHECK(s.at(am + ".amholder.amtxtGlow.txtDisplay").text == "217");
		CHECK(s.at(am + ".grtxt.txtDisplay").text == "1");
		CHECK(s.at(am + ".fmd.auto").visible);
		CHECK(s.at(am + ".fmd.single").visible);
		CHECK_FALSE(s.at(am + ".fmd.burst").visible);
		CHECK(s.at(am + ".fmd.auto").frame == s.label(am + ".fmd.auto", "active"));
		CHECK(s.at(am + ".fmd.single").frame == s.label(am + ".fmd.single", "inactive"));
		CHECK(s.at(am + ".fmd.auto.gfx").frame == s.label(am + ".fmd.auto.gfx", "auto"));
		CHECK(s.at(am + ".fmd.single.gfx").frame == s.label(am + ".fmd.single.gfx", "single"));
		CHECK(s.at(am + ".fmd").visible);
		CHECK_FALSE(s.at(am + ".atp").visible);
		CHECK(s.at(am + ".weaponProgress").visible);
		CHECK(s.at(am + ".bgAmmo").visible);
		CHECK(s.renderer(am + ".bgAmmo.Background.renderHolder") == kBgDrawRenderer);
		m.weapon.firing_mode = 3;
		m.weapon.firing_mode_types = (1u << 0) | (1u << 3);
		m.weapon.active_weapon_ammo = 6;
		m.weapon.active_weapon_critical_ammo = true;
		m.weapon.secondary_weapon_ammo = 0;
		s.hud->refresh(m);
		CHECK(s.at(am + ".clip.cltxt.txtDisplay").text == "6");
		CHECK(s.at(am + ".grtxt.txtDisplay").text == "0");
		CHECK(s.at(am + ".fmd.burst").visible);
		CHECK_FALSE(s.at(am + ".fmd.auto").visible);
		CHECK(s.at(am + ".fmd.burst").frame == s.label(am + ".fmd.burst", "active"));
		CHECK(s.at(am + ".reload").frame == s.label(am + ".reload", "reload_us"));
		CHECK(s.at(am + ".reload").playing);
		CHECK(s.at(am + ".clip").frame == 0);
		m.weapon.active_weapon_critical_ammo = false;
		m.weapon.active_weapon_show_ammo = false;
		s.hud->refresh(m);
		CHECK(s.at(am + ".reload").frame == 0);
		CHECK(s.at(am + ".clip.cltxt.txtDisplay").text == "---");
		CHECK(s.at(am + ".amholder.amtxt.txtDisplay").text == "---");
		HudClientState hc = test_client();
		hc.hardcore = true;
		Screen h = initialized(*r, kHudMpScreenPartition, moment_m02(), hc);
		CHECK_FALSE(h.at(a).visible);
		const std::unique_ptr<HudWidgetController> ammo_ctl = make_hud_widget_controller("Widget.Hud.Ammo");
		REQUIRE(ammo_ctl != nullptr);
		HudData bound;
		ammo_ctl->bind(moment_m02(), hc, bound);
		const As2Value *is_hardcore = hud_data_find(bound, "IsHardcore");
		REQUIRE(is_hardcore != nullptr);
		CHECK(is_hardcore->truthy());
		row("ammo", "25/217, 1 grenade, auto of single+auto; burst id 3, low ammo reload_us, ShowAmmo off ---");
	}

	TEST_CASE("hud screen: Health") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		HudModel m = moment_m02();
		m.soldier.health = 94.0f;
		Screen s = initialized(*r, kHudMpScreenPartition, m);
		const std::string h = "instance1.Health_01";
		const std::string holder = h + ".Health.healthHolder";
		CHECK(s.at(h).visible);
		CHECK(s.at(h).matrix.tx == 1143.0f);
		CHECK(s.at(h).matrix.ty == 577.0f);
		CHECK(s.at(holder + ".amtxt.txtDisplay").text == "94");
		CHECK(s.at(holder).frame == s.label(holder, "us"));
		CHECK(s.has_tilt(holder, -15.0f, 15.0f));
		CHECK(s.renderer(holder) == "t2");
		CHECK(s.renderer(holder + ".EKGRenderTarget") == kSoldierRenderer);
		CHECK(s.at(holder + ".bgHealth").visible);
		m.soldier.health = 55.5f;
		s.hud->refresh(m);
		CHECK(s.at(holder + ".amtxt.txtDisplay").text == "56");
		m.soldier.health = 0.0f;
		s.hud->refresh(m);
		CHECK(s.at(holder + ".amtxt.txtDisplay").text == "0");
		row("health", "94 -> \"94\", 55.5 -> \"56\", EKG target t2;iUISoldierCompRender");
	}

	TEST_CASE("hud screen: Compass") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		HudModel m = moment_m02();
		Screen s = initialized(*r, kHudTdmScreenPartition, m);
		const std::string c = "instance1.Compass_01";
		const std::string hd = c + ".multiplayerCompass";
		CHECK(s.at(c).visible);
		CHECK(s.at(hd).symbol == "mc_singlePlayerCompass");
		CHECK(s.at(hd).dynamic);
		CHECK(s.has_tilt(hd, -15.0f, -15.0f));
		CHECK(s.renderer(hd) == "t2");
		CHECK_FALSE(s.at(hd + ".bg").visible);
		CHECK(s.at(hd + ".cps").frame == 212);
		CHECK(s.at(hd + ".cps").total_frames() == 361);
		for (int i = 1; i <= 5; ++i) {
			const std::string obj = hd + ".objective" + std::to_string(i) + ".obj";
			CHECK_FALSE(s.at(obj).visible);
			CHECK(s.at(obj).frame == std::min<uint32_t>(179, s.at(obj).total_frames() - 1));
		}
		m.soldier.compass_angle = 295.0f;
		s.hud->refresh(m);
		CHECK(s.at(hd + ".cps").frame == 295);
		m.soldier.compass_angle = 359.7f;
		s.hud->refresh(m);
		CHECK(s.at(hd + ".cps").frame == 0);
		row("compass", "mc_singlePlayerCompass attached as multiplayerCompass, cps frame = heading, 5 objectives hidden");
	}

	TEST_CASE("hud screen: HudBackgroundWidget and HudBackground") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		Screen s = initialized(*r, kHudTdmScreenPartition, moment_m02());
		const std::string w = "instance1.HudBackgroundWidget_01";
		const auto *api = dynamic_cast<HudBackgroundWidgetApi *>(s.hud->controller(w));
		const auto *bg = dynamic_cast<HudBackgroundApi *>(s.hud->controller(w + ".Background"));
		REQUIRE(api != nullptr);
		REQUIRE(bg != nullptr);
		CHECK(s.at(w).visible);
		CHECK(s.at(w).matrix.tx == 47.0f);
		CHECK(s.has_tilt(w, -15.0f, -15.0f));
		CHECK(api->tilt_mode() == "left");
		CHECK_FALSE(api->hardcore());
		CHECK(api->width_value() > bg->start_width());
		const As2Clip render = s.clip(w + ".Background.renderHolder");
		const double sw = api->width_value();
		const double sh = api->height_value();
		CHECK(render.width() == doctest::Approx(std::floor(sw + sw / kHorizontalFadePercentage + 0.5)));
		CHECK(render.x() == doctest::Approx(-sw / kHorizontalFadePercentage / 2.0));
		CHECK(render.height() == doctest::Approx(std::floor(sh + sh / kVerticalFadePercentage + 0.5)));
		CHECK(render.y() == doctest::Approx(-sh / kVerticalFadePercentage / 2.0));
		CHECK(s.renderer(w + ".Background.renderHolder") == kBgDrawRenderer);
		CHECK(s.renderer(w) == "t2");
		const float left = s.at(w).matrix.tx + render.x();
		const float right = left + render.width();
		CHECK(left == doctest::Approx(42.5f).epsilon(0.01));
		CHECK(right - s.at(w).matrix.tx == doctest::Approx(183.5f).epsilon(0.01));
		Screen mp = initialized(*r, kHudMpScreenPartition, moment_m02());
		const auto *ammo_bg = dynamic_cast<HudBackgroundWidgetApi *>(mp.hud->controller("instance1.Ammo_01.Ammo.bgAmmo"));
		REQUIRE(ammo_bg != nullptr);
		CHECK(mp.clip("instance1.Ammo_01.Ammo.bgAmmo.Background.renderHolder").height() == doctest::Approx(std::floor(90.0 + 90.0 / kVerticalFadePercentage + 0.5)));
		row("background", "TDM panel start " + std::to_string(sw) + " x " + std::to_string(sh) + ", renderHolder x " + std::to_string(left) + " .. " +
						std::to_string(right) + " before the -15 deg tilt");
	}

	TEST_CASE("hud screen: Minimap") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		Screen s = initialized(*r, kHudTdmScreenPartition, moment_m02());
		const std::string mm = "instance1.Minimap_01";
		CHECK(s.at(mm).visible);
		CHECK(s.renderer(mm + ".map") == kMinimapRenderer);
		CHECK(s.has_tilt(mm, -15.0f, -15.0f));
		CHECK_FALSE(s.at(mm + ".testCross").visible);
		CHECK(s.at(mm + ".hitindicator").frame == s.label(mm + ".hitindicator", "idle"));
		const float w0 = s.clip(mm).width();
		const float x0 = s.clip(mm).x();
		HudClientState big = test_client();
		big.minimap_small = false;
		s.hud->set_client(big);
		s.hud->refresh(moment_m02());
		CHECK(s.at(mm + ".tint").frame == s.label(mm + ".tint", "MaxIn"));
		CHECK(s.hud->tweens().size() == 4);
		s.hud->advance(0.0);
		s.hud->advance(0.025);
		CHECK(s.clip(mm).width() > w0);
		s.hud->advance(0.05);
		CHECK(s.clip(mm).width() == doctest::Approx(600.0f).epsilon(0.001));
		CHECK(s.clip(mm).x() == doctest::Approx(x0 - 40.0f));
		CHECK(s.hud->tweens().empty());
		row("minimap", "map t2;iUIMinimapRender, Size false tweens 4 properties over 0.05 s easeOutBack to 600 px");
	}

	TEST_CASE("hud screen: Crosshair") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		HudModel m = moment_m02();
		constexpr float kPi = 3.14159265358979323846f;
		const float vfov = 55.0f;
		m.weapon.dispersion = std::atan(44.125f / 360.0f * std::tan(vfov / 2.0f * kPi / 180.0f)) * 180.0f / kPi;
		Screen s = initialized(*r, kCrosshairDefaultPartition, m);
		const std::string x = "instance1.Crosshair_01";
		CHECK(s.at(x).visible);
		CHECK(s.at(x).matrix.tx == 640.0f);
		CHECK(s.at(x).matrix.ty == 360.0f);
		CHECK(s.at(x + ".crosshair").visible);
		CHECK_FALSE(s.at(x + ".dot").visible);
		CHECK_FALSE(s.at(x + ".circle").visible);
		CHECK_FALSE(s.at(x + ".grenade").visible);
		CHECK(s.renderer(x + ".crosshair") == "x1");
		CHECK(s.at(x + ".crosshair.up").matrix.ty == -44.0f);
		CHECK(s.at(x + ".crosshair.down").matrix.ty == 44.0f);
		CHECK(s.at(x + ".crosshair.left").matrix.tx == -44.0f);
		CHECK(s.at(x + ".crosshair.right").matrix.tx == 44.0f);
		m.weapon.zoomed = true;
		s.hud->refresh(m);
		s.hud->advance(0.0);
		s.hud->advance(0.1);
		CHECK(s.at(x + ".crosshair").color.mul[3] == doctest::Approx(0.5f).epsilon(0.01));
		s.hud->advance(0.15);
		CHECK(s.at(x + ".crosshair").color.mul[3] == doctest::Approx(0.0f));
		m.weapon.zoomed = false;
		s.hud->refresh(m);
		s.hud->advance(0.0);
		s.hud->advance(0.1);
		CHECK(s.at(x + ".crosshair").color.mul[3] == doctest::Approx(1.0f));
		m.weapon.active_weapon_crosshair = 0;
		s.hud->refresh(m);
		CHECK_FALSE(s.at(x).visible);
		row("crosshair", "dispersion 44.125 px -> arms at +-44, HideOnZoom fades 0.2 s out / 0.1 s in");
	}

	TEST_CASE("hud screen: Hitindicator, LatencyIndicator and the widgets hidden until data") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		HudModel m = moment_m02();
		Screen s = initialized(*r, kHudMpScreenPartition, m);
		const std::string hi = "instance1.Hitindicator_01.hitindicator";
		CHECK(s.at("instance1.Hitindicator_01").visible);
		m.weapon.show_hitindicator = true;
		m.weapon.hitindicator_level = 1.0f;
		s.hud->refresh(m);
		CHECK(s.at(hi).frame == 1);
		CHECK(s.at(hi + ".HitController").frame == 0);
		CHECK_FALSE(s.at("instance1.LatencyIndicator_01").visible);
		for (const char *w : { "HudMessage_01", "HudMessage_02", "HudMessage_04", "HudMessage_05", "PassangerList_01", "VehicleHealth_01", "ScoreAggregator_01",
					 "SupportIconManager_01", "HudAlertMessage_01", "HudAlertMessage_02" }) {
			CHECK_MESSAGE(!s.at(std::string("instance1.") + w).visible, w);
		}
		CHECK_FALSE(s.at("instance1.HudMessage_01.posGuide").visible);
		Screen t = initialized(*r, kHudTdmScreenPartition, m);
		CHECK_FALSE(t.at("instance1.SquadList_00").visible);
		CHECK_FALSE(t.at("instance1.HudMessage_00").visible);
		CHECK_FALSE(t.at("instance1.MapmarkerManager_00").visible);
		row("hidden", "hit level 1 -> frame index 1, latency hidden, 13 data-driven widgets hidden until their data");
	}

	TEST_CASE("hud screen: install unchanged") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		const SnapshotDiff diff = diff_snapshots(r->mount->before, r->mount->reader.snapshot());
		std::printf("ui.hud.screen install.unchanged %s\n", diff.clean() ? "yes" : diff.summary().c_str());
		CHECK(diff.clean());
	}
}
