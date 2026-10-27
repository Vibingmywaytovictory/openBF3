#include <doctest/doctest.h>

#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/kit_def.h"
#include "bf3/core/install.h"
#include "bf3/core/install_snapshot.h"
#include "bf3/core/ui/as2_clip.h"
#include "bf3/core/ui/hud_messages.h"
#include "bf3/core/ui/hud_screen.h"
#include "bf3/core/ui/hud_widget_registry.h"
#include "bf3/core/ui/hud_widgets.h"
#include "bf3/core/ui/swf_hit.h"
#include "bf3/core/ui/swf_library.h"
#include "bf3/core/ui/swf_scene.h"
#include "bf3/core/ui/swf_text.h"
#include "bf3/core/ui/text_db.h"
#include "bf3/core/ui/ui_direct_access.h"
#include "bf3/core/ui/ui_fonts.h"
#include "test_support.h"
#include "ui_retail_support.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace bf3::core;
using namespace bf3::core::ui;
using ui_retail::Retail;
using ui_retail::retail_or_skip;

namespace {

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

HudClientState text_client(const TextDatabase &p_db) {
	return ui_retail::retail_text_client(p_db, test_client());
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

float timeline_alpha(const SwfSceneNode &p_row, std::string_view p_child) {
	REQUIRE(p_row.timeline != nullptr);
	std::vector<std::vector<SwfDisplayEntry>> lists;
	const uint32_t frame = p_row.frame;
	WorkBudget b(1u << 20);
	REQUIRE(display_lists_at(*p_row.timeline, std::span<const uint32_t>(&frame, 1), b, lists).ok());
	for (const SwfDisplayEntry &e : lists[0]) {
		if (e.name == p_child) {
			return swf_color_xform(e.color).mul[3];
		}
	}
	const std::string missing = std::string(p_child) + " not on the timeline at frame " + std::to_string(frame);
	FAIL_CHECK(missing);
	return -1.0f;
}

struct AlphaTrace {
	std::vector<float> drawn;
	std::vector<float> timeline;
};

void trace_alpha(const Screen &p_s, const std::string &p_row, std::string_view p_child, AlphaTrace &r_t) {
	p_s.scene->take_changes();
	const SwfSceneNode &row = p_s.at(p_row);
	r_t.drawn.push_back(p_s.at(p_row + "." + std::string(p_child)).world_color.mul[3]);
	r_t.timeline.push_back(row.color.mul[3] * timeline_alpha(row, p_child));
}

void check_trace(const AlphaTrace &p_t, std::span<const float> p_want) {
	REQUIRE(p_t.drawn.size() == p_want.size());
	for (size_t i = 0; i < p_want.size(); ++i) {
		CHECK(p_t.drawn[i] == doctest::Approx(p_want[i]));
		CHECK(p_t.timeline[i] == doctest::Approx(p_want[i]));
	}
}

void check_trace_follows_timeline(const AlphaTrace &p_t) {
	REQUIRE(p_t.drawn.size() == p_t.timeline.size());
	for (size_t i = 0; i < p_t.drawn.size(); ++i) {
		CHECK(p_t.drawn[i] == doctest::Approx(p_t.timeline[i]));
	}
}

std::string trace_text(const AlphaTrace &p_t) {
	std::string s;
	char buf[16];
	for (const float a : p_t.drawn) {
		std::snprintf(buf, sizeof(buf), "%.3f", double(a));
		s += (s.empty() ? "" : " ") + std::string(buf);
	}
	return s;
}

constexpr std::array<float, 4> kKillRowFadeIn{ 64.0f / 256.0f, 128.0f / 256.0f, 192.0f / 256.0f, 1.0f };
constexpr std::array<float, 5> kKillRowFadeOut{ 1.0f, 205.0f / 256.0f, 154.0f / 256.0f, 102.0f / 256.0f, 51.0f / 256.0f };

}

TEST_SUITE("ui-hud-screen-unit") {
	TEST_CASE("hud screen unit: each AS2 class registers its controller once from its widget file") {
		const std::vector<std::string> want{ "Screen.Weapon.CrosshairDefault", "Shared.HudBackground",
			"Widget.Button.Button", "Widget.Button.Buttons.ButtonBase", "Widget.Button.Buttons.IconButton",
			"Widget.Button.Buttons.LargeArrowButton", "Widget.Button.Buttons.ResizableButton",
			"Widget.Button.Buttons.TextButton", "Widget.Button.Buttons.TextIconButton",
			"Widget.Button.Buttons.TextIconButtonAlignedRight", "Widget.Crosshair.Crosshair",
			"Widget.Crosshair.Hitindicator", "Widget.DogTag.DogTag", "Widget.DogTag.DogTagPair",
			"Widget.DogTag.DogTagText", "Widget.Hud.Ammo", "Widget.Hud.Compass", "Widget.Hud.DamageIndicator",
			"Widget.Hud.Health", "Widget.Hud.HudBackgroundWidget", "Widget.Hud.Minimap", "Widget.Hud.PassangerList",
			"Widget.Hud.SquadList", "Widget.Hud.VehicleHealth", "Widget.Icon3D.Alerts.AlertManager",
			"Widget.Icon3D.Alerts.Alerttag", "Widget.Icon3D.Interaction.InteractionManager",
			"Widget.Icon3D.Mapmarker.MapmarkerManager", "Widget.Icon3D.Mapmarker.Mapmarkertag",
			"Widget.Icon3D.TeamSupport.SupportIconManager", "Widget.Icon3D.TeamSupport.SupportIconTag",
			"Widget.Image.ImageManager", "Widget.Message.HudAlertMessage", "Widget.Message.HudMessage",
			"Widget.Message.LogMessage", "Widget.Message.MessageTypes.GameMessage",
			"Widget.Message.MessageTypes.KillLogMessage", "Widget.Message.MessageTypes.LocalKillLogMessage",
			"Widget.Message.MessageTypes.ScoreMessage", "Widget.Message.ObjectiveMessage",
			"Widget.Message.ScoreAggregator", "Widget.Message.TooltipMessage", "Widget.Misc.LatencyIndicator",
			"Widget.ScreenSpecific.KillCam", "Widget.TicketCounter.TicketCounter", "fb.Base.UIScreen",
			"Util.ScrollText", "Util.SimpleScrollText", "Util.SimpleTextBlockScroll", "Shared.KitIcons",
			"Shared.ServiceStars", "Graphics.GlowRect", "Graphics.PopupBox", "Widget.Bar.ScrollBar", "Widget.List.List",
			"Widget.List.Rows.RowBase", "Widget.List.Rows.ScoreBoardRow", "Widget.List.ScoreboardHeader",
			"Widget.ScreenSpecific.ScoreboardSpectatorBox", "Widget.Button.TabBar", "fb.Base.UIPopup",
			"Widget.Popup.PopupGenericWidget", "Widget.Popup.PopupButtonsManager", "Widget.Image.Dimmer",
			"Widget.List.ListLabel", "Widget.Text.TextField", "Widget.Text.PageHeader", "Widget.Input.InputListener",
			"Widget.Button.ConsoleButtonBar", "Screen.LoadingScreen", "Widget.Message.LoadingMessage",
			"Widget.SpawnTimer.SpawnTimer", "Widget.TicketCounter.TicketCounterSpawnScreen", "Widget.Image.OverviewMap",
			"Widget.ScreenSpecific.SpawnPointsHeader", "Widget.List.Rows.TeamSpawn", "Widget.List.Rows.BaseSpawn",
			"Widget.List.Rows.SpawnEmpty", "Widget.Bar.KitBar", "Widget.Bar.AdvancedBar" };
		const HudWidgetRegistry &installed = HudWidgetRegistry::installed();
		std::vector<std::string> sorted = want;
		std::sort(sorted.begin(), sorted.end());
		CHECK(installed.classes() == sorted);
		CHECK(installed.rejected().empty());
		for (const std::string &c : want) {
			CHECK_MESSAGE(make_hud_widget_controller(c) != nullptr, c);
			CHECK_MESSAGE(hud_widget_class_known(c), c);
		}
		CHECK(make_hud_widget_controller("Widget.NoSuch.Class") == nullptr);
		CHECK_FALSE(hud_widget_class_known("Widget.NoSuch.Class"));
		HudWidgetRegistry local;
		CHECK(local.add("Test.Class", [] { return make_hud_widget_controller("fb.Base.UIScreen"); }));
		CHECK_FALSE(local.add("Test.Class", [] { return make_hud_widget_controller("fb.Base.UIScreen"); }));
		CHECK_FALSE(local.add("", [] { return make_hud_widget_controller("fb.Base.UIScreen"); }));
		CHECK_FALSE(local.add("Test.Empty", HudWidgetFactory{}));
		CHECK(local.rejected() == std::vector<std::string>{ "Test.Class", "", "Test.Empty" });
		CHECK(local.make("Test.Class") != nullptr);
		row("registry", std::to_string(installed.classes().size()) + " AS2 classes, each registered once");
	}

	TEST_CASE("hud screen unit: DirectAccess registerMC types 13 array item, 14 member and 15 array-length invoke") {
		struct Log : UiDirectAccessTarget {
			std::vector<std::string> lines;
			UiDirectAccess *da = nullptr;
			void direct_access(const UiDirectAccessApply &p_apply) override {
				lines.push_back(p_apply.clip + " " + ui_direct_access_op_name(p_apply.op) + " " + p_apply.name + "=" +
						p_apply.value.to_string());
				if (p_apply.op == UiDirectAccessOp::ArrayLengthInvoke && da != nullptr) {
					for (int32_t i = 0; i < int32_t(p_apply.value.to_number()); ++i) {
						const std::string row = "list.row" + std::to_string(i);
						da->register_mc(row + ".label",
								{ As2Value::of(13), As2Value::of("rows"), As2Value::of(0), As2Value::of(i),
										As2Value::of("Label") });
						da->register_mc(row,
								{ As2Value::of(13), As2Value::of("rows"), As2Value::of(14), As2Value::of(i),
										As2Value::of("Index"), As2Value::of("ID") });
					}
				}
			}
		};
		Log log;
		UiDirectAccess da(log);
		log.da = &da;
		const auto row_of = [](const char *p_label, int32_t p_index) {
			As2Value r = As2Value::object();
			r.set("Label", As2Value::of(p_label));
			r.set("Index", As2Value::of(p_index));
			return r;
		};
		REQUIRE(da.register_mc("list", { As2Value::of(15), As2Value::of("rows"), As2Value::of("updateList") })
						.has_value());
		REQUIRE(da.register_mc(
						  "count", { As2Value::of(14), As2Value::of("rows"), As2Value::of("size"), As2Value::of(true) })
						.has_value());
		const std::optional<uint32_t> title = da.register_mc("title", { As2Value::of(0), As2Value::of("name") });
		REQUIRE(title.has_value());
		REQUIRE(da.register_mc("title", { As2Value::of(12), As2Value::of("name"), As2Value::of("setText") })
						.has_value());
		CHECK(log.lines.empty());
		da.set_array("rows", { row_of("ALPHA", 0), row_of("BRAVO", 1) });
		const std::vector<std::string> first{ "list REGISTER_ARRAYLENGTHINVOKE updateList=2",
			"list.row0.label REGISTER_TEXT =ALPHA", "list.row0 REGISTER_MEMBER ID=0",
			"list.row1.label REGISTER_TEXT =BRAVO", "list.row1 REGISTER_MEMBER ID=1", "count REGISTER_MEMBER size=2" };
		CHECK(log.lines == first);
		log.lines.clear();
		da.set_array("rows", { row_of("ALPHA", 0), row_of("CHARLIE", 1) });
		CHECK(log.lines == std::vector<std::string>{ "list.row1.label REGISTER_TEXT =CHARLIE" });
		log.lines.clear();
		da.set_value("name", As2Value::of("US"));
		CHECK(log.lines == std::vector<std::string>{ "title REGISTER_TEXT =US", "title REGISTER_INVOKE setText=US" });
		log.lines.clear();
		da.set_value("name", As2Value::of("US"));
		CHECK(log.lines.empty());
		CHECK(da.unregister_mc(*title));
		CHECK_FALSE(da.unregister_mc(*title));
		da.set_value("name", As2Value::of("RU"));
		CHECK(log.lines == std::vector<std::string>{ "title REGISTER_INVOKE setText=RU" });
		log.lines.clear();
		REQUIRE(da.register_mc("late", { As2Value::of(3), As2Value::of("name") }).has_value());
		CHECK(log.lines == std::vector<std::string>{ "late REGISTER_VISIBLE =RU" });
		CHECK(da.unregister_under("list") == 5);
		log.lines.clear();
		da.set_array("rows", { row_of("DELTA", 0) });
		CHECK(log.lines == std::vector<std::string>{ "count REGISTER_MEMBER size=1" });
		CHECK(da.errors().empty());
		CHECK_FALSE(da.register_mc("bad", {}).has_value());
		CHECK_FALSE(da.register_mc("bad", { As2Value::of(17), As2Value::of("name") }).has_value());
		CHECK_FALSE(da.register_mc("bad", { As2Value::of(0) }).has_value());
		CHECK_FALSE(da.register_mc("bad",
							  { As2Value::of(13), As2Value::of("rows"), As2Value::of(13), As2Value::of(0),
									  As2Value::of("Label") })
						.has_value());
		CHECK(da.errors().size() == 4);
		const std::optional<uint32_t> text_first = da.register_mc("swapped",
				{ As2Value::of(13), As2Value::of(0), As2Value::of("rows"), As2Value::of(0), As2Value::of("Label") });
		REQUIRE(text_first.has_value());
		CHECK(log.lines.back() == "swapped REGISTER_TEXT =DELTA");
		row("direct_access",
				std::to_string(da.applied()) +
						" direct-access applies over arrays, members, invokes and length invokes");
	}

	TEST_CASE("hud screen unit: Ammo, Health and Minimap bind Visible false while the kill card, spawn screen, end of "
			  "round or the menu is up") {
		const std::array<const char *, 3> classes{ "Widget.Hud.Ammo", "Widget.Hud.Health", "Widget.Hud.Minimap" };
		const auto visible = [](const HudWidgetController &p_ctl, const HudModel &p_model,
									 const HudClientState &p_client) {
			HudData bound;
			p_ctl.bind(p_model, p_client, bound);
			const As2Value *v = hud_data_find(bound, "Visible");
			REQUIRE(v != nullptr);
			return v->truthy();
		};
		size_t hidden = 0;
		for (const char *c : classes) {
			const std::unique_ptr<HudWidgetController> ctl = make_hud_widget_controller(c);
			REQUIRE_MESSAGE(ctl != nullptr, c);
			const HudModel alive = moment_m02();
			const HudClientState client = test_client();
			CHECK_MESSAGE(visible(*ctl, alive, client), c);
			HudModel m = alive;
			m.user.is_in_menu = true;
			hidden += visible(*ctl, m, client) ? 0u : 1u;
			m = alive;
			m.kill_card.visible = true;
			hidden += visible(*ctl, m, client) ? 0u : 1u;
			m = alive;
			m.spawn_screen.visible = true;
			hidden += visible(*ctl, m, client) ? 0u : 1u;
			m = alive;
			m.end_of_round.visible = true;
			hidden += visible(*ctl, m, client) ? 0u : 1u;
			HudClientState hardcore = client;
			hardcore.hardcore = true;
			hidden += visible(*ctl, alive, hardcore) ? 0u : 1u;
		}
		CHECK(hidden == classes.size() * 5);
		row("soldier_visible",
				std::to_string(hidden) +
						" of 15 hidden binds: menu, kill card, spawn screen, end of round, hardcore on 3 widgets");
	}

	TEST_CASE("hud screen unit: the compass frame is the heading rounded up, as the footage's minimap and compass show") {
		struct StillMoment {
			const char *id;
			float map_rotation_deg;
			int32_t cps_frame;
		};
		constexpr std::array<StillMoment, 10> kStill{ {
				{ "m03", 91.5f, 270 },
				{ "m04", 312.5f, 49 },
				{ "m05", 279.5f, 82 },
				{ "m07", 72.25f, 289 },
				{ "m08", 76.5f, 286 },
				{ "m09", 77.0f, 285 },
				{ "m10", 275.0f, 87 },
				{ "m11", 293.0f, 69 },
				{ "m12", 59.5f, 302 },
				{ "m14", 168.5f, 192 },
		} };
		double sum = 0.0;
		double sq = 0.0;
		for (const StillMoment &m : kStill) {
			const double offset = 360.0 - double(m.map_rotation_deg) - double(m.cps_frame - 1);
			sum += offset;
			sq += offset * offset;
			CHECK(hud_compass_frame(float(m.cps_frame - 1) - 0.5f) == m.cps_frame - 1);
		}
		const double n = double(kStill.size());
		const double mean = sum / n;
		const double sem = std::sqrt((sq - n * mean * mean) / (n - 1.0) / n);
		CHECK(mean == doctest::Approx(-0.625));
		CHECK(std::fabs(mean + 0.5) < sem);
		CHECK(std::fabs(mean) > 3.0 * sem);
		row("compass_frame_rule", "minimap heading - compass frame over " + std::to_string(kStill.size()) + " still moments mean " + std::to_string(mean) + ", standard error " +
				std::to_string(sem) + " (heading rounded up -0.5, rounded to nearest 0)");
	}

	TEST_CASE("hud screen unit: compass frame, crosshair dispersion, AS2 values, alignment and easing") {
		CHECK(hud_compass_frame(0.0f) == 0);
		CHECK(hud_compass_frame(212.0f) == 212);
		CHECK(hud_compass_frame(359.6f) == 0);
		CHECK(hud_compass_frame(-1.0f) == 359);
		CHECK(hud_compass_frame(std::nanf("")) == 0);
		CHECK(hud_compass_frame(211.5f) == 212);
		CHECK(hud_compass_frame(211.01f) == 212);
		CHECK(hud_compass_frame(-0.5f) == 0);
		CHECK(hud_compass_frame(1e7f) == 280);
		CHECK(hud_crosshair_dispersion(0.0f) == 0.0);
		CHECK(hud_crosshair_dispersion(std::nanf("")) == 0.0);
		CHECK(hud_crosshair_dispersion(-1.0f) == 0.0);
		CHECK(hud_crosshair_dispersion(3.5f) == doctest::Approx(0.7));
		CHECK(hud_crosshair_arm(1.5f, 0.151f) == 45.0);
		CHECK(hud_crosshair_arm(3.0f, 0.151f) == 75.0);
		CHECK(hud_crosshair_arm(3.5f, 0.151f) == 85.0);
		CHECK(hud_crosshair_arm(3.6f, 0.151f) == 87.0);
		CHECK(hud_crosshair_arm(4.2f, 0.151f) == 99.0);
		CHECK(hud_crosshair_arm(5.0f, 0.151f) == 115.0);
		CHECK(hud_crosshair_arm(7.0f, 0.151f) == 155.0);
		CHECK(hud_crosshair_arm(0.0f, 0.151f) == 15.0);
		CHECK(hud_crosshair_arm(hud_crosshair_spread_for_arm(44.125, 0.151f), 0.151f) == 44.0);
		CHECK(hud_crosshair_spread_for_arm(4.25, 0.151f) == 0.0f);
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
		CHECK(hud_damage_angle_frame(0.0f, 270.0f) == 271);
		CHECK(hud_damage_angle_frame(0.0f, 90.0f) == 91);
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
		CHECK(comp.indicators().at(0).field("Angle").to_number() == 271.0);
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
		CHECK(comp.indicators().at(0).field("Angle").to_number() == 271.0);
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
		const std::array<std::array<float, 2>, 6> observed_coverage_edges{ { { 0.087f, 0.812f }, { 0.193f, 0.603f }, { 0.247f, 0.633f }, { 0.311f, 0.782f }, { 0.518f, 0.717f }, { 0.205f, 0.855f } } };
		const std::array<std::array<float, 2>, 6> observed_light_edges{ { { 0.195f, 0.965f }, { 0.296f, 1.008f }, { 0.273f, 0.988f }, { 0.272f, 1.283f }, { 0.393f, 1.104f }, { 0.210f, 0.953f } } };
		const auto mean_of = [](const std::array<std::array<float, 2>, 6> &p_edges, size_t p_i) {
			float s = 0.0f;
			for (const auto &e : p_edges) {
				s += e[p_i];
			}
			return s / float(p_edges.size());
		};
		CHECK(bg.coverage_start_of_fade == doctest::Approx(mean_of(observed_coverage_edges, 0)).epsilon(0.01));
		CHECK(bg.coverage_full_of_fade == doctest::Approx(mean_of(observed_coverage_edges, 1)).epsilon(0.01));
		CHECK(bg.light_start_of_fade == doctest::Approx(mean_of(observed_light_edges, 0)).epsilon(0.02));
		CHECK(bg.light_full_of_fade == doctest::Approx(mean_of(observed_light_edges, 1)).epsilon(0.01));
		CHECK((bg.coverage_start_of_fade + bg.coverage_full_of_fade) / 2.0f == doctest::Approx(0.5f).epsilon(0.01));
		CHECK(bg.coverage_full_of_fade < bg.light_full_of_fade);
		CHECK(hud_bg_coverage_ramp(bg, 0.0f) == 0.0f);
		CHECK(hud_bg_coverage_ramp(bg, 0.26f) == 0.0f);
		CHECK(hud_bg_coverage_ramp(bg, 0.495f) == doctest::Approx(0.5f));
		CHECK(hud_bg_coverage_ramp(bg, 0.73f) == 1.0f);
		CHECK(hud_bg_light_ramp(bg, 0.27f) == 0.0f);
		CHECK(hud_bg_light_ramp(bg, 0.66f) == doctest::Approx(0.5f));
		CHECK(hud_bg_light_ramp(bg, 1.05f) == 1.0f);
		CHECK(hud_bg_light_ramp(bg, 2.0f) == 1.0f);
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
		CHECK(kMinimapSquareLocal == 170.0f);
		struct HatchPhase {
			const char *id;
			float map_anchored;
			float player_anchored;
		};
		constexpr std::array<HatchPhase, 16> kHatchPhase{ {
				{ "m01", 0.754f, 0.741f },
				{ "m02", 0.789f, 0.230f },
				{ "m03", 0.471f, 0.348f },
				{ "m04", 0.684f, -0.621f },
				{ "m05", 0.573f, -0.403f },
				{ "m06", 0.227f, 0.370f },
				{ "m07", 0.431f, -0.430f },
				{ "m08", 0.435f, 0.470f },
				{ "m09", 0.462f, -0.184f },
				{ "m10", 0.334f, -0.093f },
				{ "m11", 0.402f, -0.277f },
				{ "m12", 0.482f, 0.229f },
				{ "m13", 0.635f, -0.546f },
				{ "m14", 0.666f, -0.663f },
				{ "m15", 0.157f, 0.130f },
				{ "m16", 0.610f, 0.495f },
		} };
		int32_t map_wins = 0;
		double map_sum = 0.0;
		double player_sum = 0.0;
		for (const HatchPhase &h : kHatchPhase) {
			map_wins += h.map_anchored > h.player_anchored ? 1 : 0;
			map_sum += h.map_anchored;
			player_sum += h.player_anchored;
		}
		CHECK(map_wins == 14);
		CHECK(map_sum / 16.0 == doctest::Approx(0.507).epsilon(0.002));
		CHECK(player_sum / 16.0 == doctest::Approx(-0.01275));
		const std::array<float, 5> observed_satellite_weight{ -0.058f, -0.665f, -0.331f, -0.531f, -0.354f };
		for (const float w : observed_satellite_weight) {
			CHECK(w <= kMinimapSatelliteWeight);
		}
		CHECK(kMinimapSatelliteWeight == 0.0f);
		const auto video_y = [](std::array<float, 3> p_rgb) { return 16.0f + (0.2126f * p_rgb[0] + 0.7152f * p_rgb[1] + 0.0722f * p_rgb[2]) * 219.0f / 255.0f; };
		const std::array<float, 3> tint{ 0.3f * 255.0f, 0.6f * 255.0f, 0.8f * 255.0f };
		const std::array<float, 3> detail{ 0.9f * 255.0f, 255.0f, 255.0f };
		const float opacity = 0.5f;
		const float detail_alpha = 0.3f;
		const std::array<float, 3> dark{ tint[0] * opacity, tint[1] * opacity, tint[2] * opacity };
		const std::array<float, 3> light{ detail[0] * detail_alpha + dark[0] * (1.0f - detail_alpha), detail[1] * detail_alpha + dark[1] * (1.0f - detail_alpha),
			detail[2] * detail_alpha + dark[2] * (1.0f - detail_alpha) };
		const std::array<float, 2> observed_dark_y{ 77.2f, 76.5f };
		const float observed_light_y = 122.0f;
		for (const float y : observed_dark_y) {
			CHECK(std::fabs(video_y(dark) - y) < 1.0f);
		}
		CHECK(std::fabs(video_y(light) - observed_light_y) < 1.0f);
		row("engine_parts", "iBgDraw over W: ammo 0.854*W+(0.9,31.7,54.1) from 3552 frames, light and coverage share one ramp per axis from 0.36 to 1.05 of the AS2 fade (1/21 x 1/5.25 of renderHolder), TDM light starts at y 451; EKG 78.7 px/s, 1.04 s, peak 9 px, undershoot 3 px; minimap 1.375 m/unit at MapZoom 0, no satellite term (slope <= 0 on m01 m02 m13 m14 m16), "
				"0.5 DetailTextureTint under a 0.3 detail tint where the field is above 0.5: dark Y 76.3 against 77.2 and 76.5, light Y 122.5 against 122.0 over a dark scene");
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
		for (std::string_view partition : { kKillScreenPartition, kSpawnScreenPcPartition, kScoreboardHud32ScreenPartition, kPreRoundWaitingScreenPartition,
					 kEorWinningTeamScreenPartition }) {
			Result<HudScreenAsset> a = read_hud_screen_asset(*r->level, partition);
			CHECK_MESSAGE(a.ok(), partition);
			CHECK_MESSAGE((a.ok() && !a->movie.empty()), partition);
		}
		Screen s = open_screen(*r, kHudMpScreenPartition);
		CHECK(s.hud->stats().unknown_classes == 0);
		CHECK(std::count_if(s.hud->widgets().begin(), s.hud->widgets().end(),
					  [](const auto &w) { return w->class_name == "Widget.Image.ImageManager"; }) == 4);
		Screen t = open_screen(*r, kHudTdmScreenPartition);
		CHECK(t.hud->stats().unknown_classes == 0);
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
		m.user.is_in_menu = true;
		s.hud->refresh(m);
		CHECK_FALSE(s.at(h).visible);
		m.user.is_in_menu = false;
		m.kill_card.visible = true;
		s.hud->refresh(m);
		CHECK_FALSE(s.at(h).visible);
		m.kill_card.visible = false;
		s.hud->refresh(m);
		CHECK(s.at(h).visible);
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
		HudModel dead = moment_m02();
		dead.spawn_screen.visible = true;
		s.hud->refresh(dead);
		CHECK_FALSE(s.at(mm).visible);
		s.hud->refresh(moment_m02());
		CHECK(s.at(mm).visible);
		row("minimap", "map t2;iUIMinimapRender, Size false tweens 4 properties over 0.05 s easeOutBack to 600 px");
	}

	TEST_CASE("hud screen: Crosshair") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		HudModel m = moment_m02();
		Result<HudScreenAsset> asset = read_hud_screen_asset(*r->level, kCrosshairDefaultPartition);
		REQUIRE(asset.ok());
		const HudWidgetNode *node = asset->find("Crosshair_01");
		REQUIRE(node != nullptr);
		REQUIRE(node->dispersion_base_offset.has_value());
		CHECK(*node->dispersion_base_offset == double(0.151f));
		m.weapon.dispersion = hud_crosshair_spread_for_arm(44.125, *node->dispersion_base_offset);
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
		m.weapon.zoomed = false;
		m.weapon.active_weapon_crosshair = 1;
		m.weapon.dispersion = 4.2f;
		s.hud->refresh(m);
		s.hud->advance(0.0);
		CHECK(s.at(x + ".crosshair.right").matrix.tx == 99.0f);
		m.weapon.dispersion = 3.5f;
		s.hud->refresh(m);
		s.hud->advance(0.0);
		CHECK(s.at(x + ".crosshair.right").matrix.tx == 85.0f);
		row("crosshair", "UICrosshairDataBinding DispersionBaseOffset 0.151 as BaseValue, Dispersion 0.2 per degree: 44.125 px -> arms at +-44, 4.2 deg -> 99, 3.5 deg -> 85, "
						 "HideOnZoom fades 0.2 s out / 0.1 s in");
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
		for (const char *w : { "PassangerList_01", "VehicleHealth_01", "HudAlertMessage_01", "HudAlertMessage_02" }) {
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
		CHECK(s.at("instance1.ScoreAggregator_01").visible);
		CHECK(s.at("instance1.ScoreAggregator_01.aggHolder").frame == s.label("instance1.ScoreAggregator_01.aggHolder", "hide"));
		CHECK(t.at("instance1.HudMessage_00").visible);
		REQUIRE(dynamic_cast<HudMessageApi *>(t.hud->controller("instance1.HudMessage_00")) != nullptr);
		CHECK(dynamic_cast<HudMessageApi *>(t.hud->controller("instance1.HudMessage_00"))->rows() == 0);
		row("hidden", "hit level 1 -> frame index 1, latency hidden, 4 data-driven widgets hidden until their data, 5 HudMessage lists shown empty, ScoreAggregator on hide");
	}

	TEST_CASE("hud screen: SquadList") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		std::array<int32_t, 2> kit_icons{};
		const std::array<const char *, 2> kits{ "gameplay/kits/usassault", "gameplay/kits/usengineer" };
		Screen probe = open_screen(*r, kHudTdmScreenPartition);
		const std::string sl = "instance1.SquadList_00";
		for (size_t k = 0; k < kits.size(); ++k) {
			Result<gameplay::KitDef> kit = gameplay::read_kit_def(*r->level, kits[k]);
			REQUIRE_MESSAGE(kit.ok(), (kit.ok() ? std::string() : kit.error().message));
			REQUIRE(kit->hud_icon.rfind("UIHudIcon_", 0) == 0);
			kit_icons[k] = kit->hud_icon_value;
			CHECK(uint32_t(kit_icons[k]) == probe.label(sl + ".squadMember0.kit", kit->hud_icon.substr(10)) + 1);
		}
		CHECK(probe.label(sl + ".squadMember0.leader", "SquadLeader") + 1 == uint32_t(kUiHudIconSquadLeader));
		CHECK(probe.label(sl + ".squadMember0.kit", "dead") + 1 == uint32_t(kUiHudIconDead));
		HudModel m = moment_m02();
		const std::array<const char *, 4> names{ "WWWWWWWW", "MMMMM", "HHHHHHHHHH", "NNN" };
		for (uint8_t i = 0; i < 4; ++i) {
			HudPlayer &p = m.players[i];
			std::snprintf(p.name.data(), p.name.size(), "%s", names[i]);
			p.team = 1;
			p.squad = 1;
			p.alive = i != 2;
			p.kit_icon = int16_t(kit_icons[i == 1 ? 1 : 0]);
		}
		m.squad.local_squad_members.count = 1;
		m.squad.local_squad_members.index = { 0, 1, 2, 3 };
		m.squad.local_squad_members.leader = 0;
		Screen s = initialized(*r, kHudTdmScreenPartition, m);
		CHECK(s.at(sl).visible);
		CHECK(s.renderer(sl) == "t2");
		for (int i = 0; i < 4; ++i) {
			CHECK_FALSE(s.at(sl + ".squadMember" + std::to_string(i)).visible);
		}
		const SwfSceneNode &authored_txt = probe.at(sl + ".squadMember0.label.txt");
		REQUIRE(authored_txt.edit_text != nullptr);
		REQUIRE(authored_txt.movie != nullptr);
		const SwfColor authored = swf_edit_text_format(*authored_txt.movie, *authored_txt.edit_text).color;
		CHECK(authored == SwfColor{ 0xE4, 0xFF, 0x9D, 0xFF });
		const SwfSceneNode &authored_bg = probe.at(sl + ".squadMember0.label.txtBg");
		REQUIRE(authored_bg.edit_text != nullptr);
		CHECK(swf_edit_text_format(*authored_bg.movie, *authored_bg.edit_text).color == SwfColor{ 0x00, 0x66, 0x00, 0xFF });
		CHECK(s.clip(sl + ".squadMember3.label.txtBg").text_color() == std::optional<uint32_t>(0x006600u));
		const SwfColorXform tint{ { 0.0f, 0.0f, 0.0f, 1.0f }, { float(authored.r), float(authored.g), float(authored.b), 0.0f } };
		for (int i = 0; i < 4; ++i) {
			const std::string rp = sl + ".squadMember" + std::to_string(i);
			for (const char *icon : { "kit", "leader", "voip", "booster" }) {
				CHECK_MESSAGE(s.at(rp + "." + icon).color == tint, (rp + "." + icon));
			}
			const SwfSceneNode &txt = s.at(rp + ".label.txt");
			CHECK(swf_edit_text_format(*txt.movie, *txt.edit_text).color == authored);
			CHECK(s.clip(rp + ".label.txt").text_color() == std::optional<uint32_t>((uint32_t(authored.r) << 16) | (uint32_t(authored.g) << 8) | authored.b));
		}
		m.squad.local_squad_members.count = 4;
		s.hud->refresh(m);
		for (int i = 0; i < 4; ++i) {
			const std::string rp = sl + ".squadMember" + std::to_string(i);
			const As2Clip rw = s.clip(rp);
			CHECK(s.at(rp).visible);
			CHECK(s.at(rp + ".label.txt").text == names[size_t(i)]);
			CHECK(s.at(rp + ".label.txtBg").text == names[size_t(i)]);
			CHECK(rw.alpha() == doctest::Approx(i == 2 ? 50.0f : 100.0f));
			const char *kit_label = i == 2 ? "dead" : i == 1 ? "KitEngineer" : "KitAssault";
			CHECK(s.at(rp + ".kit").frame == s.label(rp + ".kit", kit_label));
			CHECK(s.at(rp + ".leader").visible == (i == 0));
			CHECK(rw.child("label").x() == doctest::Approx(i == 0 ? 50.0f : 28.0f));
			if (i == 0) {
				CHECK(rw.child("leader").x() == doctest::Approx(40.0f));
				CHECK(s.at(rp + ".leader").frame == s.label(rp + ".leader", "SquadLeader"));
			}
			CHECK(rw.child("voip").x() == doctest::Approx(rw.child("label").x() + rw.child("label").child("txt").text_width() + 15.0f));
		}
		CHECK(s.clip(sl + ".squadMember0.label.txt").text_width() > s.clip(sl + ".squadMember3.label.txt").text_width());
		m.players[1].booster_icon = 140;
		s.hud->refresh(m);
		CHECK(s.at(sl + ".squadMember1.booster").visible);
		CHECK(s.at(sl + ".squadMember1.booster").frame == 139);
		CHECK(s.clip(sl + ".squadMember1.label").x() == doctest::Approx(48.0f));
		CHECK(s.clip(sl + ".squadMember0.label").x() == doctest::Approx(70.0f));
		m.players[1].booster_icon = 0;
		HudWidgetController *ctl = s.hud->controller(sl);
		REQUIRE(ctl != nullptr);
		HudWidgetHost host(*s.hud, sl, "Widget.Hud.SquadList");
		As2Value list = As2Value::array();
		const std::array<int32_t, 4> boosters{ 140, 0, 145, 144 };
		for (int i = 0; i < 4; ++i) {
			As2Value v = As2Value::object();
			v.set("Label", As2Value::of(names[size_t(i)]));
			v.set("Dead", As2Value::of(false));
			v.set("Kit", As2Value::of(kit_icons[0]));
			v.set("SquadLeader", As2Value::of(i == 1));
			v.set("Voip", As2Value::of(int32_t(1)));
			v.set("BoosterIcon", As2Value::of(boosters[size_t(i)]));
			list.push(std::move(v));
		}
		ctl->update(host, "SquadList", list);
		for (int i = 0; i < 4; ++i) {
			const std::string rp = sl + ".squadMember" + std::to_string(i);
			const As2Clip rw = s.clip(rp);
			CHECK(rw.child("label").x() == doctest::Approx(i == 1 ? 70.0f : 48.0f));
			CHECK(s.at(rp + ".booster").visible);
			CHECK(s.at(rp + ".booster").frame == uint32_t(std::max(boosters[size_t(i)], 1) - 1));
			CHECK(rw.child("voip").x() == doctest::Approx(rw.child("label").x() + rw.child("label").child("txt").text_width() + 15.0f));
		}
		CHECK(s.clip(sl + ".squadMember1.leader").x() == doctest::Approx(60.0f));
		CHECK(s.at(sl + ".squadMember3.booster").frame == s.label(sl + ".squadMember3.booster", "grenadeBoost"));
		As2Value one = As2Value::array();
		one.push(list.at(0));
		ctl->update(host, "SquadList", one);
		for (int i = 0; i < 4; ++i) {
			CHECK_FALSE(s.at(sl + ".squadMember" + std::to_string(i)).visible);
		}
		CHECK(s.hud->stats().as2.failed_ops == 0);
		char authored_hex[8] = {};
		std::snprintf(authored_hex, sizeof(authored_hex), "%02x%02x%02x", authored.r, authored.g, authored.b);
		row("squad_list", "HudPlayer::booster_icon 140 -> booster frame 140; kit icons " + std::to_string(kit_icons[0]) + "/" + std::to_string(kit_icons[1]) + " (UIHudIcon enum = 1-based label frame), icons tinted with the authored txt colour " + authored_hex + " (txtBg 006600 untinted), rows 4 at label x 50/28 without boosters, 70/48 with");
	}

