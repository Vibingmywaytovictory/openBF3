#include <doctest/doctest.h>

#include "bf3/core/gameplay/death_timeline.h"
#include "bf3/core/install_snapshot.h"
#include "bf3/core/level/level.h"
#include "bf3/core/ui/hud_screen.h"
#include "bf3/core/ui/options_model.h"
#include "bf3/core/ui/ui_input.h"
#include "bf3/host/logic_screen_effects.h"
#include "bf3/host/ui_flow_session.h"
#include "test_support.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace bf3;
using namespace bf3::host;
namespace ui = bf3::core::ui;

namespace {

using Lines = std::vector<std::string>;

struct Level {
	bf3::test::Mounted *mount = nullptr;
	std::unique_ptr<core::level::LoadedLevel> level;
	std::string error;
};

Level *level_or_skip() {
	static Level l;
	static bool tried = false;
	if (!tried) {
		tried = true;
		l.mount = bf3::test::shared_mount_or_skip();
		if (l.mount == nullptr) {
			return nullptr;
		}
		core::Result<std::unique_ptr<core::level::LoadedLevel>> loaded =
				core::level::load_level(l.mount->install, "MP_Subway", "TeamDeathMatch0");
		if (!loaded) {
			l.error = loaded.error().message;
		} else {
			l.level = std::move(*loaded);
		}
	} else if (l.mount == nullptr) {
		bf3::test::skip_install_case("no install");
		return nullptr;
	}
	REQUIRE_MESSAGE(l.error.empty(), l.error);
	return &l;
}

bool is_step(const std::string &p_line) {
	if (p_line == "push emptyscreen" || p_line == "push tablistenerscreen") {
		return false;
	}
	for (const std::string_view p : { "native ", "logic ", "push ", "close " }) {
		if (p_line.starts_with(p)) {
			return true;
		}
	}
	return false;
}

struct Script {
	UiFlowSession &session;
	UiGameState state;
	sim::Tick tick = 0;
	std::vector<std::pair<sim::Tick, std::string>> timeline;
	size_t printed = 0;
	Lines widget_events;
	Lines actions;

	void collect() {
		for (const std::string &l : session.log()) {
			if (is_step(l)) {
				timeline.emplace_back(tick, l);
			}
		}
		session.clear_log();
		for (const ui::UiFlowAction &a : session.take_actions()) {
			timeline.emplace_back(tick, "deploy " + a.name);
			std::string line = a.name;
			for (const std::string &p : a.params) {
				line += " " + p;
			}
			actions.push_back(std::move(line));
		}
		for (const UiFlowWidgetEvent &e : session.take_widget_events()) {
			widget_events.push_back(ui::ui_data_category_leaf(e.partition) + "." + e.instance + " " + e.query);
		}
	}
	void run_to(sim::Tick p_tick) {
		while (tick < p_tick) {
			++tick;
			REQUIRE(session.update(state).ok());
			collect();
		}
	}
	void input(ui::UiInputConcept p_concept) {
		session.input_action(int32_t(p_concept));
		collect();
	}
	Lines phase(const char *p_label) {
		Lines out;
		std::printf("ui.flow.session %s\n", p_label);
		for (size_t i = printed; i < timeline.size(); ++i) {
			const auto &[t, l] = timeline[i];
			std::printf("  %u %s\n", unsigned(t), l.c_str());
			if (!l.starts_with("logic ")) {
				out.push_back(l);
			}
		}
		printed = timeline.size();
		return out;
	}
	int64_t at(std::string_view p_line, sim::Tick p_from) const {
		for (const auto &[t, l] : timeline) {
			if (t >= p_from && l == p_line) {
				return int64_t(t);
			}
		}
		return -1;
	}
};
}

