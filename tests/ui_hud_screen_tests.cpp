#include <doctest/doctest.h>

#include "bf3/core/ebx_db.h"
#include "bf3/core/install.h"
#include "bf3/core/install_snapshot.h"
#include "bf3/core/ui/as2_clip.h"
#include "bf3/core/ui/hud_messages.h"
#include "bf3/core/ui/hud_screen.h"
#include "bf3/core/ui/hud_widgets.h"
#include "bf3/core/ui/swf_library.h"
#include "bf3/core/ui/swf_scene.h"
#include "bf3/core/ui/swf_text.h"
#include "bf3/core/ui/text_db.h"
#include "bf3/core/ui/ui_fonts.h"
#include "test_support.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <functional>
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

const TextDatabase &text_db(Retail &r_r) {
	static std::unique_ptr<TextDatabase> db;
	if (db == nullptr) {
		Result<TextDatabase> t = open_text_database(r_r.mount->install);
		REQUIRE_MESSAGE(t.ok(), (t.ok() ? std::string() : t.error().message));
		db = std::make_unique<TextDatabase>(std::move(*t));
	}
	return *db;
}

HudClientState text_client(const TextDatabase &p_db) {
	HudClientState c = test_client();
	c.localise_hash = [&p_db](uint32_t p_hash) {
		const std::optional<std::string_view> t = p_db.find(p_hash);
		return t ? std::string(*t) : std::string();
	};
	c.translate = [&p_db](std::string_view p_sid) -> std::optional<std::string> {
		const std::optional<std::string_view> t = p_db.find_sid(p_sid);
		return t ? std::optional<std::string>(std::string(*t)) : std::nullopt;
	};
	return c;
}

void set_player(HudModel &r_m, uint8_t p_player, int8_t p_team, std::string_view p_name) {
	HudPlayer &pl = r_m.players[p_player];
	pl.team = p_team;
	pl.name.fill('\0');
	std::copy_n(p_name.data(), std::min(p_name.size(), pl.name.size() - 1), pl.name.data());
}

HudModel kill_model() {
	HudModel m = moment_m02();
	m.local_player = 0;
	m.local_team = 1;
	set_player(m, 0, 1, "AAAA");
	set_player(m, 1, 2, "BBBBBB");
	set_player(m, 2, 1, "CC");
	set_player(m, 3, 2, "DDD");
	return m;
}

void push_line(HudKillFeed &f, uint8_t p_killer, uint8_t p_victim, uint32_t p_weapon, bool p_headshot, bool p_team_kill, bool p_suicide) {
	++f.newest_serial;
	HudKill &k = f.lines[f.newest_serial % f.lines.size()];
	k = HudKill{};
	k.serial = f.newest_serial;
	k.killer = p_killer;
	k.victim = p_victim;
	k.weapon_name_sid_hash = p_weapon;
	k.headshot = p_headshot;
	k.team_kill = p_team_kill;
	k.suicide = p_suicide;
}

void push_kill(HudModel &r_m, uint8_t p_killer, uint8_t p_victim, uint32_t p_weapon, bool p_headshot, bool p_team_kill = false, bool p_suicide = false) {
	push_line(r_m.messages.kill_message, p_killer, p_victim, p_weapon, p_headshot, p_team_kill, p_suicide);
}

void push_score(HudModel &r_m, uint32_t p_sid_hash, int32_t p_points) {
	HudScoreFeed &f = r_m.messages.score_message;
	++f.newest_serial;
	HudScoreEvent &e = f.events[f.newest_serial % f.events.size()];
	e.serial = f.newest_serial;
	e.sid_hash = p_sid_hash;
	e.points = p_points;
}

void push_local_kill(HudModel &r_m, uint8_t p_killer, uint8_t p_victim, uint32_t p_weapon, bool p_headshot) {
	push_line(r_m.messages.local_kill_message, p_killer, p_victim, p_weapon, p_headshot, false, false);
}

void push_damage(HudModel &r_m, uint8_t p_shooter, float p_bearing_deg, float p_damage) {
	HudDamageFeed &f = r_m.damage;
	++f.newest_serial;
	HudDamageHit &h = f.hits[f.newest_serial % f.hits.size()];
	h.serial = f.newest_serial;
	h.shooter = p_shooter;
	h.bearing_deg = p_bearing_deg;
	h.damage = p_damage;
}

void drive(Screen &r_s, std::string_view p_path, std::string_view p_key, const As2Value &p_value) {
	const HudWidgetState *w = r_s.hud->widget(p_path);
	REQUIRE_MESSAGE(w != nullptr, std::string(p_path));
	w->controller->update(*w->host, p_key, p_value);
}

HudMessageCompData message_data(Retail &r_r) {
	Result<HudMessageCompData> d = read_hud_message_comp(*r_r.level);
	REQUIRE_MESSAGE(d.ok(), (d.ok() ? std::string() : d.error().message));
	return *d;
}

uint32_t frames_until(Screen &r_s, const std::function<bool()> &p_done, uint32_t p_limit) {
	const double dt = 1.0 / double(r_s.scene->frame_rate());
	for (uint32_t f = 1; f <= p_limit; ++f) {
		REQUIRE(r_s.scene->advance(1).ok());
		r_s.hud->advance(dt);
		if (p_done()) {
			return f;
		}
	}
	return 0;
}