	TEST_CASE("hud screen: Icon3D managers and tags") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		HudModel m = moment_m02();
		HudWorldTags world;
		HudClientState c = test_client();
		c.world_tags = &world;
		Screen t = initialized(*r, kHudTdmScreenPartition, m, c);
		const std::string mm = "instance1.MapmarkerManager_00";
		const HudWidgetNode *node = t.asset.find("MapmarkerManager_00");
		REQUIRE(node != nullptr);
		REQUIRE(node->widget_path_query.has_value());
		CHECK(node->widget_path_query->key == 1055499673);
		CHECK(node->widget_path_query->category == "ui/uicomponents/uimapmarkertagcomp");
		CHECK(node->widget_path_query->category_short_name == "UIMapmarkertagComp");
		const auto *api = dynamic_cast<HudIcon3DManagerApi *>(t.hud->controller(mm));
		REQUIRE(api != nullptr);
		CHECK(t.at(mm).visible);
		CHECK_FALSE(t.at(mm + ".Marker").visible);
		CHECK(api->published_path() == "_level0." + mm);
		CHECK(api->published_key() == 1055499673.0);
		CHECK(api->component_name() == "UIMapmarkertagComp");
		CHECK(t.clip(mm).x() == doctest::Approx(0.0f));
		CHECK(t.clip(mm).y() == doctest::Approx(0.0f));
		HudStageRects wide;
		wide.visible = SwfRect{ -160.0f, 1440.0f, -20.0f, 740.0f };
		t.hud->set_stage_rects(wide);
		CHECK(t.clip(mm).x() == doctest::Approx(-160.0f));
		CHECK(t.clip(mm).y() == doctest::Approx(-20.0f));
		t.hud->set_stage_rects(HudStageRects{});
		HudWorldTag marker;
		marker.id = 7;
		marker.component = HudTagComponent::Mapmarker;
		marker.x = 640.0f;
		marker.y = 300.0f;
		marker.data = { { "Icons", As2Value::of(int32_t(4)) }, { "Label", As2Value::of("WWWWWW") } };
		world.tags = { marker };
		t.hud->refresh(m);
		REQUIRE(api->tag_paths().size() == 1);
		const std::string tag = api->tag_paths().front();
		CHECK(tag == mm + ".tag7");
		CHECK(t.hud->widget(tag) != nullptr);
		CHECK(t.hud->widget(tag)->class_name == "Widget.Icon3D.Mapmarker.Mapmarkertag");
		CHECK(t.at(tag).visible);
		CHECK(t.clip(tag).x() == doctest::Approx(640.0f));
		CHECK(t.clip(tag).y() == doctest::Approx(300.0f));
		CHECK(t.at(tag).frame == t.label(tag, "create"));
		CHECK(t.at(tag + ".tag.icon").frame == 3);
		CHECK(t.at(tag + ".tag.iconBg").visible);
		CHECK(t.at(tag + ".tag.label").visible);
		CHECK(t.at(tag + ".tag.label").frame == t.label(tag + ".tag.label", "red"));
		CHECK(t.at(tag + ".tag.label.nameTxt").text.empty());
		CHECK(t.at(tag + ".tag").frame == t.label(tag + ".tag", "showRed"));
		const uint32_t complete = t.label(tag, "remove") - 1;
		CHECK(frames_until(t, [&] { return t.at(tag).frame == complete; }, 30) > 0);
		REQUIRE(t.scene->advance(3).ok());
		CHECK(t.at(tag).frame == complete);
		world.tags.front().x = 600.0f;
		world.tags.front().data.push_back({ "IsClose", As2Value::of(false) });
		t.hud->refresh(m);
		CHECK(t.clip(tag).x() == doctest::Approx(600.0f));
		CHECK(t.at(tag + ".tag").frame == t.label(tag + ".tag", "hiddenRed"));
		CHECK(t.at(tag + ".tag.label.nameTxt").text == "WWWWWW");
		CHECK(t.at(tag + ".tag.label.nameTxtBg").text == "WWWWWW");
		world.tags.front().data.push_back({ "LookingAt", As2Value::of(true) });
		t.hud->refresh(m);
		CHECK(t.at(tag + ".tag").frame == t.label(tag + ".tag", "showRed"));
		world.tags.front().data.push_back({ "Spotted", As2Value::of(int32_t(1)) });
		t.hud->refresh(m);
		CHECK(t.at(tag).frame == t.label(tag, "spotted"));
		world.tags.clear();
		t.hud->refresh(m);
		CHECK(t.at(tag).frame == t.label(tag, "remove"));
		CHECK(frames_until(t, [&] { return t.scene->find_node(tag) == nullptr; }, 30) > 0);
		t.hud->refresh(m);
		CHECK(api->tag_paths().empty());
		CHECK(t.hud->widget(tag) == nullptr);