TEST_SUITE("ui-flow-session") {
	TEST_CASE("ui flow session: a scripted TDM round through the client UI logic") {
		Level *l = level_or_skip();
		if (l == nullptr) {
			return;
		}
		UiFlowSessionOptions echo;
		echo.echo_networked = true;
		core::Result<std::unique_ptr<UiFlowSession>> made = UiFlowSession::create(*l->level, echo);
		REQUIRE_MESSAGE(made.ok(), (made.ok() ? std::string() : made.error().message));
		UiFlowSession &s = **made;
		const UiFlowSessionStats &st = s.stats();
		std::printf("ui.flow.session ui graphs %zu (%zu assets), watches %zu, fades %zu, end of round %zu, pre-round "
					"%zu, round over %zu\n",
				st.ui_graph_nodes, st.graphs, st.watches, st.fades, st.end_of_round_nodes, st.pre_round_nodes,
				st.round_over_nodes);
		ui_flow_tdm_pc_defaults(s, "TeamDeathMatch0");
		CHECK(s.data("ui/uicomponents/uisettingscomp", 1733092213) == "TeamDeathMatch0");
		CHECK(s.data("ui/uicomponents/uisettingscomp", 1870459195) == "TeamDeathMatch");
		Script sc{ s };
		sc.state.phase = net::MatchPhase::PreRound;
		sc.state.joined = true;
		sc.run_to(60);
		const Lines join = sc.phase("join");
		const ui::UiFlowScreen *deploy_screen = s.flow().find_screen(ui::kSpawnScreenPcPartition);
		REQUIRE(deploy_screen != nullptr);
		CHECK(s.flow().focused() != deploy_screen->id);
		CHECK(s.flow().concept_screen(int32_t(ui::UiInputConcept::Activate)) == deploy_screen->id);
		sc.input(ui::UiInputConcept::Activate);
		sc.run_to(100);
		sc.state.own_spawns = 1;
		sc.run_to(160);
		const Lines first_spawn = sc.phase("deploy and spawn in pre-round");
		sc.state.counting_down = true;
		sc.run_to(220);
		const Lines countdown = sc.phase("countdown");
		sc.state.phase = net::MatchPhase::InRound;
		sc.state.counting_down = false;
		sc.run_to(500);
		const Lines started = sc.phase("round started");
		sc.input(ui::UiInputConcept::Menu);
		sc.run_to(540);
		const Lines menu = sc.phase("menu");
		const std::optional<uint64_t> menu_target = s.flow().concept_screen(int32_t(ui::UiInputConcept::Deactivate));
		REQUIRE(menu_target.has_value());
		CHECK(ui::ui_data_category_leaf(s.flow().screen(*menu_target)->partition) == "ingamemenump");
		CHECK_FALSE(s.flow().concept_screen(int32_t(ui::UiInputConcept::Tab)).has_value());
		sc.input(ui::UiInputConcept::Deactivate);
		sc.run_to(600);
		const Lines resume = sc.phase("resume");
		sc.state.own_deaths = 1;
		sc.run_to(700);
		CHECK(s.flow().find_screen(ui::kKillScreenPartition) != nullptr);
		sc.run_to(1000);
		CHECK(s.flow().find_screen(ui::kKillScreenPartition) == nullptr);
		const Lines death = sc.phase("death");
		sc.input(ui::UiInputConcept::Activate);
		sc.run_to(1060);
		sc.state.own_spawns = 2;
		sc.run_to(1200);
		const Lines deploy = sc.phase("deploy and spawn");
		sc.state.own_deaths = 2;
		sc.run_to(1300);
		sc.state.own_spawns = 3;
		sc.run_to(1700);
		const Lines revive = sc.phase("revive");
		sc.state.phase = net::MatchPhase::RoundOver;
		sc.state.winner = 2;
		sc.run_to(1800);
		sc.state.end_of_round = true;
		sc.run_to(3400);
		const Lines round_over = sc.phase("round over");
		sc.state.phase = net::MatchPhase::PreRound;
		sc.state.winner = 0;
		sc.run_to(3500);
		const Lines restart = sc.phase("restart");
		sc.input(ui::UiInputConcept::Activate);
		sc.run_to(3540);
		sc.state.end_of_round = false;
		sc.state.own_spawns = 4;
		sc.run_to(3600);
		const Lines restart_spawn = sc.phase("deploy after the restart");
		const Lines spawn_screens{ "push spawnscreenticketcountertdmscreen", "push spawnbuttonscreen",
			"push spawnscreenpc" };
		const Lines join_expected{ "native Spawn", "native PreRound.Waiting", "push chatlogscreen",
			"push spawnscreenticketcountertdmscreen", "push scoreboardtwoteamshud32screen", "push spawnbuttonscreen",
			"push spawnscreenpc", "deploy SelectSpawnPoint", "deploy GameInput", "deploy MouseInput",
			"deploy OnFrontendCameraChanged" };
		CHECK(join == join_expected);
		const Lines spawned_tail{ "deploy MouseInput", "deploy GameInput", "deploy StopScrollTextTimer",
			"deploy UnSpawnCustomizationVehicle", "deploy UnSpawnCustomization", "deploy OnFrontendCameraChanged",
			"deploy NotifyMinimapZoomLevelHasChanged" };
		Lines first_spawn_expected{ "deploy OnFrontendCameraChanged", "deploy Spawn", "native Playing",
			"push hudscreen", "close spawnscreenpc", "close spawnbuttonscreen",
			"close spawnscreenticketcountertdmscreen", "push preroundwaitingscreen", "push hudmpscreen",
			"push hudtdmscreen" };
		first_spawn_expected.insert(first_spawn_expected.end(), spawned_tail.begin(), spawned_tail.end());
		CHECK(first_spawn == first_spawn_expected);
		const Lines menu_expected{ "close hudscreen", "close hudmpscreen", "close hudtdmscreen", "push ingamemenump",
			"close scoreboardtwoteamshud32screen", "close tablistenerscreen", "deploy MouseInput",
			"deploy ActivateCommoRose", "deploy GameInput", "deploy MouseCursor", "deploy MouseInput",
			"deploy SetSelectedLevel", "deploy RestrictMouse", "deploy OnFrontendCameraChanged" };
		CHECK(menu == menu_expected);
		const Lines resume_expected{ "close ingamemenump", "deploy MouseInput", "deploy GameInput",
			"deploy OnFrontendCameraChanged", "push hudscreen", "push scoreboardtwoteamshud32screen",
			"push hudmpscreen", "push hudtdmscreen", "deploy MouseInput", "deploy GameInput",
			"deploy StopScrollTextTimer" };
		CHECK(resume == resume_expected);
		CHECK(countdown ==
				Lines{ "native PreRound.Countdown", "close preroundwaitingscreen", "push preroundwaitingscreen" });
		CHECK(started == Lines{ "native PreRound.RoundStarted", "close preroundwaitingscreen" });
		const Lines death_expected{ "native Died", "push chatscreen", "close chatscreen",
			"deploy NotifyMinimapZoomLevelHasChanged", "close hudscreen", "close hudmpscreen", "close hudtdmscreen",
			"push killscreen", "deploy GameInput", "close killscreen", "push spawnscreenticketcountertdmscreen",
			"push spawnbuttonscreen", "push spawnscreenpc", "deploy DefragMemory", "deploy SelectSpawnPoint",
			"deploy GameInput", "deploy MouseInput", "deploy OnFrontendCameraChanged" };
		CHECK(death == death_expected);
		const int64_t died = sc.at("native Died", 601);
		CHECK(died == 601);
		CHECK(sc.at("push killscreen", 601) - died == int64_t(sim::logic::logic_delay_ticks(1.0)));
		CHECK(sc.at("push spawnscreenticketcountertdmscreen", 601) - died ==
				int64_t(sim::logic::logic_delay_ticks(4.0)));
		const core::Result<core::gameplay::DeathTimeline> timeline =
				core::gameplay::read_death_timeline(l->level->db(), s.runtime().graph());
		REQUIRE_MESSAGE(timeline.ok(), (timeline.ok() ? std::string() : timeline.error().message));
		CHECK(timeline->kill_card_s == 1.0f);
		CHECK(timeline->spawn_screens_s == 4.0f);
		CHECK(timeline->kill_card_lifetime_s() == 3.0f);
		CHECK(sc.at("push killscreen", 601) - died == int64_t(sim::logic::logic_delay_ticks(timeline->kill_card_s)));
		CHECK(sc.at("close killscreen", 601) - died ==
				int64_t(sim::logic::logic_delay_ticks(timeline->spawn_screens_s)));
		Lines deploy_expected{ "deploy OnFrontendCameraChanged", "deploy Spawn", "native Playing", "push hudscreen",
			"close spawnscreenpc", "close spawnbuttonscreen", "close spawnscreenticketcountertdmscreen",
			"push hudmpscreen", "push hudtdmscreen" };
		deploy_expected.insert(deploy_expected.end(), spawned_tail.begin(), spawned_tail.end());
		CHECK(deploy == deploy_expected);
		Lines revive_expected{ "native Died", "push chatscreen", "close chatscreen",
			"deploy NotifyMinimapZoomLevelHasChanged", "close hudscreen", "close hudmpscreen", "close hudtdmscreen",
			"push killscreen", "deploy GameInput", "native Playing", "close killscreen", "push hudscreen",
			"push hudmpscreen", "push hudtdmscreen" };
		revive_expected.insert(revive_expected.end(), spawned_tail.begin(), spawned_tail.end());
		CHECK(revive == revive_expected);
		const Lines round_over_expected{ "native RoundOver.OnRoundOver 2", "close hudscreen", "close hudmpscreen",
			"close hudtdmscreen", "native EndOfRound", "deploy UnSpawnCustomizationVehicle",
			"deploy UnSpawnCustomization", "deploy OnFrontendCameraChanged", "push eorwinningteamscreen",
			"deploy OnFrontendCameraChanged", "close eorwinningteamscreen", "push scoreboardtwoteamshud64screen",
			"deploy OnFrontendCameraChanged", "close scoreboardtwoteamshud64screen", "push eordetailstabsscreen",
			"push eordetailsscoreboardscreen", "push scoreboardtwoteamsscreen", "deploy OnFrontendCameraChanged",
			"deploy MouseInput", "deploy GameInput", "deploy OnFrontendCameraChanged" };
		CHECK(round_over == round_over_expected);
		const int64_t eor = sc.at("native EndOfRound", 1701);
		const int64_t banner = sc.at("push eorwinningteamscreen", 1701);
		CHECK(eor == 1801);
		CHECK(banner - eor == int64_t(sim::logic::logic_delay_ticks(1.0)) + 2);
		CHECK(sc.at("push scoreboardtwoteamshud64screen", 1701) - banner ==
				int64_t(sim::logic::logic_delay_ticks(4.0)));
		CHECK(sc.at("push eordetailstabsscreen", 1701) - eor == int64_t(sim::logic::logic_delay_ticks(12.0)));
		Lines restart_expected{ "native Reload" };
		restart_expected.insert(restart_expected.end(), join_expected.begin(), join_expected.end());
		CHECK(restart == restart_expected);
		CHECK(restart_spawn == first_spawn_expected);
		CHECK(s.stats().reloads == 1);
		CHECK(std::count(sc.widget_events.begin(), sc.widget_events.end(),
					  "preroundwaitingscreen.HudAlertMessage_03 UIWidgetEventID_Show") == 3);
		for (const std::string &e : s.errors()) {
			std::printf("ui.flow.session error %s\n", e.c_str());
		}
		for (const std::string &e : s.flow().errors()) {
			std::printf("ui.flow.session flow error %s\n", e.c_str());
		}
		CHECK(s.errors().empty());
		CHECK(s.flow().errors().empty());
	}

	TEST_CASE("ui flow session: after a death, Deactivate in CUSTOMIZE goes back by the graphs (ACCESSORIES to "
			  "LOADOUT to KITS to the deploy screen) and on the deploy screen opens ingamemenump") {
		Level *l = level_or_skip();
		if (l == nullptr) {
			return;
		}
		core::Result<std::unique_ptr<UiFlowSession>> made = UiFlowSession::create(*l->level);
		REQUIRE_MESSAGE(made.ok(), (made.ok() ? std::string() : made.error().message));
		UiFlowSession &s = **made;
		ui_flow_tdm_pc_defaults(s, "TeamDeathMatch0");
		Script sc{ s };
		sc.state.phase = net::MatchPhase::InRound;
		sc.state.joined = true;
		sc.run_to(60);
		sc.state.own_spawns = 1;
		sc.run_to(120);
		sc.state.own_deaths = 1;
		sc.run_to(400);
		sc.phase("spawned and died");
		CHECK(s.flow().find_screen("ui/flow/screen/hudtdmscreen") == nullptr);
		CHECK(s.widget_fired("ui/flow/screen/spawnscreenpc", "TabBar_01", "UIWidgetEventID_OnItemReleased", "ID2"));
		sc.collect();
		sc.run_to(420);
		CHECK(sc.phase("customize tab") ==
				Lines{ "close spawnscreenpc", "push customizesoldierscreen", "close spawnscreenticketcountertdmscreen",
						"deploy GameInput", "deploy MouseInput", "deploy RequestWeaponStats",
						"deploy SpawnCustomization", "deploy OnFrontendCameraChanged" });
		CHECK(s.widget_fired(
				"ui/flow/screen/customizesoldierscreen", "KitView_01", "UIWidgetEventID_OnItemReleased", "0"));
		sc.collect();
		sc.run_to(440);
		CHECK(sc.phase("loadout") ==
				Lines{ "close customizesoldierscreen", "push customizesoldiersubscreen",
						"deploy SetWeaponCustomization", "deploy OnFrontendCameraChanged" });
		CHECK(s.widget_fired(
				"ui/flow/screen/customizesoldiersubscreen", "KitSelector_01", "UIWidgetEventID_OnItemReleased", "1"));
		sc.collect();
		sc.run_to(460);
		CHECK(sc.phase("accessories") ==
				Lines{ "close customizesoldiersubscreen", "push customizeaccessoriesscreen",
						"deploy OnFrontendCameraChanged" });
		const auto escape = [&](sim::Tick p_to) {
			CHECK(s.input_action(int32_t(ui::UiInputConcept::Deactivate)));
			sc.collect();
			sc.run_to(p_to);
		};
		escape(480);
		CHECK(sc.phase("Deactivate in ACCESSORIES") ==
				Lines{ "close customizeaccessoriesscreen", "push customizesoldiersubscreen",
						"deploy StorePrimaryWeaponAccessories", "deploy OnFrontendCameraChanged" });
		escape(500);
		CHECK(sc.phase("Deactivate in LOADOUT") ==
				Lines{ "close customizesoldiersubscreen", "push customizesoldierscreen",
						"deploy SetWeaponCustomization", "deploy OnFrontendCameraChanged" });
		CHECK(std::count(sc.widget_events.begin(), sc.widget_events.end(),
					  "customizesoldierscreen.KitView_01 UIWidgetEventID_OnItemHighlighted") == 2);
		escape(520);
		CHECK(sc.phase("Deactivate in KITS") ==
				Lines{ "close customizesoldierscreen", "close spawnbuttonscreen", "deploy UnSpawnCustomizationVehicle",
						"deploy UnSpawnCustomization", "push spawnscreenticketcountertdmscreen",
						"push spawnbuttonscreen", "push spawnscreenpc", "deploy SelectSpawnPoint", "deploy GameInput",
						"deploy MouseInput", "deploy OnFrontendCameraChanged" });
		escape(540);
		const Lines menu = sc.phase("Deactivate on the deploy screen");
		REQUIRE(menu.size() >= 3);
		CHECK(menu[0] == "close spawnscreenpc");
		CHECK(menu[1] == "close spawnbuttonscreen");
		CHECK(menu[2] == "push ingamemenump");
		CHECK(s.flow().find_screen("ui/flow/screen/ingamemenump") != nullptr);
		CHECK(s.errors().empty());
		CHECK(s.flow().errors().empty());
	}

	TEST_CASE("ui flow session: a client that never deploys gets EndOfRound from the match's round-end unspawn") {
		Level *l = level_or_skip();
		if (l == nullptr) {
			return;
		}
		UiFlowSessionOptions echo;
		echo.echo_networked = true;
		core::Result<std::unique_ptr<UiFlowSession>> made = UiFlowSession::create(*l->level, echo);
		REQUIRE_MESSAGE(made.ok(), (made.ok() ? std::string() : made.error().message));
		UiFlowSession &s = **made;
		ui_flow_tdm_pc_defaults(s, "TeamDeathMatch0");
		Script sc{ s };
		sc.state.phase = net::MatchPhase::InRound;
		sc.state.joined = true;
		sc.run_to(120);
		sc.phase("join, never deployed");
		sc.state.phase = net::MatchPhase::RoundOver;
		sc.state.winner = 1;
		sc.run_to(180);
		sc.state.end_of_round = true;
		sc.run_to(180 + sim::logic::logic_delay_ticks(13.0));
		const Lines over = sc.phase("round over without a spawn");
		const auto has = [&](std::string_view p_line) {
			return std::find(over.begin(), over.end(), p_line) != over.end();
		};
		CHECK(sc.at("native EndOfRound", 121) == 181);
		CHECK(has("close spawnbuttonscreen"));
		CHECK(has("push eorwinningteamscreen"));
		CHECK(has("push eordetailstabsscreen"));
		CHECK(s.errors().empty());
	}

	TEST_CASE("ui flow session: networked edges are delivered only from received events, as the echo of them would be; "
			  "the server's "
			  "ghost applies") {
		Level *l = level_or_skip();
		if (l == nullptr) {
			return;
		}
		UiFlowSessionOptions echo;
		echo.echo_networked = true;
		core::Result<std::unique_ptr<UiFlowSession>> made_echo = UiFlowSession::create(*l->level, echo);
		core::Result<std::unique_ptr<UiFlowSession>> made_fed = UiFlowSession::create(*l->level);
		core::Result<std::unique_ptr<UiFlowSession>> made_alone = UiFlowSession::create(*l->level);
		REQUIRE(made_echo.ok());
		REQUIRE(made_fed.ok());
		REQUIRE(made_alone.ok());
		Script a{ **made_echo }, b{ **made_fed }, c{ **made_alone };
		for (Script *sc : { &a, &b, &c }) {
			ui_flow_tdm_pc_defaults(sc->session, "TeamDeathMatch0");
		}
		uint64_t forwarded = 0;
		const auto run_to = [&](sim::Tick p_tick) {
			while (a.tick < p_tick) {
				for (Script *sc : { &a, &b, &c }) {
					++sc->tick;
				}
				REQUIRE(a.session.update(a.state).ok());
				a.collect();
				const std::vector<sim::logic::LogicNetworkedEvent> sent(
						a.session.runtime().networked().begin(), a.session.runtime().networked().end());
				forwarded += sent.size();
				b.session.receive_networked(sent);
				REQUIRE(b.session.update(b.state).ok());
				b.collect();
				REQUIRE(c.session.update(c.state).ok());
				c.collect();
			}
		};
		const auto set = [&](auto p_change) {
			for (Script *sc : { &a, &b, &c }) {
				p_change(sc->state);
			}
		};
		set([](UiGameState &r_s) {
			r_s.phase = net::MatchPhase::InRound;
			r_s.joined = true;
		});
		run_to(120);
		set([](UiGameState &r_s) {
			r_s.phase = net::MatchPhase::RoundOver;
			r_s.winner = 1;
		});
		run_to(180);
		set([](UiGameState &r_s) { r_s.end_of_round = true; });
		run_to(180 + sim::logic::logic_delay_ticks(13.0));
		const Lines over_echo = a.phase("networked by the echo");
		const Lines over_fed = b.phase("networked from received events");
		const Lines over_alone = c.phase("networked from nothing");
		CHECK(over_fed == over_echo);
		CHECK(b.timeline == a.timeline);
		CHECK(a.at("push eorwinningteamscreen", 121) > 0);
		CHECK(b.at("push eorwinningteamscreen", 121) == a.at("push eorwinningteamscreen", 121));
		CHECK(c.at("push eorwinningteamscreen", 121) < 0);
		const UiFlowSessionStats &sa = a.session.stats(), &sb = b.session.stats(), &sc = c.session.stats();
		std::printf("ui.flow.session networked: %llu fired, %llu echoed; fed %llu received, %llu delivered, %llu "
					"unmatched, %llu "
					"fired locally and not delivered\n",
				(unsigned long long)sa.networked_local, (unsigned long long)sa.networked_echoed,
				(unsigned long long)sb.networked_received, (unsigned long long)sb.networked_delivered,
				(unsigned long long)sb.networked_unmatched, (unsigned long long)sb.networked_local);
		CHECK(forwarded > 0);
		CHECK(sa.networked_echoed > 0);
		CHECK(sb.networked_received == forwarded);
		CHECK(sb.networked_delivered == sa.networked_echoed);
		CHECK(sb.networked_echoed == 0);
		CHECK(sb.networked_local == sa.networked_local);
		CHECK(sc.networked_echoed == 0);
		CHECK(sc.networked_delivered == 0);
		CHECK(sc.networked_local > 0);

		const sim::logic::LogicGhost ghost = a.session.runtime().ghost();
		CHECK(b.session.apply_ghost(ghost).ok());
		sim::logic::LogicGhost wrong = ghost;
		wrong.sequence_start.push_back(sim::logic::LogicGhost::kNoStart);
		CHECK_FALSE(b.session.apply_ghost(wrong).ok());
		CHECK(sb.ghosts_applied == 1);
		CHECK(sb.ghosts_refused == 1);
		CHECK(b.session.errors().empty());
	}

	TEST_CASE("ui flow session: mp_eor_fading's flash and fades follow the logic's Sequence, LVE and Fade events") {
		Level *l = level_or_skip();
		if (l == nullptr) {
			return;
		}
		UiFlowSessionOptions echo;
		echo.echo_networked = true;
		core::Result<std::unique_ptr<UiFlowSession>> made = UiFlowSession::create(*l->level, echo);
		REQUIRE_MESSAGE(made.ok(), (made.ok() ? std::string() : made.error().message));
		UiFlowSession &s = **made;
		ui_flow_tdm_pc_defaults(s, "TeamDeathMatch0");
		const LogicScreenEffectsStats &es = s.effects().stats();
		std::printf("ui.flow.session screen effects: %zu fades (%zu timed by a property), %zu visual environments (%zu "
					"by a sequence track)\n",
				es.fades, es.fade_times_from_properties, es.environments, es.tracked_visibilities);
		CHECK(es.fade_times_from_properties >= 1);
		CHECK(es.tracked_visibilities >= 1);
		CHECK(es.linked_visibilities >= 1);
		const std::string white = "fx/visualenviroments/fullscreen/ve_whitescreen";
		Script sc{ s };
		sc.state.phase = net::MatchPhase::InRound;
		sc.state.joined = true;
		sc.run_to(60);
		size_t from_runtime = 0;
		size_t unresolved = 0;
		for (const LogicEnvironmentState &e : s.screen_effects().environments) {
			from_runtime += e.source == LogicVisibilitySource::Runtime ? 1 : 0;
			unresolved += e.source == LogicVisibilitySource::Unresolved ? 1 : 0;
			if (e.visual_environment == "levels/frontend/lighting/visualenvironment_frontendcustomize") {
				CHECK(e.source != LogicVisibilitySource::Instance);
			}
		}
		std::printf("ui.flow.session linked visibilities: %zu, %zu read from the runtime, %zu unresolved\n",
				es.linked_visibilities, from_runtime, unresolved);
		CHECK(from_runtime + unresolved == es.linked_visibilities);
		sc.state.own_spawns = 1;
		sc.run_to(120);
		CHECK(s.screen_effects().environment(white) == nullptr);
		CHECK(s.screen_effects().screen_faded == 0.0f);
		sc.state.phase = net::MatchPhase::RoundOver;
		sc.state.winner = 1;
		const auto ticks = [](double p_s) { return sim::logic::logic_delay_ticks(p_s); };
		const sim::Tick over = 121;
		struct At {
			sim::Tick tick;
			LogicScreenEffectsFrame frame;
		};
		std::vector<At> seen;
		while (sc.tick < over + ticks(1.8)) {
			sc.run_to(sc.tick + 1);
			seen.push_back(At{ sc.tick, s.screen_effects() });
		}
		const auto at = [&](sim::Tick p_tick) -> const LogicScreenEffectsFrame & {
			for (const At &a : seen) {
				if (a.tick == p_tick) {
					return a.frame;
				}
			}
			FAIL("no frame at tick " << p_tick);
			return seen.front().frame;
		};
		sim::Tick enabled = 0;
		sim::Tick disabled = 0;
		for (const At &a : seen) {
			const bool on = a.frame.environment(white) != nullptr;
			if (on && enabled == 0) {
				enabled = a.tick;
			}
			if (!on && enabled != 0 && disabled == 0) {
				disabled = a.tick;
			}
		}
		std::printf("ui.flow.session ve_whitescreen enabled at %u, disabled at %u (round over at %u)\n",
				unsigned(enabled), unsigned(disabled), unsigned(over));
		REQUIRE(enabled != 0);
		REQUIRE(disabled != 0);
		const LogicEnvironmentState *half = at(enabled + ticks(0.375)).environment(white);
		REQUIRE(half != nullptr);
		CHECK(half->visibility == doctest::Approx(0.5).epsilon(0.05));
		CHECK(at(enabled + ticks(0.33)).ui_faded == doctest::Approx(1.0));
		CHECK(at(enabled + ticks(0.16)).ui_faded == doctest::Approx(0.5).epsilon(0.1));
		CHECK(at(enabled + ticks(0.75)).screen_faded == 0.0f);
		CHECK(at(enabled + ticks(1.0)).screen_faded == doctest::Approx(0.5).epsilon(0.1));
		CHECK(disabled - enabled == ticks(0.75) + ticks(0.5));
		CHECK(at(disabled).screen_faded == 1.0f);
		sc.state.end_of_round = true;
		const sim::Tick eor = sc.tick + 1;
		sc.run_to(eor + ticks(0.5));
		const float half_in = s.screen_effects().screen_faded;
		sc.run_to(eor + ticks(1.5));
		std::printf("ui.flow.session EndOfRound fade-in: %.2f after 0.5 s, %.2f after 1.5 s\n", double(half_in),
				double(s.screen_effects().screen_faded));
		CHECK(half_in > 0.0f);
		CHECK(half_in < 1.0f);
		CHECK(s.screen_effects().screen_faded == 0.0f);
		CHECK(s.screen_effects().ui_faded == 0.0f);
		sc.state.phase = net::MatchPhase::PreRound;
		sc.state.end_of_round = false;
		sc.run_to(sc.tick + 10);
		CHECK(s.stats().reloads == 1);
		CHECK(s.screen_effects().environment(white) == nullptr);
		CHECK(s.screen_effects().screen_faded == 0.0f);
		CHECK(s.screen_effects().ui_faded == 0.0f);
		CHECK(s.errors().empty());
	}

	TEST_CASE("ui flow session: a spawn made before the flow joins enters Playing after the spawn screens") {
		Level *l = level_or_skip();
		if (l == nullptr) {
			return;
		}
		core::Result<std::unique_ptr<UiFlowSession>> made = UiFlowSession::create(*l->level);
		REQUIRE_MESSAGE(made.ok(), (made.ok() ? std::string() : made.error().message));
		UiFlowSession &s = **made;
		ui_flow_tdm_pc_defaults(s, "TeamDeathMatch0");
		Script sc{ s };
		sc.state.phase = net::MatchPhase::InRound;
		sc.state.joined = true;
		sc.state.own_spawns = 1;
		sc.run_to(60);
		const Lines joined = sc.phase("joined already spawned");
		CHECK(sc.at("native Spawn", 0) == 1);
		CHECK(sc.at("native Playing", 0) == 2);
		CHECK(std::find(joined.begin(), joined.end(), "close spawnscreenpc") != joined.end());
		CHECK(s.flow().find_screen("ui/flow/screen/spawnscreenpc") == nullptr);
		CHECK(s.flow().find_screen("ui/flow/screen/hudmpscreen") != nullptr);
		CHECK(s.data("ui/uicomponents/usercomp", -1093247625) == "False");
		CHECK(s.errors().empty());
		CHECK(s.flow().errors().empty());
	}

	TEST_CASE("ui flow session: MapSize toggles uiminimapcomp IsSmall and MapZoom steps the zoom of the current size") {
		Level *l = level_or_skip();
		if (l == nullptr) {
			return;
		}
		core::Result<std::unique_ptr<UiFlowSession>> made = UiFlowSession::create(*l->level);
		REQUIRE_MESSAGE(made.ok(), (made.ok() ? std::string() : made.error().message));
		UiFlowSession &s = **made;
		ui_flow_tdm_pc_defaults(s, "TeamDeathMatch0");
		Script sc{ s };
		sc.state.phase = net::MatchPhase::InRound;
		sc.state.joined = true;
		sc.state.own_spawns = 1;
		sc.run_to(120);
		constexpr std::string_view comp = "ui/uicomponents/uiminimapcomp";
		constexpr int32_t is_small = -1358262034;
		constexpr int32_t small_zoom = -1127323954;
		constexpr int32_t large_zoom = -1095821268;
		const auto read = [&](int32_t p_key) { return s.data(comp, p_key).value_or("<none>"); };
		std::printf("ui.flow.session minimap before: IsSmall %s, small zoom %s, large zoom %s\n",
				read(is_small).c_str(), read(small_zoom).c_str(), read(large_zoom).c_str());
		CHECK(read(is_small) == "True");
		CHECK(read(small_zoom) == "<none>");
		CHECK(read(large_zoom) == "<none>");
		CHECK(s.flow().concept_screen(int32_t(ui::UiInputConcept::MapSize)).has_value());
		CHECK(s.flow().concept_screen(int32_t(ui::UiInputConcept::MapZoom)).has_value());
		const auto press = [&](ui::UiInputConcept p_concept, const char *p_label) {
			sc.input(p_concept);
			sc.run_to(sc.tick + 10);
			std::printf("ui.flow.session minimap after %s: IsSmall %s, small zoom %s, large zoom %s\n", p_label,
					read(is_small).c_str(), read(small_zoom).c_str(), read(large_zoom).c_str());
		};
		press(ui::UiInputConcept::MapZoom, "MapZoom on the small map");
		CHECK(read(is_small) == "True");
		CHECK(read(small_zoom) == "1");
		CHECK(read(large_zoom) == "<none>");
		press(ui::UiInputConcept::MapSize, "MapSize");
		CHECK(read(is_small) == "False");
		press(ui::UiInputConcept::MapZoom, "MapZoom on the large map");
		CHECK(read(small_zoom) == "1");
		CHECK(read(large_zoom) == "1");
		press(ui::UiInputConcept::MapSize, "MapSize again");
		CHECK(read(is_small) == "True");
		CHECK(s.errors().empty());
		CHECK(s.flow().errors().empty());
	}

	TEST_CASE("ui flow session: Restarting shows the pre-round box and the next pre-round reloads the logic") {
		Level *l = level_or_skip();
		if (l == nullptr) {
			return;
		}
		core::Result<std::unique_ptr<UiFlowSession>> made = UiFlowSession::create(*l->level);
		REQUIRE_MESSAGE(made.ok(), (made.ok() ? std::string() : made.error().message));
		UiFlowSession &s = **made;
		ui_flow_tdm_pc_defaults(s, "TeamDeathMatch0");
		Script sc{ s };
		sc.state.phase = net::MatchPhase::PreRound;
		sc.state.joined = true;
		sc.run_to(60);
		sc.input(ui::UiInputConcept::Activate);
		sc.run_to(100);
		sc.state.own_spawns = 1;
		sc.run_to(160);
		sc.state.phase = net::MatchPhase::InRound;
		sc.run_to(300);
		sc.phase("playing");
		sc.state.phase = net::MatchPhase::Restarting;
		sc.run_to(360);
		const Lines restarting = sc.phase("restarting");
		CHECK(restarting ==
				Lines{ "native PreRound.Restarting", "push preroundwaitingscreen", "close preroundwaitingscreen",
						"push preroundwaitingscreen" });
		CHECK(s.data("ui/uicomponents/uipreroundcomp", -566234449) == "ID_H_PREROUND_RESTARTING_TITLE");
		CHECK(s.data("ui/uicomponents/uipreroundcomp", -1675254140) == "ID_H_PREROUND_RESTARTING");
		CHECK(s.flow().find_screen(ui::kPreRoundWaitingScreenPartition) != nullptr);
		sc.state.phase = net::MatchPhase::PreRound;
		sc.state.own_spawns = 0;
		sc.state.own_deaths = 0;
		sc.run_to(420);
		const Lines reload = sc.phase("pre-round after restarting");
		REQUIRE_FALSE(reload.empty());
		CHECK(reload.front() == "native Reload");
		CHECK(s.stats().reloads == 1);
		CHECK(s.errors().empty());
		CHECK(s.flow().errors().empty());
	}

	TEST_CASE("ui flow session: a screen host reports enter and exit") {
		Level *l = level_or_skip();
		if (l == nullptr) {
			return;
		}
		UiFlowSessionOptions o;
		o.initialize_screens = false;
		core::Result<std::unique_ptr<UiFlowSession>> made = UiFlowSession::create(*l->level, o);
		REQUIRE_MESSAGE(made.ok(), (made.ok() ? std::string() : made.error().message));
		UiFlowSession &s = **made;
		ui_flow_tdm_pc_defaults(s, "TeamDeathMatch0");
		UiGameState st;
		st.phase = net::MatchPhase::PreRound;
		st.joined = true;
		const auto run = [&](int p_ticks) {
			for (int i = 0; i < p_ticks; ++i) {
				REQUIRE(s.update(st).ok());
			}
		};
		const auto names = [&](const std::vector<uint64_t> &p_ids) {
			Lines out;
			for (const uint64_t id : p_ids) {
				const ui::UiFlowScreen *f = s.flow().screen(id);
				out.push_back(f != nullptr ? ui::ui_data_category_leaf(f->partition) : std::string("?"));
			}
			return out;
		};
		run(60);
		const std::vector<uint64_t> joined = s.flow().take_opened();
		for (const std::string &n : names(joined)) {
			std::printf("ui.flow.session host opened %s\n", n.c_str());
		}
		CHECK(names(joined) ==
				Lines{ "chatlogscreen", "spawnscreenticketcountertdmscreen", "scoreboardtwoteamshud32screen",
						"spawnbuttonscreen", "spawnscreenpc" });
		for (const uint64_t id : joined) {
			CHECK_FALSE(s.flow().screen(id)->initialized);
		}
		CHECK(std::count(s.log().begin(), s.log().end(), "push emptyscreen") == 0);
		s.clear_log();
		for (const uint64_t id : joined) {
			s.screen_initialized(id);
			CHECK(s.flow().screen(id)->initialized);
		}
		for (const std::string &line : s.log()) {
			std::printf("ui.flow.session host initialized: %s\n", line.c_str());
		}
		CHECK(std::count(s.log().begin(), s.log().end(), "push emptyscreen") == 1);
		CHECK(std::count(s.log().begin(), s.log().end(), "push tablistenerscreen") == 1);
		CHECK(names(s.flow().take_opened()) == Lines{ "emptyscreen", "tablistenerscreen" });
		st.own_spawns = 1;
		run(60);
		const std::vector<uint64_t> closing = s.flow().take_closing();
		for (const std::string &n : names(closing)) {
			std::printf("ui.flow.session host closing %s\n", n.c_str());
		}
		CHECK(names(closing) == Lines{ "spawnscreenpc", "spawnbuttonscreen", "spawnscreenticketcountertdmscreen" });
		for (const uint64_t id : closing) {
			REQUIRE(s.flow().screen(id) != nullptr);
			CHECK(s.flow().screen(id)->exiting);
			s.screen_exited(id);
			CHECK(s.flow().screen(id) == nullptr);
		}
		CHECK(s.errors().empty());
		CHECK(s.flow().errors().empty());
	}

	TEST_CASE("ui flow session: a round that starts without a pre-round reloads the logic") {
		Level *l = level_or_skip();
		if (l == nullptr) {
			return;
		}
		core::Result<std::unique_ptr<UiFlowSession>> made = UiFlowSession::create(*l->level, UiFlowSessionOptions{});
		REQUIRE_MESSAGE(made.ok(), (made.ok() ? std::string() : made.error().message));
		UiFlowSession &s = **made;
		ui_flow_tdm_pc_defaults(s, "TeamDeathMatch0");
		UiGameState st;
		st.joined = true;
		for (const net::MatchPhase p :
				{ net::MatchPhase::InRound, net::MatchPhase::RoundOver, net::MatchPhase::InRound }) {
			st.phase = p;
			st.winner = p == net::MatchPhase::RoundOver ? 1 : 0;
			for (int i = 0; i < 10; ++i) {
				REQUIRE(s.update(st).ok());
			}
		}
		CHECK(s.stats().reloads == 1);
		CHECK(s.errors().empty());
	}

	TEST_CASE("ui flow session: OPTIONS walks optionsgraph: tabs, values, key binding, reset, Save settings?") {
		Level *l = level_or_skip();
		if (l == nullptr) {
			return;
		}
		core::Result<std::unique_ptr<UiFlowSession>> made = UiFlowSession::create(*l->level);
		REQUIRE_MESSAGE(made.ok(), (made.ok() ? std::string() : made.error().message));
		UiFlowSession &s = **made;
		ui_flow_tdm_pc_defaults(s, "TeamDeathMatch0");
		constexpr std::string_view kSettings = "ui/uicomponents/uisettingscomp";
		s.set_data(kSettings, ui::kOptionsSettingsChangedKey, "False");
		s.set_data(kSettings, ui::kOptionsVideoChangedKey, "False");
		s.set_data(kSettings, ui::kOptionsFullscreenKey, "False");
		s.set_data("ui/uicomponents/uimemcardcomp", ui::kOptionsCanSaveKey, "True");
		std::vector<UiFlowDataWrite> settings_writes;
		s.add_data_sink("uisettingscomp", [&](const UiFlowDataWrite &p_write) { settings_writes.push_back(p_write); });
		Script sc{ s };
		sc.state.phase = net::MatchPhase::InRound;
		sc.state.joined = true;
		sc.run_to(60);
		sc.input(ui::UiInputConcept::Activate);
		sc.state.own_spawns = 1;
		sc.run_to(200);
		sc.phase("options: spawned");
		const auto fire = [&](std::string_view p_partition, std::string_view p_instance, std::string_view p_query,
								  const std::string &p_value) {
			CHECK_MESSAGE(s.widget_fired(p_partition, p_instance, p_query, p_value), std::string(p_instance));
			sc.collect();
			sc.run_to(sc.tick + 10);
		};
		const auto press = [&](ui::UiInputConcept p_concept) {
			sc.input(p_concept);
			sc.run_to(sc.tick + 10);
		};
		const auto writes = [&] {
			Lines out;
			s.dispatch_data_writes();
			for (const UiFlowDataWrite &w : std::exchange(settings_writes, {})) {
				out.push_back(std::to_string(w.key) + "=" + w.value);
			}
			for (const std::string &w : out) {
				std::printf("ui.flow.session options write %s\n", w.c_str());
			}
			return out;
		};
		const auto steps = [](const Lines &p_lines) {
			Lines out;
			for (const std::string &line : p_lines) {
				if (line.starts_with("push ") || line.starts_with("close ") || line.starts_with("deploy ")) {
					out.push_back(line);
				}
			}
			return out;
		};
		s.dispatch_data_writes();
		settings_writes.clear();
		press(ui::UiInputConcept::Menu);
		sc.phase("options: menu");
		const std::string menu = "ui/flow/screen/ingamemenump";
		const std::string tab = "UIWidgetEventID_OnItemReleased";
		fire(menu, "List_01", tab, "ID_M_IGMMP_OPTIONS");
		CHECK(steps(sc.phase("options: OPTIONS")) ==
				Lines{ "close ingamemenump", "push optionscontrolsscreen", "deploy OnFrontendCameraChanged" });
		CHECK(writes() == Lines{ "1855787041=False", "968900879=ID0" });
		const std::array<std::string, 5> screens{ std::string(ui::kOptionsScreens[0]),
			std::string(ui::kOptionsScreens[1]), std::string(ui::kOptionsScreens[2]),
			std::string(ui::kOptionsScreens[3]), std::string(ui::kOptionsScreens[4]) };
		for (size_t i : { size_t(1), size_t(2), size_t(3) }) {
			fire(screens[i - 1], "TabBar_01", tab, "ID" + std::to_string(i));
			CHECK(steps(sc.phase("options: tab")) ==
					Lines{ "close " + ui::ui_data_category_leaf(screens[i - 1]),
							"push " + ui::ui_data_category_leaf(screens[i]), "deploy OnFrontendCameraChanged" });
			CHECK(writes() == Lines{ "968900879=ID" + std::to_string(i) });
		}
		fire(screens[3], "List_01", "UIWidgetEventID_OnChanged", "1");
		fire(screens[3], "List_01", "UIWidgetEventID_RThumb", "-1726867587");
		CHECK(writes() == Lines{ "-812298789=1", "-1996364377=-1726867587" });
		const auto has = [](const Lines &p_lines, const std::string &p_line) {
			return std::find(p_lines.begin(), p_lines.end(), p_line) != p_lines.end();
		};
		const std::string delay = "ui/flow/screen/popups/popupdelay";
		s.set_data(kSettings, ui::kOptionsFullscreenKey, "True");
		for (const bool keep : { false, true }) {
			CAPTURE(keep);
			sc.actions.clear();
			fire(screens[3], "Button_02", tab, "");
			const Lines applied = steps(sc.phase("options: APPLY FULLSCREEN RESOLUTION"));
			CHECK(has(applied, "push popupdelay"));
			CHECK(has(sc.actions, "ApplyVideoSettings APPLY"));
			CHECK(has(sc.actions, "StartChangeResolutionTimer"));
			const ui::UiFlowScreen *f = s.flow().find_screen(delay);
			REQUIRE(f != nullptr);
			CHECK(f->modal);
			CHECK(f->dialog_title == "ID_M_POPUP_KEEPRESOLUTION_TITLE");
			CHECK(f->dialog_text == "ID_M_POPUP_KEEPRESOLUTION_DESC");
			REQUIRE(f->dialog_buttons.size() == 2);
			CHECK(f->dialog_buttons[0].label == "ID_M_OPTIONS_YES");
			CHECK(f->dialog_buttons[1].label == "ID_M_OPTIONS_NO");
			sc.actions.clear();
			if (keep) {
				fire(delay, "PopupButtonsManager_01", tab, "ID_M_OPTIONS_YES");
			} else {
				press(ui::UiInputConcept::Cancel);
			}
			const Lines closed = steps(sc.phase(keep ? "options: keep the resolution" : "options: resolution timeout"));
			CHECK(has(closed, "close popupdelay"));
			CHECK(has(sc.actions, "ApplyVideoSettings REVERT") == !keep);
			CHECK(s.flow().find_screen(delay) == nullptr);
			CHECK(s.flow().find_screen(screens[3]) != nullptr);
		}
		s.set_data(kSettings, ui::kOptionsFullscreenKey, "False");
		writes();
		const std::string adjust = "ui/flow/screen/screenadjustscreen";
		fire(screens[3], "List_03", "UIWidgetEventID_OnItemReleased", "0");
		const Lines opened = steps(sc.phase("options: OPEN SCREEN ADJUST"));
		CHECK(has(opened, "push screenadjustscreen"));
		REQUIRE(s.flow().find_screen(adjust) != nullptr);
		fire(adjust, "ScreenAdjust_01", "UIWidgetEventID_OnNavigateLeft", "0.96");
		fire(adjust, "ScreenAdjust_01", "UIWidgetEventID_OnReachedBottom", "0.94");
		CHECK(writes() == Lines{ "1758844605=0.96", "-1649763324=0.94" });
		fire(adjust, "Button_01", "UIWidgetEventID_OnItemReleased", "");
		const Lines left = steps(sc.phase("options: leave SCREEN ADJUST"));
		CHECK(has(left, "close screenadjustscreen"));
		CHECK(s.flow().find_screen(adjust) == nullptr);
		CHECK(s.flow().find_screen(screens[3]) != nullptr);
		writes();
		fire(screens[3], "TabBar_01", tab, "ID4");
		sc.phase("options: KEY BINDINGS tab");
		CHECK(writes() == Lines{ "968900879=ID4", "70081798=ID0" });
		fire(screens[4], "TabBar_02", tab, "ID1");
		CHECK(writes() == Lines{ "70081798=ID1" });
		fire(screens[4], "List_01", "UIWidgetEventID_OnChanged", "0");
		fire(screens[4], "List_01", "UIWidgetEventID_OnItemReleased", "3");
		CHECK(steps(sc.phase("options: bind key")) ==
				Lines{ "push popupbindkey", "deploy EnterBindKeyMode", "deploy MouseInput" });
		CHECK(std::count(sc.actions.begin(), sc.actions.end(), "MouseInput False True") == 1);
		CHECK(writes() == Lines{ "-668488254=0", "1279238654=3" });
		s.set_data(kSettings, ui::kOptionsKeyBoundKey, "False");
		fire("ui/flow/screen/popups/popupbindkey", "ChangeListener_01", "UIWidgetEventID_OnChanged", "False");
		CHECK(steps(sc.phase("options: key pressed")) ==
				Lines{ "close popupbindkey", "deploy MouseInput", "deploy ConfirmKeyBind" });
		const std::string popup = "ui/flow/screen/popups/popupgeneric";
		const auto dialog = [&](const char *p_title, const char *p_text) {
			const ui::UiFlowScreen *f = s.flow().find_screen(popup);
			REQUIRE(f != nullptr);
			CHECK(f->dialog_title == p_title);
			CHECK(f->dialog_text == p_text);
			Lines out;
			for (const ui::UiDialogButton &b : f->dialog_buttons) {
				out.push_back(b.label + "=" + std::to_string(b.input_concept));
			}
			std::printf("ui.flow.session options dialog %s:", p_title);
			for (const std::string &b : out) {
				std::printf(" %s", b.c_str());
			}
			std::printf("\n");
			return f->dialog_buttons;
		};
		s.set_data(kSettings, ui::kOptionsSettingsChangedKey, "True");
		press(ui::UiInputConcept::Deactivate);
		CHECK(steps(sc.phase("options: leave")) == Lines{ "push popupgeneric" });
		std::vector<ui::UiDialogButton> buttons = dialog("ID_M_OPTIONS_OPTIONS", "ID_M_OPTIONS_SAVE_SETTINGS");
		REQUIRE(buttons.size() == 2);
		CHECK(buttons[0].label == "ID_M_OPTIONS_YES");
		CHECK(buttons[1].label == "ID_M_OPTIONS_NO");
		sc.actions.clear();
		fire(popup, "PopupButtonsManager_01", tab, buttons[0].label);
		const Lines yes = steps(sc.phase("options: YES"));
		CHECK(std::find(yes.begin(), yes.end(), "push ingamemenump") != yes.end());
		CHECK(std::count(sc.actions.begin(), sc.actions.end(), "OptionsBack SAVE") == 1);
		CHECK(std::count(sc.actions.begin(), sc.actions.end(), "OptionsBack REVERT") == 0);
		CHECK(writes() == Lines{ "1855787041=True" });
		fire(menu, "List_01", tab, "ID_M_IGMMP_OPTIONS");
		sc.phase("options: OPTIONS again");
		writes();
		press(ui::UiInputConcept::Deactivate);
		CHECK(steps(sc.phase("options: leave again")) == Lines{ "push popupgeneric" });
		buttons = dialog("ID_M_OPTIONS_OPTIONS", "ID_M_OPTIONS_SAVE_SETTINGS");
		REQUIRE(buttons.size() == 2);
		sc.actions.clear();
		fire(popup, "PopupButtonsManager_01", tab, buttons[1].label);
		const Lines no = steps(sc.phase("options: NO"));
		CHECK(std::find(no.begin(), no.end(), "push ingamemenump") != no.end());
		CHECK(std::count(sc.actions.begin(), sc.actions.end(), "OptionsBack REVERT") == 1);
		CHECK(std::count(sc.actions.begin(), sc.actions.end(), "OptionsBack SAVE") == 0);
		writes();
		s.set_data(kSettings, ui::kOptionsSettingsChangedKey, "False");
		fire(menu, "List_01", tab, "ID_M_IGMMP_OPTIONS");
		sc.phase("options: OPTIONS a third time");
		fire(screens[0], "Button_02", tab, "");
		CHECK(steps(sc.phase("options: reset")) == Lines{ "push popupgeneric" });
		buttons = dialog("ID_M_OPTIONS_RESET_TITLE", "ID_M_OPTIONS_RESET_DESC");
		REQUIRE_FALSE(buttons.empty());
		sc.actions.clear();
		fire(popup, "PopupButtonsManager_01", tab, buttons[0].label);
		CHECK(steps(sc.phase("options: reset YES")) ==
				Lines{ "close popupgeneric", "close optionscontrolsscreen", "push optionscontrolsscreen",
						"deploy ResetOptions", "deploy OnFrontendCameraChanged" });
		writes();
		sc.actions.clear();
		press(ui::UiInputConcept::Deactivate);
		const Lines unchanged = steps(sc.phase("options: leave unchanged"));
		CHECK(std::find(unchanged.begin(), unchanged.end(), "push popupgeneric") == unchanged.end());
		CHECK(std::find(unchanged.begin(), unchanged.end(), "push ingamemenump") != unchanged.end());
		CHECK(std::none_of(sc.actions.begin(), sc.actions.end(),
				[](const std::string &p_a) { return p_a.starts_with("OptionsBack"); }));
		CHECK(s.errors().empty());
		CHECK(s.flow().errors().empty());
		CHECK(s.stats().data_writes_dropped == 0);
	}

	TEST_CASE("ui flow session: every update drains the data-write journal, with or without a sink") {
		Level *l = level_or_skip();
		if (l == nullptr) {
			return;
		}
		core::Result<std::unique_ptr<UiFlowSession>> made = UiFlowSession::create(*l->level);
		REQUIRE_MESSAGE(made.ok(), (made.ok() ? std::string() : made.error().message));
		UiFlowSession &s = **made;
		ui_flow_tdm_pc_defaults(s, "TeamDeathMatch0");
		s.set_data("ui/uicomponents/uisettingscomp", ui::kOptionsSettingsChangedKey, "False");
		s.set_data("ui/uicomponents/uisettingscomp", ui::kOptionsVideoChangedKey, "False");
		Script sc{ s };
		sc.state.phase = net::MatchPhase::InRound;
		sc.state.joined = true;
		sc.run_to(60);
		sc.input(ui::UiInputConcept::Activate);
		sc.state.own_spawns = 1;
		sc.run_to(200);
		const std::string menu = "ui/flow/screen/ingamemenump";
		sc.input(ui::UiInputConcept::Menu);
		sc.run_to(sc.tick + 10);
		for (int i = 0; i < 3; ++i) {
			const uint64_t dispatched = s.stats().data_writes_dispatched;
			CHECK(s.widget_fired(menu, "List_01", "UIWidgetEventID_OnItemReleased", std::string("ID_M_IGMMP_OPTIONS")));
			sc.run_to(sc.tick + 1);
			CHECK(s.stats().data_writes_dispatched >= dispatched + 2);
			CHECK(s.pending_data_writes() == 0);
			sc.input(ui::UiInputConcept::Deactivate);
			sc.run_to(sc.tick + 10);
		}
		CHECK(s.stats().data_writes_dispatched >= 6);
		CHECK(s.stats().data_writes_peak < kUiFlowSessionMaxDataWrites);
		CHECK(s.stats().data_writes_dropped == 0);
		uint64_t heard = 0;
		const uint32_t sink = s.add_data_sink("uisettingscomp", [&](const UiFlowDataWrite &) { ++heard; });
		CHECK(s.widget_fired(menu, "List_01", "UIWidgetEventID_OnItemReleased", std::string("ID_M_IGMMP_OPTIONS")));
		sc.run_to(sc.tick + 1);
		CHECK(heard >= 2);
		s.remove_data_sink(sink);
		const uint64_t before = heard;
		sc.input(ui::UiInputConcept::Deactivate);
		sc.run_to(sc.tick + 10);
		CHECK(heard == before);
		CHECK(s.pending_data_writes() == 0);
	}

	TEST_CASE("ui flow session: RESUME in the menu opened on the deploy screen brings the deploy screens back") {
		Level *l = level_or_skip();
		if (l == nullptr) {
			return;
		}
		core::Result<std::unique_ptr<UiFlowSession>> made = UiFlowSession::create(*l->level);
		REQUIRE_MESSAGE(made.ok(), (made.ok() ? std::string() : made.error().message));
		UiFlowSession &s = **made;
		ui_flow_tdm_pc_defaults(s, "TeamDeathMatch0");
		Script sc{ s };
		sc.state.phase = net::MatchPhase::InRound;
		sc.state.joined = true;
		sc.run_to(60);
		sc.phase("join");
		const std::string menu = "ui/flow/screen/ingamemenump";
		if (!s.input_action(int32_t(ui::UiInputConcept::Deactivate))) {
			s.input_action(int32_t(ui::UiInputConcept::Menu));
		}
		sc.collect();
		sc.run_to(sc.tick + 10);
		const Lines opened = sc.phase("Escape on the deploy screen");
		const ui::UiFlowScreen *m = s.flow().find_screen(menu);
		REQUIRE(m != nullptr);
		CHECK(m->shown);
		sc.actions.clear();
		CHECK(s.widget_fired(menu, "List_01", "UIWidgetEventID_OnItemReleased", std::string("ID_M_IGMMP_RESUME")));
		sc.collect();
		sc.run_to(sc.tick + 10);
		const Lines resumed = sc.phase("RESUME");
		for (const std::string &a : sc.actions) {
			std::printf("  action %s\n", a.c_str());
		}
		const ui::UiFlowScreen *pc = s.flow().find_screen(ui::kSpawnScreenPcPartition);
		const ui::UiFlowScreen *button = s.flow().find_screen("ui/flow/screen/spawnbuttonscreen");
		CHECK(s.flow().find_screen(menu) == nullptr);
		REQUIRE(pc != nullptr);
		REQUIRE(button != nullptr);
		CHECK(pc->shown);
		CHECK(button->shown);
		CHECK(s.flow().concept_screen(int32_t(ui::UiInputConcept::Activate)) == pc->id);
		const auto has = [](const Lines &p_lines, std::string_view p_line) {
			return std::find(p_lines.begin(), p_lines.end(), p_line) != p_lines.end();
		};
		CHECK(has(opened, "close spawnbuttonscreen"));
		CHECK(has(opened, "push ingamemenump"));
		CHECK(has(resumed, "close ingamemenump"));
		CHECK(has(resumed, "push spawnbuttonscreen"));
		CHECK(has(resumed, "push spawnscreenpc"));
		constexpr int32_t kIsInMenu = -1093247625;
		CHECK(s.data("usercomp", kIsInMenu) == "True");
	}

	TEST_CASE("ui flow session: a hidden screen takes no input concept") {
		Level *l = level_or_skip();
		if (l == nullptr) {
			return;
		}
		core::Result<std::unique_ptr<UiFlowSession>> made = UiFlowSession::create(*l->level);
		REQUIRE(made.ok());
		UiFlowSession &s = **made;
		const core::EbxResolver &db = l->level->db();
		const core::gameplay::LogicGraph &g = s.runtime().graph();
		std::set<std::string> graphs;
		for (const uint32_t n : g.nodes_of_type(core::gameplay::LogicTypeId::UIGraph)) {
			const std::optional<core::EbxInstance> inst = logic_node_instance(db, g, n);
			const std::optional<core::EbxInstance> asset = inst ? db.follow(inst->data(), "GraphAsset") : std::nullopt;
			if (const std::optional<std::string_view> name =
							asset ? db.name_of(asset->partition().guid()) : std::nullopt) {
				graphs.insert(std::string(*name));
			}
		}
		std::vector<std::string> pending(graphs.begin(), graphs.end());
		size_t hide_targets = 0;
		size_t hide_targets_with_concepts = 0;
		size_t concept_states = 0;
		while (!pending.empty()) {
			const std::string name = pending.back();
			pending.pop_back();
			core::Result<ui::UiGraphAsset> a = ui::read_ui_graph_asset(db, name);
			if (!a) {
				continue;
			}
			for (const ui::UiGraphNode &n : a->nodes) {
				if (!n.screen.empty() && graphs.insert(n.screen).second) {
					pending.push_back(n.screen);
				}
				const bool takes = std::any_of(n.ports.begin(), n.ports.end(), [](const ui::UiGraphPort &p_port) {
					return p_port.role == ui::UiGraphPortRole::Output && p_port.input_action >= 0;
				});
				concept_states += takes ? 1 : 0;
			}
			for (const ui::UiGraphConnection &c : a->connections) {
				const ui::UiGraphNode &t = a->nodes[c.target_node];
				if (c.target_port >= t.ports.size() || t.ports[c.target_port].role != ui::UiGraphPortRole::Hide) {
					continue;
				}
				++hide_targets;
				hide_targets_with_concepts +=
						std::any_of(t.ports.begin(), t.ports.end(), [](const ui::UiGraphPort &p_port) {
							return p_port.role == ui::UiGraphPortRole::Output && p_port.input_action >= 0;
						});
			}
		}
		std::printf("ui.flow.session concepts: %zu graphs, %zu states take one, %zu Hide edges, %zu reach one\n",
				graphs.size(), concept_states, hide_targets, hide_targets_with_concepts);
		CHECK(concept_states > 0);
		CHECK(hide_targets > 0);
		CHECK(hide_targets_with_concepts == 0);
		ui_flow_tdm_pc_defaults(s, "TeamDeathMatch0");
		Script sc{ s };
		sc.state.phase = net::MatchPhase::InRound;
		sc.state.joined = true;
		size_t hidden_seen = 0;
		const auto no_hidden_target = [&] {
			for (const ui::UiFlowScreen &screen : s.flow().screens()) {
				hidden_seen += !screen.shown && !screen.exiting ? 1 : 0;
			}
			for (int32_t c = 0; c < ui::kUiInputConceptCount; ++c) {
				if (const std::optional<uint64_t> target = s.flow().concept_screen(c)) {
					REQUIRE(s.flow().screen(*target) != nullptr);
					CHECK(s.flow().screen(*target)->shown);
				}
			}
		};
		sc.run_to(60);
		no_hidden_target();
		sc.input(ui::UiInputConcept::Tab);
		sc.run_to(90);
		no_hidden_target();
		sc.input(ui::UiInputConcept::Menu);
		sc.run_to(120);
		no_hidden_target();
		std::printf("ui.flow.session hidden screens seen %zu\n", hidden_seen);
		CHECK(hidden_seen > 0);
	}

	TEST_CASE("ui flow session: install unchanged") {
		Level *l = level_or_skip();
		if (l == nullptr) {
			return;
		}
		const core::SnapshotDiff diff = core::diff_snapshots(l->mount->before, l->mount->reader.snapshot());
		std::printf("ui.flow.session install.unchanged %s\n", diff.clean() ? "yes" : diff.summary().c_str());
		CHECK(diff.clean());
	}
}