const HudTween *tween_of(const Screen &p_s, std::string_view p_path, HudTweenProperty p_property) {
	for (const HudTween &t : p_s.hud->tweens()) {
		if (t.path == p_path && t.property == p_property) {
			return &t;
		}
	}
	return nullptr;
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
		CHECK(hud_tween_ease(HudTweenEase::ExpoOut, 0.0) == 0.0);
		CHECK(hud_tween_ease(HudTweenEase::ExpoOut, 1.0) == 1.0);
		CHECK(hud_tween_ease(HudTweenEase::ExpoOut, 0.5) == doctest::Approx(1.001 * (1.0 - std::pow(2.0, -5.0))));
		As2Value o = As2Value::object();
		o.set("Id", As2Value::of(3.0)).set("Type", As2Value::of("Kill"));
		o.set("Id", As2Value::of(4.0));
		CHECK(o.field("Id").to_number() == 4.0);
		CHECK(o.names.size() == 2);
		CHECK_FALSE(o.field("Missing").defined());
		CHECK(o.truthy());
		CHECK(o.to_string() == "[object Object]");
		As2Value l = As2Value::array({ As2Value::of(1.0), As2Value::of("a") });
		l.push(As2Value::of(true));
		CHECK(l.size() == 3);
		CHECK(l.to_string() == "1,a,true");
		CHECK_FALSE(l.at(9).defined());
		CHECK(l == As2Value::array({ As2Value::of(1.0), As2Value::of("a"), As2Value::of(true) }));
		CHECK_FALSE(l == As2Value::array());
		const std::array<std::string, 2> args{ "M16A4", "x" };
		CHECK(hud_format_text("[{0:~}]", args) == "[M16A4]");
		CHECK(hud_format_text("{1} {0}", args) == "x M16A4");
		CHECK(hud_format_text("{2}{a}{", args) == "{a}{");
		CHECK(hud_message_source(224794678) == HudMessageSource::Kill);
		CHECK(hud_message_source(1675248891) == HudMessageSource::LocalKill);
		CHECK(hud_message_source(7) == HudMessageSource::None);
		HudModel m = kill_model();
		m.players[2].squad = 1;
		CHECK(hud_relation(m, 0) == kHudRelationSquad);
		CHECK(hud_relation(m, 1) == kHudRelationEnemy);
		CHECK(hud_relation(m, 2) == kHudRelationFriendly);
		m.players[0].squad = 1;
		CHECK(hud_relation(m, 2) == kHudRelationSquad);
		CHECK(hud_relation(m, kHudNoPlayer) == kHudRelationEnemy);
	}

	TEST_CASE("hud screen unit: message queue rows, expiry, overflow and RemoveFlag") {
		HudMessageCompData d;
		HudMessageInfo &k = d.info[size_t(HudMessageSource::Kill)];
		k.queue_size = 4;
		k.normal_seconds = 5.0f;
		k.short_seconds = 1.0f;
		k.row_type = "Kill";
		HudMessageComp comp(d);
		HudClientState c;
		c.localise_hash = [](uint32_t p_hash) { return p_hash == kHudKillWeaponFormatHash ? std::string(kHudKillWeaponFormat) : std::string("W"); };
		HudModel m = kill_model();
		comp.update(m, c, 0.0);
		CHECK(comp.list(HudMessageSource::Kill).size() == 0);
		push_kill(m, 0, 1, 7, true);
		comp.update(m, c, 0.0);
		const As2Value &l = comp.list(HudMessageSource::Kill);
		REQUIRE(l.size() == 1);
		CHECK(l.at(0).field("Id").to_number() == 1.0);
		CHECK(l.at(0).field("Type").to_string() == "Kill");
		CHECK(l.at(0).field("RemoveFlag").to_number() == 0.0);
		const As2Value &v = l.at(0).field("Value");
		CHECK(v.field("KillerColor").to_number() == kHudRelationSquad);
		CHECK(v.field("VictimColor").to_number() == kHudRelationEnemy);
		CHECK(v.field("Text").to_string() == "[W]");
		CHECK(v.field("KillerName").to_string() == "AAAA");
		CHECK(v.field("VictimName").to_string() == "BBBBBB");
		CHECK(v.field("HeadShot").truthy());
		CHECK_FALSE(v.field("TeamKill").truthy());
		push_kill(m, 1, 1, 7, false, false, true);
		comp.update(m, c, 1.0);
		REQUIRE(comp.list(HudMessageSource::Kill).size() == 2);
		const As2Value &sv = comp.list(HudMessageSource::Kill).at(1).field("Value");
		CHECK(sv.field("Text").to_string() == kHudKillsIsNoSid);
		CHECK_FALSE(sv.field("KillerName").defined());
		CHECK(sv.field("KillerColor").to_number() == kHudRelationEnemy);
		comp.update(m, c, 4.99);
		CHECK(comp.list(HudMessageSource::Kill).at(0).field("RemoveFlag").to_number() == 0.0);
		comp.update(m, c, 5.0);
		REQUIRE(comp.list(HudMessageSource::Kill).size() == 2);
		CHECK(comp.list(HudMessageSource::Kill).at(0).field("RemoveFlag").to_number() == 1.0);
		comp.update(m, c, 5.1);
		REQUIRE(comp.list(HudMessageSource::Kill).size() == 1);
		CHECK(comp.list(HudMessageSource::Kill).at(0).field("Id").to_number() == 2.0);
		HudMessageComp full(d);
		HudModel f = kill_model();
		for (int i = 0; i < 5; ++i) {
			push_kill(f, 3, 2, 7, false);
		}
		full.update(f, c, 0.0);
		CHECK(full.list(HudMessageSource::Kill).size() == 5);
		full.update(f, c, 0.99);
		CHECK(full.list(HudMessageSource::Kill).at(0).field("RemoveFlag").to_number() == 0.0);
		full.update(f, c, 1.0);
		CHECK(full.list(HudMessageSource::Kill).at(0).field("RemoveFlag").to_number() == 1.0);
		CHECK(full.list(HudMessageSource::Kill).at(1).field("RemoveFlag").to_number() == 0.0);
		const As2Value &fv = full.list(HudMessageSource::Kill).at(1).field("Value");
		CHECK(fv.field("KillerColor").to_number() == kHudRelationEnemy);
		CHECK(fv.field("VictimColor").to_number() == kHudRelationFriendly);
		push_kill(f, 0, 2, 7, false, true);
		full.update(f, c, 2.0);
		CHECK(full.list(HudMessageSource::Kill).at(full.list(HudMessageSource::Kill).size() - 1).field("Value").field("TeamKill").truthy());
		push_kill(f, 3, 2, 7, false, true);
		full.update(f, c, 2.0);
		CHECK_FALSE(full.list(HudMessageSource::Kill).at(full.list(HudMessageSource::Kill).size() - 1).field("Value").field("TeamKill").truthy());
		for (int i = 0; i < 20; ++i) {
			push_kill(f, 3, 2, 7, false);
		}
		full.update(f, c, 3.0);
		CHECK(full.messages_added(HudMessageSource::Kill) == 7 + kHudKillFeedSize);
		HudMessageCompData ld = d;
		ld.info[size_t(HudMessageSource::LocalKill)] = HudMessageInfo{ 2, 5.0f, 1.0f, "LocalKill" };
		HudMessageComp local(ld);
		HudModel lm = kill_model();
		push_local_kill(lm, 0, 1, 7, true);
		local.update(lm, c, 0.0);
		CHECK(local.list(HudMessageSource::Kill).size() == 0);
		REQUIRE(local.list(HudMessageSource::LocalKill).size() == 1);
		const As2Value &le = local.list(HudMessageSource::LocalKill).at(0);
		CHECK(le.field("Type").to_string() == "LocalKill");
		CHECK(le.field("Value").field("KillerName").to_string() == "AAAA");
		CHECK(le.field("Value").field("Text").to_string() == "[W]");
		CHECK_FALSE(le.field("Value").field("IsLocalPlayer").truthy());
		CHECK_FALSE(le.field("Value").field("TeamKill").defined());
		push_local_kill(lm, 3, 0, 7, false);
		push_local_kill(lm, 0, 3, 7, false);
		local.update(lm, c, 0.5);
		REQUIRE(local.list(HudMessageSource::LocalKill).size() == 3);
		CHECK(local.list(HudMessageSource::LocalKill).at(1).field("Value").field("IsLocalPlayer").truthy());
		local.update(lm, c, 1.5);
		CHECK(local.list(HudMessageSource::LocalKill).at(0).field("RemoveFlag").to_number() == 1.0);
		CHECK(local.list(HudMessageSource::LocalKill).at(1).field("RemoveFlag").to_number() == 0.0);
		HudMessageCompData sd = d;
		sd.info[size_t(HudMessageSource::Score)] = HudMessageInfo{ 3, 5.0f, 5.0f, "Score" };
		sd.score_aggregate_time = 8.0f;
		HudMessageComp score(sd);
		HudModel sm = kill_model();
		score.update(sm, c, 0.0);
		CHECK_FALSE(score.score_aggregate().defined());
		push_score(sm, 7, 100);
		push_score(sm, 7, 10);
		score.update(sm, c, 0.0);
		REQUIRE(score.list(HudMessageSource::Score).size() == 2);
		CHECK(score.list(HudMessageSource::Score).at(0).field("Type").to_string() == "Score");
		CHECK(score.list(HudMessageSource::Score).at(0).field("Value").field("Text").to_string() == "W");
		CHECK(score.list(HudMessageSource::Score).at(0).field("Value").field("Score").to_number() == 100.0);
		CHECK(score.list(HudMessageSource::Score).at(1).field("Value").field("Score").to_number() == 10.0);
		CHECK(score.score_aggregate().to_number() == 110.0);
		push_score(sm, 7, 100);
		score.update(sm, c, 7.9);
		CHECK(score.score_aggregate().to_number() == 210.0);
		score.update(sm, c, 15.89);
		CHECK(score.score_aggregate().to_number() == 210.0);
		score.update(sm, c, 15.9);
		CHECK(score.score_aggregate().to_number() == 0.0);
		push_score(sm, 7, 100);
		score.update(sm, c, 16.0);
		CHECK(score.score_aggregate().to_number() == 100.0);
	}

	TEST_CASE("hud screen unit: damage indicator angle frames, sources, expiry and the 30 Hz update") {
		CHECK(hud_damage_angle_frame(0.0f, 0.0f) == 1);
		CHECK(hud_damage_angle_frame(0.0f, 270.0f) == 91);
		CHECK(hud_damage_angle_frame(0.0f, 90.0f) == 271);
		CHECK(hud_damage_angle_frame(90.0f, 90.0f) == 1);
		CHECK(hud_damage_angle_frame(10.0f, 190.0f) == 181);
		CHECK(hud_damage_angle_frame(359.6f, 0.0f) == 1);
		CHECK(hud_damage_angle_frame(std::nanf(""), 0.0f) == 1);
		HudDamageIndicatorComp comp(HudDamageIndicatorCompData{ 5.0f, 30 });
		HudModel m{};
		m.soldier.compass_angle = 0.0f;
		comp.update(m, 0.0);
		CHECK(comp.indicators().size() == 0);
		push_damage(m, 4, 270.0f, 30.0f);
		comp.update(m, 0.1);
		REQUIRE(comp.indicators().size() == 1);
		CHECK(comp.indicators().at(0).field("Angle").to_number() == 91.0);
		CHECK(comp.indicators().at(0).field("Scale").to_number() == 30.0);
		CHECK(comp.indicators().at(0).field("New").truthy());
		comp.update(m, 0.11);
		CHECK(comp.indicators().at(0).field("New").truthy());
		comp.update(m, 0.2);
		CHECK_FALSE(comp.indicators().at(0).field("New").truthy());
		m.soldier.compass_angle = 90.0f;
		comp.update(m, 0.3);
		CHECK(comp.indicators().at(0).field("Angle").to_number() == 181.0);
		push_damage(m, 5, 90.0f, 140.0f);
		push_damage(m, 6, 180.0f, 10.0f);
		push_damage(m, 7, 0.0f, 10.0f);
		comp.update(m, 1.0);
		REQUIRE(comp.indicators().size() == kHudDamageIndicators);
		CHECK(comp.indicators().at(0).field("Scale").to_number() == 100.0);
		push_damage(m, 4, 0.0f, 20.0f);
		comp.update(m, 2.0);
		CHECK(comp.indicators().at(0).field("Angle").to_number() == 91.0);
		CHECK(comp.indicators().at(0).field("New").truthy());
		CHECK(comp.hits_seen() == 5);
		comp.update(m, 6.0);
		REQUIRE(comp.indicators().size() == 1);
		CHECK(comp.indicators().at(0).field("Scale").to_number() == 20.0);
		comp.update(m, 7.0);
		CHECK(comp.indicators().size() == 0);
		for (int i = 0; i < int(kHudDamageFeedSize) + 3; ++i) {
			push_damage(m, uint8_t(10 + i), 0.0f, 1.0f);
		}
		comp.update(m, 8.0);
		CHECK(comp.hits_seen() == 5 + kHudDamageFeedSize);
		CHECK(comp.indicators().size() == kHudDamageIndicators);
	}

	TEST_CASE("hud screen unit: engine-drawn parts measured from the 18.57 footage") {
		CHECK(hud_bg_light_ramp_fraction_x() == doctest::Approx(1.0 / 21.0));
		CHECK(hud_bg_light_ramp_fraction_y() == doctest::Approx(1.0 / 5.25));
		const HudBgDrawLook &bg = kHudBgDrawLook;
		const std::array<float, 3> observed_ammo_offset{ 0.91f, 31.7f, 54.12f };
		const std::array<float, 3> observed_ammo_gain{ 0.8424f, 0.8527f, 0.8676f };
		CHECK(bg.premul_rgb255 == observed_ammo_offset);
		CHECK(1.0f - bg.coverage == doctest::Approx((observed_ammo_gain[0] + observed_ammo_gain[1] + observed_ammo_gain[2]) / 3.0f).epsilon(0.001));
		CHECK(bg.premul_rgb255[2] / 255.0f > bg.coverage);
		const float observed_light_start_of_fade = 0.36f;
		const float observed_light_full_of_fade = 1.05f;
		CHECK(bg.ramp_start_of_fade == observed_light_start_of_fade);
		CHECK(bg.ramp_full_of_fade == observed_light_full_of_fade);
		CHECK(hud_bg_ramp(bg, 0.0f) == 0.0f);
		CHECK(hud_bg_ramp(bg, 0.36f) == 0.0f);
		CHECK(hud_bg_ramp(bg, 0.705f) == doctest::Approx(0.5f));
		CHECK(hud_bg_ramp(bg, 1.05f) == 1.0f);
		CHECK(hud_bg_ramp(bg, 2.0f) == 1.0f);
		const HudEkgLook &k = kHudEkgLook;
		CHECK(k.rect_x0 == 1056.0f);
		CHECK(k.rect_y0 == 566.0f);
		CHECK(k.rect_x1 == 1166.0f);
		CHECK(k.rect_y1 == 592.0f);
		CHECK(k.scroll_px_per_second == 78.7f);
		CHECK(k.period_seconds == 1.04f);
		CHECK(k.baseline_y == 578.0f);
		CHECK(k.peak_px == 9.0f);
		CHECK(k.undershoot_px == 3.0f);
		CHECK(k.rgb == std::array<uint8_t, 3>{ 201, 221, 232 });
		CHECK(hud_ekg_beat_px(k, 0.0f) == 0.0f);
		CHECK(hud_ekg_beat_px(k, 5.0f) == doctest::Approx(9.0f));
		CHECK(hud_ekg_beat_px(k, 10.0f) == doctest::Approx(-3.0f));
		CHECK(hud_ekg_beat_px(k, 18.0f) == 0.0f);
		CHECK(hud_ekg_beat_px(k, 40.0f) == 0.0f);
		CHECK(hud_ekg_beat_px(k, -1.0f) == 0.0f);
		const double t_peak = 5.0 / 78.7;
		CHECK(hud_ekg_offset_px(k, t_peak, k.rect_x1) == doctest::Approx(9.0f).epsilon(1e-3));
		CHECK(hud_ekg_offset_px(k, t_peak + 1.0, k.rect_x1 - 78.7f) == doctest::Approx(9.0f).epsilon(1e-3));
		CHECK(hud_ekg_offset_px(k, t_peak + 1.04, k.rect_x1) == doctest::Approx(9.0f).epsilon(1e-3));
		CHECK(hud_ekg_offset_px(k, std::nan(""), 1100.0f) == 0.0f);
		CHECK(hud_ekg_alpha(k, 1050.0f) == 0.0f);
		CHECK(hud_ekg_alpha(k, 1102.5f) == doctest::Approx(0.5f));
		CHECK(hud_ekg_alpha(k, 1160.0f) == 1.0f);
		CHECK(kMinimapMetresPerLocalUnit == 1.375f);
		CHECK(kMinimapHatchTileLocal == 10.6f);
		CHECK(kMinimapSquareLocal == 170.0f);
		row("engine_parts", "iBgDraw over W: ammo 0.854*W+(0.9,31.7,54.1) from 3552 frames, light and coverage share one ramp per axis from 0.36 to 1.05 of the AS2 fade (1/21 x 1/5.25 of renderHolder), TDM light starts at y 451; EKG 78.7 px/s, 1.04 s, peak 9 px, undershoot 3 px; minimap 1.375 m/unit");
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
		CHECK(left == doctest::Approx(42.75f).epsilon(0.01));
		CHECK(right - s.at(w).matrix.tx == doctest::Approx(173.75f).epsilon(0.01));
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
		for (const char *w : { "PassangerList_01", "VehicleHealth_01", "SupportIconManager_01", "HudAlertMessage_01", "HudAlertMessage_02" }) {
			CHECK_MESSAGE(!s.at(std::string("instance1.") + w).visible, w);
		}
		for (const char *w : { "HudMessage_01", "HudMessage_02", "HudMessage_04", "HudMessage_05" }) {
			const std::string path = std::string("instance1.") + w;
			CHECK_MESSAGE(s.at(path).visible, w);
			CHECK_FALSE(s.at(path + ".posGuide").visible);
			const auto *api = dynamic_cast<HudMessageApi *>(s.hud->controller(path));
			REQUIRE(api != nullptr);
			CHECK(api->rows() == 0);
		}
		Screen t = initialized(*r, kHudTdmScreenPartition, m);
		CHECK_FALSE(t.at("instance1.SquadList_00").visible);
		CHECK(s.at("instance1.ScoreAggregator_01").visible);
		CHECK(s.at("instance1.ScoreAggregator_01.aggHolder").frame == s.label("instance1.ScoreAggregator_01.aggHolder", "hide"));
		CHECK(t.at("instance1.HudMessage_00").visible);
		REQUIRE(dynamic_cast<HudMessageApi *>(t.hud->controller("instance1.HudMessage_00")) != nullptr);
		CHECK(dynamic_cast<HudMessageApi *>(t.hud->controller("instance1.HudMessage_00"))->rows() == 0);
		CHECK_FALSE(t.at("instance1.MapmarkerManager_00").visible);
		row("hidden", "hit level 1 -> frame index 1, latency hidden, 7 data-driven widgets hidden until their data, 5 HudMessage lists shown empty, ScoreAggregator on hide");
	}

	TEST_CASE("hud screen: KillFeed") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		const HudMessageCompData data = message_data(*r);
		const HudMessageInfo &kill = data.of(HudMessageSource::Kill);
		CHECK(kill.queue_size == 4);
		CHECK(kill.normal_seconds == 5.0f);
		CHECK(kill.short_seconds == 1.0f);
		CHECK(kill.row_type == "Kill");
		CHECK(data.score_aggregate_time == 8.0f);
		const TextDatabase &db = text_db(*r);
		CHECK(db.find(kHudKillWeaponFormatHash) == kHudKillWeaponFormat);
		const uint32_t weapon = text_hash("ID_P_WNAME_M16A4");
		const std::optional<std::string_view> weapon_name = db.find(weapon);
		REQUIRE(weapon_name.has_value());
		HudMessageComp comp(data);
		HudClientState c = text_client(db);
		c.messages = &comp;
		HudModel m = kill_model();
		comp.update(m, c, 0.0);
		Screen s = initialized(*r, kHudMpScreenPartition, m, c);
		const std::string feed = "instance1.HudMessage_02";
		const HudWidgetNode *node = s.asset.find("HudMessage_02");
		REQUIRE(node != nullptr);
		REQUIRE(node->message_query.has_value());
		CHECK(node->message_query->key == 224794678);
		CHECK(node->number_of_rows == 2);
		const auto *api = dynamic_cast<HudMessageApi *>(s.hud->controller(feed));
		REQUIRE(api != nullptr);
		CHECK(api->source() == HudMessageSource::Kill);
		CHECK(api->max_row_count() == 2.0);
		CHECK(s.at(feed).visible);
		CHECK_FALSE(s.at(feed + ".posGuide").visible);
		CHECK(s.at(feed + ".messageHolder").dynamic);
		CHECK(api->rows() == 0);
		push_kill(m, 0, 1, weapon, true);
		comp.update(m, c, 0.0);
		s.hud->refresh(m);
		REQUIRE(api->rows() == 1);
		const std::string row1 = feed + ".messageHolder.message_1";
		const std::string c1 = row1 + ".container";
		CHECK(s.at(row1).symbol == "Kill");
		CHECK(s.at(row1).frame == s.label(row1, "startAnim"));
		CHECK(s.at(row1).playing);
		CHECK(s.at(row1).color.mul[3] == doctest::Approx(1.0f));
		CHECK(s.at(c1 + ".killer").frame == s.label(c1 + ".killer", "squad"));
		CHECK(s.at(c1 + ".victim").frame == s.label(c1 + ".victim", "enemy"));
		CHECK(s.at(c1 + ".msg").frame == s.label(c1 + ".msg", "neutral"));
		CHECK(s.at(c1 + ".killer.txt").text == "AAAA");
		CHECK(s.at(c1 + ".killer.txtBg").text == "AAAA");
		CHECK(s.at(c1 + ".victim.txt").text == "BBBBBB");
		const std::string weapon_text = "[" + std::string(*weapon_name) + "]";
		CHECK(s.at(c1 + ".msg.txt").text == weapon_text);
		CHECK(s.at(c1 + ".headShotIcon").visible);
		CHECK(s.at(c1 + ".headShotIcon").frame == s.label(c1 + ".headShotIcon", "KitRecon"));
		const As2Clip killer = s.clip(c1 + ".killer");
		const As2Clip msg = s.clip(c1 + ".msg");
		const As2Clip icon = s.clip(c1 + ".headShotIcon");
		const As2Clip victim = s.clip(c1 + ".victim");
		CHECK(killer.child("txt").width() == doctest::Approx(killer.child("txt").text_width() + 5.0f));
		CHECK(killer.child("txtBg").width() == doctest::Approx(killer.child("txt").text_width() + 5.0f));
		CHECK(msg.x() == doctest::Approx(killer.x() + killer.width() + 5.0f));
		CHECK(icon.x() == doctest::Approx(msg.x() + msg.width() + 10.0f));
		CHECK(icon.y() == 17.0f);
		CHECK(victim.x() == doctest::Approx(icon.x() + 10.0f));
		CHECK(s.clip(row1).x() == doctest::Approx(-s.clip(row1).width()));
		CHECK(tween_of(s, row1, HudTweenProperty::Y) == nullptr);
		CHECK(s.clip(row1).y() == 0.0f);
		const SwfTimeline &row_tl = *s.at(row1).timeline;
		const uint32_t start_label = s.label(row1, "startAnim");
		const uint32_t out_label = s.label(row1, "outAnim");
		const uint32_t started_after = frames_until(s, [&] { return s.at(row1).frame + 1 >= out_label || !s.at(row1).playing; }, 120);
		CHECK(started_after > 0);
		CHECK_FALSE(s.at(row1).playing);
		CHECK(s.at(row1).frame == start_label + started_after);
		CHECK(s.at(row1).frame < out_label);
		const float h1 = s.clip(row1 + ".heightClip").height();
		push_kill(m, 3, 2, weapon, false);
		comp.update(m, c, 1.0);
		s.hud->refresh(m);
		REQUIRE(api->rows() == 2);
		const std::string row2 = feed + ".messageHolder.message_2";
		const std::string c2 = row2 + ".container";
		CHECK(s.at(c2 + ".killer").frame == s.label(c2 + ".killer", "enemy"));
		CHECK(s.at(c2 + ".victim").frame == s.label(c2 + ".victim", "friendly"));
		CHECK_FALSE(s.at(c2 + ".headShotIcon").visible);
		const As2Clip msg2 = s.clip(c2 + ".msg");
		CHECK(s.clip(c2 + ".victim").x() == doctest::Approx(msg2.x() + msg2.width() + 5.0f));
		const HudTween *t1b = tween_of(s, row1, HudTweenProperty::Y);
		REQUIRE(t1b != nullptr);
		CHECK(t1b->seconds == 0.4);
		CHECK(t1b->ease == HudTweenEase::ExpoOut);
		CHECK(t1b->to == 0.0f);
		CHECK(tween_of(s, row2, HudTweenProperty::Y) == nullptr);
		CHECK(s.clip(row2).y() == doctest::Approx(h1));
		comp.update(m, c, 5.0);
		s.hud->refresh(m);
		CHECK(s.at(row1).frame == out_label);
		CHECK(s.at(row1).playing);
		const uint32_t removed_after = frames_until(s, [&] { return s.scene->find_node(row1) == nullptr; }, 120);
		CHECK(removed_after == row_tl.frames - 1 - out_label);
		CHECK(api->rows() == 1);
		const HudTween *t2b = tween_of(s, row2, HudTweenProperty::Y);
		REQUIRE(t2b != nullptr);
		CHECK(t2b->to == 0.0f);
		for (int i = 0; i < 5; ++i) {
			push_kill(m, 1, 2, weapon, false);
		}
		comp.update(m, c, 5.5);
		s.hud->refresh(m);
		CHECK(api->rows() == 6);
		CHECK(api->rows_added() == 7);
		CHECK(tween_of(s, feed + ".messageHolder.message_7", HudTweenProperty::Y) == nullptr);
		CHECK(s.clip(feed + ".messageHolder.message_7").y() == doctest::Approx(5.0f * h1));
		comp.update(m, c, 6.5);
		s.hud->refresh(m);
		CHECK(s.at(feed + ".messageHolder.message_2").frame == out_label);
		push_kill(m, 1, 1, 0, false, false, true);
		comp.update(m, c, 7.0);
		s.hud->refresh(m);
		const std::string sui = feed + ".messageHolder.message_" + std::to_string(comp.messages_added(HudMessageSource::Kill));
		CHECK(s.at(sui + ".container.victim").frame == s.label(sui + ".container.victim", "neutral"));
		CHECK(s.at(sui + ".container.victim.txt").text == db.find_sid(kHudKillsIsNoSid).value_or("?"));
		CHECK(s.at(sui + ".container.msg.txt").text == "BBBBBB");
		CHECK_FALSE(s.at(sui + ".container.killer").visible);
		CHECK(s.clip(sui + ".container.killer").width() == 0.0f);
		row("kill_feed", "UIMessageCompData Kill queue 4, 5 s, short 1 s; HudMessage_02 key 224794678 rows 2; row " + weapon_text + " headshot KitRecon, " +
						"started " + std::to_string(started_after) + " frames after startAnim " + std::to_string(start_label) + ", removed " +
						std::to_string(removed_after) + " frames after outAnim " + std::to_string(out_label) + " of " + std::to_string(row_tl.frames) + ", height " +
						std::to_string(h1));
	}

	TEST_CASE("hud screen: KillFeed team kill") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		const TextDatabase &db = text_db(*r);
		HudMessageComp comp(message_data(*r));
		HudClientState c = text_client(db);
		c.messages = &comp;
		HudModel m = kill_model();
		Screen s = initialized(*r, kHudMpScreenPartition, m, c);
		push_kill(m, 0, 2, text_hash("ID_P_WNAME_M16A4"), false, true);
		comp.update(m, c, 0.0);
		s.hud->refresh(m);
		const std::string g = "instance1.HudMessage_02.messageHolder.message_99999";
		CHECK(s.at(g).symbol == "Game");
		CHECK(s.at(g).matrix.ty == -240.0f);
		CHECK(s.at(g).frame == s.label(g, "in"));
		CHECK(s.at(g + ".txt.txtDisplay").text == db.find_sid("ID_H_ALERT_FAILED_FRIENDLY_FIRE").value_or("?"));
		CHECK(s.at("instance1.HudMessage_02.messageHolder.message_1.container.victim").frame ==
				s.label("instance1.HudMessage_02.messageHolder.message_1.container.victim", "friendly"));
		const uint32_t out_after = frames_until(s, [&] { return s.at(g).frame >= s.label(g, "out"); }, 200);
		const double dt = 1.0 / double(s.scene->frame_rate());
		double left = 5.0;
		uint32_t expected = 0;
		while (left > 0.0) {
			left -= dt;
			++expected;
		}
		CHECK(out_after == expected);
		row("kill_feed_team_kill", "Game row message_99999 at y -240, out after " + std::to_string(out_after) + " frames");
	}

	TEST_CASE("hud screen: KillLog") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		const HudMessageCompData data = message_data(*r);
		const HudMessageInfo &info = data.of(HudMessageSource::LocalKill);
		CHECK(info.queue_size == 2);
		CHECK(info.normal_seconds == 5.0f);
		CHECK(info.short_seconds == 1.0f);
		CHECK(info.row_type == "LocalKill");
		const TextDatabase &db = text_db(*r);
		const uint32_t weapon = text_hash("ID_P_WNAME_M16A4");
		const std::string weapon_text = "[" + std::string(db.find(weapon).value_or("?")) + "]";
		HudMessageComp comp(data);
		HudClientState c = text_client(db);
		c.messages = &comp;
		HudModel m = kill_model();
		Screen s = initialized(*r, kHudTdmScreenPartition, m, c);
		const std::string log = "instance1.HudMessage_00";
		const HudWidgetNode *node = s.asset.find("HudMessage_00");
		REQUIRE(node != nullptr);
		REQUIRE(node->message_query.has_value());
		CHECK(node->message_query->key == 1675248891);
		CHECK(node->number_of_rows == 2);
		const auto *api = dynamic_cast<HudMessageApi *>(s.hud->controller(log));
		REQUIRE(api != nullptr);
		CHECK(api->source() == HudMessageSource::LocalKill);
		push_local_kill(m, 0, 1, weapon, true);
		comp.update(m, c, 0.0);
		s.hud->refresh(m);
		REQUIRE(api->rows() == 1);
		const std::string row1 = log + ".messageHolder.message_1";
		const std::string c1 = row1 + ".container";
		CHECK(s.at(row1).symbol == "LocalKill");
		CHECK(s.at(row1).frame == s.label(row1, "startAnim"));
		CHECK(s.at(row1).color.mul[3] == doctest::Approx(1.0f));
		CHECK(s.at(c1 + ".killer.txt").text == "AAAA");
		CHECK(s.at(c1 + ".killer.txtBg").text == "AAAA");
		CHECK(s.at(c1 + ".victim.txt").text == "BBBBBB");
		CHECK(s.at(c1 + ".msg.txt").text == weapon_text);
		CHECK(s.at(c1 + ".msg").frame == s.label(c1 + ".msg", "neutral"));
		CHECK(s.at(c1 + ".headShotIcon").visible);
		CHECK(s.at(c1 + ".headShotIcon").frame == s.label(c1 + ".headShotIcon", "KitRecon"));
		const As2Clip killer = s.clip(c1 + ".killer");
		const As2Clip msg = s.clip(c1 + ".msg");
		const As2Clip icon = s.clip(c1 + ".headShotIcon");
		const As2Clip victim = s.clip(c1 + ".victim");
		CHECK(victim.child("txt").width() == doctest::Approx(victim.child("txt").text_width() + 5.0f));
		CHECK(msg.x() == doctest::Approx(killer.x() + killer.width() + 5.0f));
		CHECK(icon.x() == doctest::Approx(msg.x() + msg.width() + 10.0f));
		CHECK(icon.y() == 17.0f);
		CHECK(victim.x() == doctest::Approx(icon.x() + 10.0f));
		CHECK(s.clip(row1).x() == doctest::Approx(-(victim.x() + victim.width()) * 0.5f));
		CHECK(s.clip(row1).y() == 0.0f);
		const uint32_t out_label = s.label(row1, "outAnim");
		const uint32_t started_after = frames_until(s, [&] { return !s.at(row1).playing; }, 60);
		CHECK(s.at(row1).frame == s.label(row1, "startAnim") + started_after);
		const float h1 = s.clip(row1 + ".heightClip").height();
		push_local_kill(m, 0, 3, weapon, false);
		comp.update(m, c, 1.0);
		s.hud->refresh(m);
		REQUIRE(api->rows() == 2);
		const std::string c2 = log + ".messageHolder.message_2.container";
		CHECK_FALSE(s.at(c2 + ".headShotIcon").visible);
		CHECK(s.clip(c2 + ".victim").x() == doctest::Approx(s.clip(c2 + ".msg").x() + s.clip(c2 + ".msg").width() + 5.0f));
		CHECK(s.clip(log + ".messageHolder.message_2").y() == doctest::Approx(h1));
		comp.update(m, c, 5.0);
		s.hud->refresh(m);
		CHECK(s.at(row1).frame == out_label);
		const uint32_t removed_after = frames_until(s, [&] { return s.scene->find_node(row1) == nullptr; }, 60);
		CHECK(removed_after == s.at(log + ".messageHolder.message_2").timeline->frames - 1 - out_label);
		CHECK(api->rows() == 1);
		row("kill_log", "UIMessageCompData LocalKill queue 2, 5 s, short 1 s; hudtdmscreen HudMessage_00 key 1675248891 rows 2; row " + weapon_text +
						" centred at x " + std::to_string(s.clip(log + ".messageHolder.message_2").x()) + ", removed " + std::to_string(removed_after) +
						" frames after outAnim");
	}

	TEST_CASE("hud screen: ScorePopup") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		const HudMessageCompData data = message_data(*r);
		const HudMessageInfo &info = data.of(HudMessageSource::Score);
		CHECK(info.queue_size == 3);
		CHECK(info.normal_seconds == 5.0f);
		CHECK(info.short_seconds == 5.0f);
		CHECK(info.row_type == "Score");
		const TextDatabase &db = text_db(*r);
		CHECK(db.find_sid(kHudScoreKillSid) == "ENEMY KILLED {0:s}");
		CHECK(db.find_sid(kHudScoreHeadshotSid) == "HEADSHOT {0:s}");
		HudMessageComp comp(data);
		HudClientState c = text_client(db);
		c.messages = &comp;
		HudModel m = kill_model();
		Screen s = initialized(*r, kHudMpScreenPartition, m, c);
		const std::string pop = "instance1.HudMessage_01";
		const HudWidgetNode *node = s.asset.find("HudMessage_01");
		REQUIRE(node != nullptr);
		REQUIRE(node->message_query.has_value());
		CHECK(node->message_query->key == 1802462620);
		CHECK(node->number_of_rows == 2);
		const auto *api = dynamic_cast<HudMessageApi *>(s.hud->controller(pop));
		REQUIRE(api != nullptr);
		CHECK(api->source() == HudMessageSource::Score);
		push_score(m, text_hash(kHudScoreKillSid), 100);
		comp.update(m, c, 0.0);
		s.hud->refresh(m);
		REQUIRE(api->rows() == 1);
		const std::string row1 = pop + ".messageHolder.message_1";
		CHECK(s.at(row1).symbol == "Score");
		CHECK(s.at(row1).matrix.tx == 140.0f);
		CHECK(s.at(row1).frame == s.label(row1, "start"));
		CHECK(s.at(row1).playing);
		CHECK(s.at(row1 + ".scoringMessage.txt").text == "ENEMY KILLED 100");
		CHECK(s.at(row1 + ".scoringMessage.txtBg").text == "ENEMY KILLED 100");
		CHECK(s.at(row1 + ".cursor_mc").visible);
		CHECK(s.clip(row1).y() == 0.0f);
		const uint32_t loop = s.label(row1, "loop");
		const uint32_t end = s.label(row1, "end");
		const uint32_t looped = frames_until(s, [&] { return s.at(row1).frame == loop; }, 2 * end);
		CHECK(looped == loop - s.label(row1, "start"));
		const uint32_t back = frames_until(s, [&] { return s.at(row1).frame == loop; }, 2 * end);
		CHECK(back == end - 1 - loop);
		const float h1 = s.clip(row1 + ".heightClip").height();
		push_score(m, text_hash(kHudScoreHeadshotSid), 10);
		comp.update(m, c, 1.0);
		s.hud->refresh(m);
		REQUIRE(api->rows() == 2);
		const std::string row2 = pop + ".messageHolder.message_2";
		CHECK(s.at(row2 + ".scoringMessage.txt").text == "HEADSHOT 10");
		CHECK(s.at(row2 + ".cursor_mc").visible);
		CHECK_FALSE(s.at(row1 + ".cursor_mc").visible);
		CHECK(s.clip(row2).y() == 0.0f);
		const HudTween *t1 = tween_of(s, row1, HudTweenProperty::Y);
		REQUIRE(t1 != nullptr);
		CHECK(t1->seconds == 0.4);
		CHECK(t1->to == doctest::Approx(h1));
		comp.update(m, c, 5.0);
		s.hud->refresh(m);
		REQUIRE(s.scene->find_node(row1) != nullptr);
		const uint32_t left_loop = frames_until(s, [&] { return s.at(row1).frame >= end; }, 2 * end);
		CHECK(left_loop > 0);
		const uint32_t removed_after = frames_until(s, [&] { return s.scene->find_node(row1) == nullptr; }, 2 * end);
		CHECK(removed_after == s.at(row2).timeline->frames - 1 - end);
		CHECK(api->rows() == 1);
		row("score_popup", "UIMessageCompData Score queue 3, 5 s, short 5 s; HudMessage_01 key 1802462620 inverted; rows ENEMY KILLED 100, HEADSHOT 10 at x 140, "
						"loop " + std::to_string(loop) + ".." + std::to_string(end - 1) + ", removed " + std::to_string(removed_after) + " frames after end " +
						std::to_string(end) + ", row height " + std::to_string(h1));
	}

	TEST_CASE("hud screen: ScoreAggregator") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		const HudMessageCompData data = message_data(*r);
		HudMessageComp comp(data);
		HudClientState c = text_client(text_db(*r));
		c.messages = &comp;
		HudModel m = kill_model();
		Screen s = initialized(*r, kHudMpScreenPartition, m, c);
		const std::string agg = "instance1.ScoreAggregator_01";
		const HudWidgetNode *node = s.asset.find("ScoreAggregator_01");
		REQUIRE(node != nullptr);
		const HudDataSource *src = node->source("ScoreAgregateValue");
		REQUIRE(src != nullptr);
		CHECK(src->key == -29960869);
		CHECK_FALSE(src->on_initialize);
		CHECK(s.at(agg).visible);
		const std::string holder = agg + ".aggHolder";
		const uint32_t hide = s.label(holder, "hide");
		const uint32_t start = s.label(holder, "start");
		const uint32_t out = s.label(holder, "out");
		CHECK(s.at(holder).frame == hide);
		push_score(m, text_hash(kHudScoreKillSid), 100);
		comp.update(m, c, 0.0);
		s.hud->refresh(m);
		CHECK(s.at(holder + ".scoringMessage.txt").text == "100");
		CHECK(s.at(holder + ".scoringMessage.txtBg").text == "100");
		CHECK(s.at(holder).frame == hide);
		push_score(m, text_hash(kHudScoreKillSid), 100);
		push_score(m, text_hash(kHudScoreHeadshotSid), 10);
		comp.update(m, c, 2.0);
		s.hud->refresh(m);
		CHECK(s.at(holder + ".scoringMessage.txt").text == "210");
		CHECK(s.at(holder).frame == start);
		CHECK(s.at(holder).playing);
		const uint32_t ready_after = frames_until(s, [&] { return !s.at(holder).playing; }, 60);
		CHECK(s.at(holder).frame == out - 1);
		push_score(m, text_hash(kHudScoreKillSid), 100);
		comp.update(m, c, 3.0);
		s.hud->refresh(m);
		CHECK(s.at(holder + ".scoringMessage.txt").text == "310");
		CHECK(s.at(holder).frame == start);
		push_score(m, text_hash(kHudScoreKillSid), 100);
		comp.update(m, c, 3.1);
		s.hud->refresh(m);
		CHECK(s.at(holder + ".scoringMessage.txt").text == "410");
		CHECK(s.at(holder).frame == start);
		comp.update(m, c, 3.1 + double(data.score_aggregate_time));
		s.hud->refresh(m);
		CHECK(s.at(holder).frame == out);
		CHECK(s.at(holder + ".scoringMessage.txt").text == "410");
		row("score_aggregator", "ScoreAgregateValue key -29960869 not on initialize; first score only sets the text, the next plays start " + std::to_string(start) +
						", ready again " + std::to_string(ready_after) + " frames later, 0 after " + std::to_string(data.score_aggregate_time) + " s plays out " +
						std::to_string(out));
	}

	TEST_CASE("hud screen: DamageIndicator") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		Result<HudDamageIndicatorCompData> data = read_hud_damage_indicator_comp(*r->level);
		REQUIRE_MESSAGE(data.ok(), (data.ok() ? std::string() : data.error().message));
		CHECK(data->max_update_time == 5.0f);
		CHECK(data->updates_per_second == 30);
		HudDamageIndicatorComp comp(*data);
		HudClientState c = test_client();
		c.damage = &comp;
		HudModel m = moment_m02();
		m.soldier.compass_angle = 0.0f;
		Screen s = initialized(*r, kHudSharedScreenPartition, m, c);
		const HudWidgetNode *node = s.asset.find("DamageIndicator_01");
		REQUIRE(node != nullptr);
		const HudDataSource *src = node->source("Indicator");
		REQUIRE(src != nullptr);
		CHECK(src->key == -1063919953);
		const std::string di = "instance1.DamageIndicator_01";
		CHECK(s.at(di).visible);
		const std::string ind = di + ".indicator0";
		REQUIRE(s.at(ind).timeline != nullptr);
		CHECK(s.at(ind).timeline->frames == 361);
		const std::string arrow = ind + ".arrow";
		const uint32_t full_hit = 2 - 1;
		const uint32_t minor_hit = 31 - 1;
		const uint32_t idle = s.at(arrow).frame;
		push_damage(m, 4, 270.0f, 60.0f);
		comp.update(m, 0.0);
		s.hud->refresh(m);
		CHECK(s.at(ind).frame == 90);
		CHECK_FALSE(s.at(ind).playing);
		CHECK(s.at(arrow).frame == full_hit);
		CHECK(s.at(arrow).playing);
		const SwfMatrix right = s.at(arrow).matrix;
		s.clip(ind).goto_and_stop(1);
		const SwfMatrix up = s.at(ind + ".arrow").matrix;
		s.clip(ind).goto_and_stop(91);
		CHECK(up.b == doctest::Approx(0.0f).epsilon(0.01));
		CHECK(up.a > 0.99f);
		CHECK(up.ty == doctest::Approx(-100.0f));
		CHECK(right.b > 0.99f);
		CHECK(right.tx == doctest::Approx(99.95f));
		CHECK(right.a == doctest::Approx(0.0f).scale(1.0f).epsilon(0.02));
		push_damage(m, 5, 180.0f, 20.0f);
		comp.update(m, 0.5);
		s.hud->refresh(m);
		const std::string ind1 = di + ".indicator1";
		CHECK(s.at(ind).frame == 180);
		CHECK(s.at(ind1).frame == 90);
		CHECK(s.at(ind + ".arrow").frame == minor_hit);
		row("damage_indicator", "UIDamageIndicatorCompData MaxUpdateTime " + std::to_string(data->max_update_time) + " s, " + std::to_string(data->updates_per_second) +
						" Hz; key -1063919953 on hudscreen DamageIndicator_01; bearing 270 at heading 0 -> frame 91 (arrow matrix b " + std::to_string(right.b) +
						", clockwise), Scale 60 -> fullHit, 20 -> minorHit, idle arrow frame " + std::to_string(idle));
	}

	TEST_CASE("hud screen: HudAlertMessage") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		const TextDatabase &db = text_db(*r);
		HudModel m = moment_m02();
		Screen s = initialized(*r, kHudMpScreenPartition, m, text_client(db));
		const std::string al = "instance1.HudAlertMessage_01";
		CHECK_FALSE(s.at(al).visible);
		drive(s, al, "WarningVisibility", As2Value::of(true));
		const std::string warn = al + ".warning";
		CHECK(s.at(al).visible);
		CHECK(s.at(warn).visible);
		CHECK(s.at(warn).frame == s.label(warn, "in"));
		const std::string title = std::string(db.find_sid("ID_H_ALERT_COMBAT_AREA_WARNING").value_or(""));
		CHECK_FALSE(title.empty());
		CHECK(s.at(warn + ".content.title").text == title);
		CHECK(s.at(warn + ".content.description").text == std::string(db.find_sid("ID_H_ALERT_COMBAT_AREA_WARNING_DESCRIPTION").value_or("")));
		const uint32_t loop = s.label(warn, "loop");
		const uint32_t shown_after = frames_until(s, [&] { return s.at(warn).frame == loop; }, 200);
		CHECK(shown_after > 0);
		drive(s, al, "Timer", As2Value::of("00:07"));
		CHECK(s.at(warn + ".content.timer").text == "00:07");
		drive(s, al, "WarningVisibility", As2Value::of(false));
		CHECK(s.at(warn).frame == s.label(warn, "out"));
		const uint32_t hidden_after = frames_until(s, [&] { return !s.at(al).visible; }, 200);
		CHECK(hidden_after > 0);
		CHECK_FALSE(s.at(warn).visible);
		const std::string info_widget = "instance1.HudAlertMessage_02";
		drive(s, info_widget, "SquadLeaderVisibility", As2Value::of(true));
		const std::string info = info_widget + ".information";
		CHECK(s.at(info_widget).visible);
		CHECK(s.at(info).visible);
		CHECK(s.at(info + ".content.title").text == std::string(db.find_sid("ID_H_ALERT_SQUAD_LEADER_PROMOTION").value_or("")));
		CHECK_FALSE(s.at(info + ".bg").visible);
		const double fps = double(s.scene->frame_rate());
		const uint32_t info_out = frames_until(s, [&] { return s.at(info).frame == s.label(info, "out"); }, uint32_t(fps * 6.0));
		CHECK(double(info_out) / fps == doctest::Approx(5.0).epsilon(0.05));
		row("hud_alert", "WarningVisibility shows warning with the ID_H_ALERT_COMBAT_AREA_WARNING texts, loop after " + std::to_string(shown_after) +
						" frames, hidden " + std::to_string(hidden_after) + " frames after out; SquadLeaderVisibility information out after " +
						std::to_string(info_out) + " frames");
	}

	TEST_CASE("hud screen: the hudscreen panels") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		HudDamageIndicatorComp comp(HudDamageIndicatorCompData{ 5.0f, 30 });
		HudClientState c = test_client();
		c.damage = &comp;
		HudModel m = moment_m02();
		Screen s = initialized(*r, kHudSharedScreenPartition, m, c);
		CHECK(s.asset.movie == "ui/assets/hudscreen");
		CHECK(s.hud->stats().unknown_classes == 0);
		for (const std::string &u : s.hud->stats().unknown_class_paths) {
			CHECK_MESSAGE(false, u);
		}
		const std::string log = "instance1.LogMessage_01";
		const std::string tip = "instance1.TooltipMessage_01";
		const std::string obj = "instance1.ObjectiveMessage_01";
		CHECK(s.has_tilt(log, -5.0f, -15.0f));
		CHECK(s.has_tilt(tip, -5.0f, 15.0f));
		CHECK(s.has_tilt(obj, 5.0f, -15.0f));
		CHECK(s.renderer(log) == "t2");
		CHECK(s.renderer(tip) == "t2");
		CHECK(s.renderer(obj) == "t2");
		CHECK_FALSE(s.at(log).visible);
		CHECK_FALSE(s.at(tip).visible);
		CHECK(s.at(obj).visible);
		CHECK_FALSE(s.at(log + ".container.bg").visible);
		CHECK_FALSE(s.at(tip + ".container.bg").visible);
		CHECK_FALSE(s.at(obj + ".header.bg").visible);
		CHECK(s.scene->find_node(log + ".container.bg.Background.bgHolder") == nullptr);
		CHECK(s.scene->find_node("instance1.AlertManager_01.Alert") == nullptr);
		for (int i = 1; i <= 4; ++i) {
			CHECK_FALSE(s.at(tip + ".container.textTop.textClip.t" + std::to_string(i)).visible);
			CHECK_FALSE(s.at(tip + ".container.textBottom.textClip.t" + std::to_string(i)).visible);
		}
		CHECK_FALSE(s.at("instance1.AlertManager_01").visible);
		CHECK(s.at("instance1.InteractionManager_01").visible);
		CHECK(s.at("instance1.DamageIndicator_01").visible);
		row("shared_screen", "hudscreen " + std::to_string(s.hud->stats().instances) + " instances, 0 unknown classes; Log/Tooltip/Objective tilts -5/-15, -5/15, "
						"5/-15 at fov 25 on t2, Objective shown, AlertManager hidden, InteractionManager shown");
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