		Screen s = initialized(*r, kHudMpScreenPartition, m, c);
		const std::string sm = "instance1.SupportIconManager_01";
		const auto *support = dynamic_cast<HudIcon3DManagerApi *>(s.hud->controller(sm));
		REQUIRE(support != nullptr);
		CHECK(s.at(sm).visible);
		CHECK_FALSE(s.at(sm + ".TeamSupportIcon").visible);
		CHECK(support->published_key() == 2138355157.0);
		CHECK(support->component_name().empty());
		const int32_t beacon = int32_t(s.label(sm + ".TeamSupportIcon.tag", "RadioBeacon")) + 1;
		HudWorldTag request;
		request.id = 3;
		request.component = HudTagComponent::TeamSupport;
		request.x = 500.0f;
		request.y = 200.0f;
		request.data = { { "Icons", As2Value::of(beacon) }, { "Team", As2Value::of(int32_t(2)) } };
		world.tags = { request, marker };
		s.hud->refresh(m);
		REQUIRE(support->tag_paths().size() == 1);
		const std::string st = support->tag_paths().front();
		CHECK(s.hud->widget(st)->class_name == "Widget.Icon3D.TeamSupport.SupportIconTag");
		CHECK(s.at(st).visible);
		CHECK(s.at(st).frame == s.label(st, "create"));
		CHECK(s.at(st + ".tag").frame == uint32_t(beacon - 1));
		CHECK(s.at(st + ".tag.icon").frame == 1);
		CHECK(frames_until(s, [&] { return s.at(st).frame == s.label(st, "remove") - 1; }, 30) > 0);

		Screen h = initialized(*r, kHudSharedScreenPartition, m, c);
		const std::string am = "instance1.AlertManager_01";
		const auto *alerts = dynamic_cast<HudIcon3DManagerApi *>(h.hud->controller(am));
		REQUIRE(alerts != nullptr);
		CHECK(h.at(am).visible);
		HudWorldTag alert;
		alert.id = 9;
		alert.component = HudTagComponent::Alert;
		alert.x = 700.0f;
		alert.y = 420.0f;
		alert.data = { { "Snap", As2Value::of(false) } };
		world.tags = { alert };
		h.hud->refresh(m);
		REQUIRE(alerts->tag_paths().size() == 1);
		const std::string at = alerts->tag_paths().front();
		CHECK(h.hud->widget(at)->class_name == "Widget.Icon3D.Alerts.Alerttag");
		CHECK(h.at(at).visible);
		const uint32_t grenade = h.at(at + ".grIcon").frame;
		world.tags.front().data = { { "Snap", As2Value::of(true) } };
		h.hud->refresh(m);
		CHECK(h.at(at + ".grIcon").frame == h.label(at + ".grIcon", "snapped"));
		CHECK(grenade != h.label(at + ".grIcon", "snapped"));
		CHECK(t.hud->stats().as2.failed_ops == 0);
		CHECK(s.hud->stats().as2.failed_ops == 0);
		CHECK(h.hud->stats().as2.failed_ops == 0);
		row("icon3d", "MapmarkerManager key 1055499673 UIMapmarkertagComp at the visible rect without the safe rect; Marker tag create -> stop at " +
						std::to_string(complete) + ", red label for UIHudIcon 4 (name set by the next updateTag, the red frame has its own field), hiddenRed/showRed snapping, spotted, remove -> removed; TeamSupportIcon frame " +
						std::to_string(beacon) + " (RadioBeacon) team 2; Alert snapped");
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
		const TextDatabase &db = ui_retail::retail_text_db(*r);
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
		AlphaTrace fade_in;
		trace_alpha(s, row1, "container", fade_in);
		const uint32_t started_after = frames_until(s, [&] {
			trace_alpha(s, row1, "container", fade_in);
			return s.at(row1).frame + 1 >= out_label || !s.at(row1).playing;
		}, 120);
		check_trace(fade_in, kKillRowFadeIn);
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
		AlphaTrace fade_out;
		trace_alpha(s, row1, "container", fade_out);
		const uint32_t removed_after = frames_until(s, [&] {
			if (s.scene->find_node(row1) == nullptr) {
				return true;
			}
			trace_alpha(s, row1, "container", fade_out);
			return false;
		}, 120);
		CHECK(removed_after == row_tl.frames - 1 - out_label);
		check_trace(fade_out, kKillRowFadeOut);
		const double fade_out_seconds = double(removed_after) / double(s.scene->frame_rate());
		CHECK(fade_out_seconds == doctest::Approx(5.0 / 30.0));
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
						std::to_string(h1) + "; container alpha in " + trace_text(fade_in) + ", out " + trace_text(fade_out) + " (" +
						std::to_string(fade_out_seconds) + " s)");
	}

	TEST_CASE("hud screen: KillFeed team kill") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		const TextDatabase &db = ui_retail::retail_text_db(*r);
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
		const TextDatabase &db = ui_retail::retail_text_db(*r);
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
		const As2Value &tinted = comp.list(HudMessageSource::LocalKill).at(0).field("Value");
		CHECK(tinted.field("KillerColor").to_number() == double(kHudRelationSquad));
		CHECK(tinted.field("VictimColor").to_number() == double(kHudRelationEnemy));
		const auto check_tint = [&](const std::string &p_name, uint32_t p_text, uint32_t p_shadow, uint32_t p_glow) {
			CHECK(s.clip(p_name).child("txt").text_color() == std::optional<uint32_t>(p_text));
			const SwfSceneNode &txt = s.at(p_name + ".txt");
			REQUIRE(txt.filters.size() == 1);
			CHECK(txt.filters[0].kind == SwfFilterKind::DropShadow);
			CHECK(txt.filters[0].color == SwfColor{ uint8_t(p_shadow >> 16), uint8_t(p_shadow >> 8), uint8_t(p_shadow), 255 });
			CHECK(txt.filters[0].blur_x == 8.0f);
			CHECK(txt.filters[0].strength == doctest::Approx(1.7f));
			CHECK(txt.filters[0].angle == doctest::Approx(1.5707963f));
			CHECK(txt.filters[0].distance == 1.0f);
			const SwfSceneNode &bg = s.at(p_name + ".txtBg");
			REQUIRE(bg.filters.size() == 1);
			CHECK(bg.filters[0].kind == SwfFilterKind::Glow);
			CHECK(bg.filters[0].color == SwfColor{ uint8_t(p_glow >> 16), uint8_t(p_glow >> 8), uint8_t(p_glow), 255 });
			CHECK(bg.filters[0].blur_x == 2.0f);
			CHECK(bg.filters[0].knockout);
		};
		check_tint(c1 + ".killer", 0xe4ff9du, 0x74dc21u, 0x006600u);
		check_tint(c1 + ".victim", 0xffeab9u, 0xff6000u, 0xcc3300u);
		CHECK(s.at(c1 + ".headShotIcon").color.add[0] == 228.0f);
		CHECK(s.at(c1 + ".headShotIcon").color.add[1] == 255.0f);
		CHECK(s.at(c1 + ".headShotIcon").color.add[2] == 157.0f);
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
		AlphaTrace fade_in;
		trace_alpha(s, row1, "container", fade_in);
		const uint32_t started_after = frames_until(s, [&] {
			trace_alpha(s, row1, "container", fade_in);
			return !s.at(row1).playing;
		}, 60);
		CHECK(s.at(row1).frame == s.label(row1, "startAnim") + started_after);
		check_trace(fade_in, kKillRowFadeIn);
		check_tint(c1 + ".killer", 0xe4ff9du, 0x74dc21u, 0x006600u);
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
		AlphaTrace fade_out;
		trace_alpha(s, row1, "container", fade_out);
		const uint32_t removed_after = frames_until(s, [&] {
			if (s.scene->find_node(row1) == nullptr) {
				return true;
			}
			trace_alpha(s, row1, "container", fade_out);
			return false;
		}, 60);
		CHECK(removed_after == s.at(log + ".messageHolder.message_2").timeline->frames - 1 - out_label);
		check_trace(fade_out, kKillRowFadeOut);
		CHECK(api->rows() == 1);
		row("kill_log", "UIMessageCompData LocalKill queue 2, 5 s, short 1 s; hudtdmscreen HudMessage_00 key 1675248891 rows 2; row " + weapon_text +
						" centred at x " + std::to_string(s.clip(log + ".messageHolder.message_2").x()) + ", removed " + std::to_string(removed_after) +
						" frames after outAnim; container alpha in " + trace_text(fade_in) + ", out " + trace_text(fade_out));
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
		const TextDatabase &db = ui_retail::retail_text_db(*r);
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
		const uint32_t start = s.label(row1, "start");
		AlphaTrace intro;
		trace_alpha(s, row1, "scoringMessage", intro);
		const uint32_t looped = frames_until(s, [&] {
			trace_alpha(s, row1, "scoringMessage", intro);
			return s.at(row1).frame == loop;
		}, 2 * end);
		CHECK(looped == loop - start);
		check_trace_follows_timeline(intro);
		REQUIRE(intro.drawn.size() == size_t(looped) + 1);
		CHECK(intro.drawn[11 - start] == 0.0f);
		CHECK(intro.drawn[12 - start] == doctest::Approx(14.0f / 256.0f));
		CHECK(intro.drawn[15 - start] == doctest::Approx(218.0f / 256.0f));
		CHECK(intro.drawn.back() == doctest::Approx(timeline_alpha(s.at(row1), "scoringMessage")));
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
		AlphaTrace outro;
		trace_alpha(s, row1, "scoringMessage", outro);
		const uint32_t removed_after = frames_until(s, [&] {
			if (s.scene->find_node(row1) == nullptr) {
				return true;
			}
			trace_alpha(s, row1, "scoringMessage", outro);
			return false;
		}, 2 * end);
		CHECK(removed_after == s.at(row2).timeline->frames - 1 - end);
		check_trace_follows_timeline(outro);
		REQUIRE(outro.drawn.size() == size_t(removed_after));
		CHECK(outro.drawn.front() > outro.drawn.back());
		CHECK(api->rows() == 1);
		row("score_popup", "UIMessageCompData Score queue 3, 5 s, short 5 s; HudMessage_01 key 1802462620 inverted; rows ENEMY KILLED 100, HEADSHOT 10 at x 140, "
						"loop " + std::to_string(loop) + ".." + std::to_string(end - 1) + ", removed " + std::to_string(removed_after) + " frames after end " +
						std::to_string(end) + ", row height " + std::to_string(h1) + "; scoringMessage alpha from start " + trace_text(intro) + "; from end " +
						trace_text(outro));
	}

	TEST_CASE("hud screen: ScoreAggregator") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		const HudMessageCompData data = message_data(*r);
		HudMessageComp comp(data);
		HudClientState c = text_client(ui_retail::retail_text_db(*r));
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
		push_damage(m, 4, 90.0f, 60.0f);
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
						" Hz; key -1063919953 on hudscreen DamageIndicator_01; bearing 90 at heading 0 -> frame 91 (arrow matrix b " + std::to_string(right.b) +
						", clockwise), Scale 60 -> fullHit, 20 -> minorHit, idle arrow frame " + std::to_string(idle));
	}

	TEST_CASE("hud screen: HudAlertMessage") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		const TextDatabase &db = ui_retail::retail_text_db(*r);
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
		CHECK(s.at("instance1.AlertManager_01").visible);
		CHECK(s.at("instance1.InteractionManager_01").visible);
		CHECK(s.at("instance1.DamageIndicator_01").visible);
		row("shared_screen", "hudscreen " + std::to_string(s.hud->stats().instances) + " instances, 0 unknown classes; Log/Tooltip/Objective tilts -5/-15, -5/15, "
						"5/-15 at fov 25 on t2, Objective shown, AlertManager and InteractionManager shown");
	}

	TEST_CASE("hud screen: the in-round controllers take no input, so the input hooks change nothing") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		const HudModel m = moment_m02();
		uint32_t controllers = 0;
		for (std::string_view partition : { kHudMpScreenPartition, kHudTdmScreenPartition, kCrosshairDefaultPartition, kHudSharedScreenPartition }) {
			Screen s = initialized(*r, partition, m);
			s.hud->refresh(m);
			s.scene->take_changes();
			const HudScreenStats before = s.hud->stats();
			for (const std::unique_ptr<HudWidgetState> &w : s.hud->widgets()) {
				if (w->controller == nullptr || w->host == nullptr) {
					continue;
				}
				++controllers;
				HudWidgetController &c = *w->controller;
				HudWidgetHost &h = *w->host;
				const HudMouse mouse{ 640.0f, 360.0f, 1 };
				c.focus_gained(h);
				CHECK_FALSE(c.input_concept(h, 0, true));
				CHECK_FALSE(c.input_concept(h, 0, false));
				CHECK_FALSE(c.key(h, 13, true));
				CHECK_FALSE(c.key(h, 13, false));
				CHECK_FALSE(c.mouse_move(h, mouse));
				CHECK_FALSE(c.mouse_press(h, mouse));
				CHECK_FALSE(c.mouse_release(h, mouse));
				c.focus_lost(h);
			}
			CHECK_FALSE(s.hud->has_focus());
			s.hud->set_focus(true);
			CHECK(s.hud->has_focus());
			s.hud->set_focus(true);
			s.hud->set_focus(false);
			CHECK_FALSE(s.hud->has_focus());
			CHECK(s.scene->take_changes().empty());
			CHECK(s.hud->stats().updates == before.updates);
			CHECK(s.hud->stats().as2.failed_ops == before.as2.failed_ops);
			CHECK(s.hud->stats().as2.missing_paths == before.as2.missing_paths);
		}
		CHECK(controllers > 0);
		row("input_hooks", std::to_string(controllers) + " controllers over the 4 in-round screens: concept, key, mouse and focus hooks unhandled, screen focus given and taken back, no scene change");
	}

	TEST_CASE("hud screen: controller timers, enter frames, custom tweens and the screen exit") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		struct Probe : HudWidgetController {
			std::vector<uint32_t> fired;
			uint32_t frames = 0;
			float value = 0.0f;
			std::vector<float> set;
			std::optional<float> tween_property(HudWidgetHost &, std::string_view p_name) override {
				return p_name == "newFrame" ? std::optional<float>(value) : std::nullopt;
			}
			void set_tween_property(HudWidgetHost &, std::string_view p_name, float p_value) override {
				if (p_name == "newFrame") {
					value = p_value;
					set.push_back(p_value);
				}
			}
			void interval(HudWidgetHost &, uint32_t p_id) override { fired.push_back(p_id); }
			void enter_frame(HudWidgetHost &) override { ++frames; }
			bool begin_exit(HudWidgetHost &) override { return false; }
		};
		Screen s = open_screen(*r, "ui/flow/screen/preroundwaitingscreen");
		CHECK_FALSE(s.hud->enter_complete());
		s.hud->initialize(HudModel{});
		CHECK(s.hud->enter_complete());
		const std::string path = std::string(kHudScreenInstance) + ".HudAlertMessage_03";
		HudWidgetState *w = nullptr;
		for (const std::unique_ptr<HudWidgetState> &cand : s.hud->widgets()) {
			if (cand->path == path) {
				w = cand.get();
			}
		}
		REQUIRE(w != nullptr);
		auto made = std::make_unique<Probe>();
		Probe &probe = *made;
		w->controller = std::move(made);
		HudWidgetHost &host = *w->host;
		const uint32_t id = host.set_interval(0.25);
		const uint32_t other = host.set_interval(0.5);
		s.hud->advance(1.0);
		CHECK(probe.fired == std::vector<uint32_t>{ id, id, id, id, other, other });
		host.clear_interval(id);
		probe.fired.clear();
		s.hud->advance(0.5);
		CHECK(probe.fired == std::vector<uint32_t>{ other });
		host.clear_interval(other);
		CHECK(s.hud->intervals() == 0);
		const float fps = s.scene->frame_rate();
		REQUIRE(fps > 0.0f);
		host.set_enter_frame(true);
		for (int i = 0; i < 10; ++i) {
			s.hud->advance(0.1);
		}
		CHECK(probe.frames == uint32_t(std::lround(fps)));
		host.set_enter_frame(false);
		s.hud->advance(1.0);
		CHECK(probe.frames == uint32_t(std::lround(fps)));
		host.add_custom_tween("newFrame", 360.0f, 2.0, HudTweenEase::Linear);
		s.hud->advance(1.0);
		CHECK(probe.value == doctest::Approx(180.0f));
		s.hud->advance(1.0);
		CHECK(probe.value == 360.0f);
		CHECK(s.hud->tweens().empty());
		const As2Clip root = s.clip(kHudScreenInstance);
		CHECK(root.current_frame() == 1);
		s.hud->begin_exit();
		CHECK(s.hud->exiting());
		s.hud->advance(0.1);
		REQUIRE(s.scene->advance_seconds(0.1).ok());
		CHECK(root.current_frame() == s.label(kHudScreenInstance, "onExit") + 1);
		CHECK_FALSE(s.hud->exit_complete());
		host.exit_complete();
		CHECK(s.hud->exit_complete());
		Screen t = open_screen(*r, "ui/flow/screen/preroundwaitingscreen");
		t.hud->initialize(HudModel{});
		REQUIRE(t.hud->enter_complete());
		const As2Clip other_root = t.clip(kHudScreenInstance);
		const uint32_t frame_before_exit = other_root.current_frame();
		t.hud->begin_exit();
		CHECK(t.hud->exit_complete());
		CHECK(other_root.current_frame() != frame_before_exit);
		row("timers",
				std::to_string(probe.frames) + " enter frames at " + std::to_string(fps) +
						" fps; intervals, the newFrame tween, the onExit frame's onExitComplete and an exit that ends "
						"with its last widget's");
	}

	TEST_CASE("hud screen: DirectAccess on a screen clip sets visibility and reaches the controller") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		struct Probe : HudWidgetController {
			std::vector<std::string> seen;
			void direct_access(HudWidgetHost &, const UiDirectAccessApply &p_apply) override {
				seen.push_back(p_apply.clip + " " + ui_direct_access_op_name(p_apply.op) + " " + p_apply.name + "=" +
						p_apply.value.to_string());
			}
		};
		Screen s = open_screen(*r, "ui/flow/screen/preroundwaitingscreen");
		s.hud->initialize(HudModel{});
		const std::string path = std::string(kHudScreenInstance) + ".HudAlertMessage_03";
		HudWidgetState *w = nullptr;
		for (const std::unique_ptr<HudWidgetState> &cand : s.hud->widgets()) {
			if (cand->path == path) {
				w = cand.get();
			}
		}
		REQUIRE(w != nullptr);
		auto made = std::make_unique<Probe>();
		Probe &probe = *made;
		w->controller = std::move(made);
		HudWidgetHost &host = *w->host;
		const As2Clip self = s.clip(path);
		REQUIRE(self.exists());
		CHECK_FALSE(self.visible());
		const std::optional<uint32_t> vis = host.register_mc(self, { As2Value::of(3), As2Value::of("shown") });
		REQUIRE(vis.has_value());
		REQUIRE(host.register_mc(self,
							{ As2Value::of(14), As2Value::of("shown"), As2Value::of("m_shown"), As2Value::of(false) })
						.has_value());
		s.hud->direct_access().set_value("shown", As2Value::of(true));
		CHECK(self.visible());
		s.hud->direct_access().set_value("shown", As2Value::of(false));
		CHECK_FALSE(self.visible());
		CHECK(probe.seen ==
				std::vector<std::string>{
						path + " REGISTER_MEMBER m_shown=true", path + " REGISTER_MEMBER m_shown=false" });
		host.unregister_mc(*vis);
		s.hud->direct_access().set_value("shown", As2Value::of(true));
		CHECK_FALSE(self.visible());
		CHECK(probe.seen.size() == 3);
		CHECK(s.hud->direct_access().registrations() == 1);
		REQUIRE(s.scene->find_node(path) != nullptr);
		CHECK(s.hud->direct_access().errors().empty());
		row("direct_access_screen", "REGISTER_VISIBLE on " + path + " and REGISTER_MEMBER to its controller");
	}

	TEST_CASE("hud screen: a movie screen without a UIScreenAsset reads _root.getData from its host") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		const HudScreenAsset asset = hud_movie_screen_asset("ui/assets/loadingscreen");
		CHECK(asset.partition == "ui/assets/loadingscreen");
		CHECK(asset.name == "loadingscreen");
		CHECK(asset.movie == "ui/assets/loadingscreen");
		CHECK(asset.widgets.empty());
		Result<SwfScene> scene = SwfScene::open(*r->lib, asset.movie);
		REQUIRE_MESSAGE(scene.ok(), (scene.ok() ? std::string() : scene.error().message));
		SwfScene sc = std::move(*scene);
		Result<std::unique_ptr<HudScreen>> made = HudScreen::open(sc, *r->lib, asset, r->fonts.get(), test_client());
		REQUIRE_MESSAGE(made.ok(), (made.ok() ? std::string() : made.error().message));
		HudScreen &hud = **made;
		const std::vector<std::string> &unknown = hud.stats().unknown_class_paths;
		CHECK(std::find(unknown.begin(), unknown.end(), std::string(kHudScreenInstance) + " Screen.LoadingScreen") ==
				unknown.end());
		CHECK(hud.controller(kHudScreenInstance) != nullptr);
		HudRootDataMap data;
		data.set_data("gameType", As2Value::of("multiplayer"));
		data.set_data("gameMode", As2Value::of("team deathmatch"));
		data.set_data("levelName", As2Value::of("ID_M_LEVEL_MP_SUBWAY"));
		HudWidgetHost host(hud, std::string(kHudScreenInstance), "Screen.LoadingScreen");
		CHECK_FALSE(host.root_get_data("gameMode").defined());
		hud.set_root_data(&data);
		CHECK(host.root_get_data("gameType").to_string() == "multiplayer");
		CHECK(host.root_get_data("gameMode").to_string() == "team deathmatch");
		CHECK(host.root_get_data("levelName").to_string() == "ID_M_LEVEL_MP_SUBWAY");
		CHECK_FALSE(host.root_get_data("serverName").defined());
		host.root_set_data("joinSquadHasShowed", As2Value::of(true));
		CHECK(data.get_data("joinSquadHasShowed").truthy());
		CHECK(data.reads() == 5);
		CHECK(data.writes() == 4);
		hud.initialize(HudModel{});
		CHECK(hud.initialized());
		CHECK(hud.enter_complete());
		hud.begin_exit();
		hud.advance(1.0 / 30.0);
		CHECK(hud.exit_complete());
		row("movie_screen",
				"ui/assets/loadingscreen opens without a UIScreenAsset and reads _root.getData from the host");
	}

	TEST_CASE("hud screen: hit test clicks spawnscreenpc Button_03") {
		Retail *r = retail_or_skip();
		if (r == nullptr) {
			return;
		}
		Screen s = open_screen(*r, "ui/flow/screen/spawnscreenpc", text_client(ui_retail::retail_text_db(*r)));
		const auto node = std::find_if(s.asset.widgets.begin(), s.asset.widgets.end(),
				[](const HudWidgetNode &w) { return w.instance == "Button_03"; });
		REQUIRE(node != s.asset.widgets.end());
		std::vector<std::string> props;
		for (const HudWidgetProperty &p : node->properties) {
			props.push_back(p.name + "=" + p.value);
		}
		const std::vector<std::string> expected_props{ "TextData=", "IconData=", "Visible=", "Toggled=",
			"TextData=ID_M_HIDE_MAP;ID_M_SHOW_MAP", "IconData=3;4", "Visible=", "Toggled=" };
		CHECK(props == expected_props);
		REQUIRE(node->events.size() > 11);
		const std::string hide_query = node->events[11].query;
		std::printf("ui.hud.screen hit Button_03 events %zu hide '%s' '%s'\n", node->events.size(),
				node->events[11].name.c_str(), hide_query.c_str());
		std::vector<std::string> fired;
		s.hud->set_event_sink([&](const HudWidgetEvent &e) {
			if (e.instance == "Button_03" || e.index < 0) {
				fired.push_back(std::to_string(e.index) + " " + e.query + " " + e.param.to_string());
			}
		});
		s.hud->initialize(HudModel{});
		fired.clear();
		const bool listed =
				std::any_of(s.hud->mouse_targets().begin(), s.hud->mouse_targets().end(), [](const HudMouseTarget &t) {
					return t.path == "instance1.Button_03" && t.widget == "instance1.Button_03";
				});
		CHECK(listed);
		const SwfSceneNode *label = s.scene->find_node("instance1.Button_03.button.label.txt");
		const SwfSceneNode *bg = s.scene->find_node("instance1.Button_03.button.bgHolder");
		REQUIRE(label != nullptr);
		REQUIRE(bg != nullptr);
		CHECK(label->text == "HIDE MAP");
		const SwfRect lb = swf_node_local_bounds(*s.scene, *bg);
		const SwfMatrix bw = swf_world_matrix(*s.scene, bg->id);
		const float cx = bw.tx + (lb.x_min + lb.x_max) * 0.5f;
		const float cy = bw.ty + (lb.y_min + lb.y_max) * 0.5f;
		std::printf("ui.hud.screen hit Button_03 at %.1f %.1f\n", cx, cy);
		CHECK(s.hud->hit_target(cx, cy) == "instance1.Button_03");
		CHECK(s.hud->hit_target(cx, cy + 400.0f).empty());
		CHECK(s.hud->mouse_move(cx, cy));
		CHECK(s.hud->hovered() == "instance1.Button_03");
		CHECK(s.hud->mouse_button(cx, cy, 1, true));
		CHECK(s.hud->mouse_button(cx, cy, 1, false));
		CHECK(s.hud->mouse_move(cx, cy + 400.0f));
		const std::vector<std::string> click{ "1 UIWidgetEventID_OnItemOver ID_M_HIDE_MAP",
			"0 UIWidgetEventID_OnItemPressed ID_M_HIDE_MAP", "4 UIWidgetEventID_ToggledOn ID_M_HIDE_MAP",
			"-1 UIWidgetEventID_PlaySoundSelect false", "3 UIWidgetEventID_OnItemReleased ID_M_HIDE_MAP",
			"2 UIWidgetEventID_OnItemOut ID_M_HIDE_MAP" };
		CHECK(fired == click);
		CHECK(label->text == "SHOW MAP");
		fired.clear();
		s.hud->mouse_move(cx, cy);
		s.hud->mouse_button(cx, cy, 1, true);
		s.hud->mouse_move(cx, cy + 400.0f);
		s.hud->mouse_button(cx, cy + 400.0f, 1, false);
		const std::vector<std::string> outside{ "1 UIWidgetEventID_OnItemOver ID_M_HIDE_MAP",
			"0 UIWidgetEventID_OnItemPressed ID_M_HIDE_MAP" };
		CHECK(fired == outside);
		CHECK(s.hud->hovered().empty());
		CHECK(label->text == "SHOW MAP");
		const SwfSceneNode *self = s.scene->find_node("instance1.Button_03");
		REQUIRE(self != nullptr);
		CHECK(self->visible);
		CHECK(s.hud->deliver_in_event("Button_03", hide_query));
		CHECK_FALSE(self->visible);
		CHECK(s.hud->hit_target(cx, cy).empty());
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
