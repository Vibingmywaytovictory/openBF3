#include <doctest/doctest.h>

#include "bf3/core/gameplay/death_timeline.h"
#include "bf3/core/install_snapshot.h"
#include "bf3/core/level/level.h"
#include "bf3/core/ui/hud_screen.h"
#include "bf3/core/ui/options_model.h"
#include "bf3/core/ui/ui_input.h"
#include "bf3/host/ui_flow_session.h"
#include "test_support.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>
#include <optional>
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
		core::Result<std::unique_ptr<UiFlowSession>> made = UiFlowSession::create(*l->level);
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
		sc.state.unspawned = true;
		sc.run_to(3400);
		const Lines round_over = sc.phase("round over");
		sc.state.phase = net::MatchPhase::PreRound;
		sc.state.unspawned = false;
		sc.state.winner = 0;
		sc.run_to(3500);
		const Lines restart = sc.phase("restart");
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
			"deploy NotifyMinimapZoomLevelHasChanged", "push killscreen", "deploy GameInput", "close killscreen",
			"push spawnscreenticketcountertdmscreen", "push spawnbuttonscreen", "push spawnscreenpc",
			"deploy DefragMemory", "deploy SelectSpawnPoint", "deploy GameInput", "deploy MouseInput",
			"deploy OnFrontendCameraChanged" };
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
		Lines deploy_expected{ "deploy OnFrontendCameraChanged", "deploy Spawn", "native Playing",
			"close spawnscreenpc", "close spawnbuttonscreen", "close spawnscreenticketcountertdmscreen" };
		deploy_expected.insert(deploy_expected.end(), spawned_tail.begin(), spawned_tail.end());
		CHECK(deploy == deploy_expected);
		Lines revive_expected{ "native Died", "push chatscreen", "close chatscreen",
			"deploy NotifyMinimapZoomLevelHasChanged", "push killscreen", "deploy GameInput", "native Playing",
			"close killscreen" };
		revive_expected.insert(revive_expected.end(), spawned_tail.begin(), spawned_tail.end());
		CHECK(revive == revive_expected);
		const Lines round_over_expected{ "native RoundOver.OnRoundOver 2", "native EndOfRound",
			"deploy UnSpawnCustomizationVehicle", "deploy UnSpawnCustomization", "deploy OnFrontendCameraChanged",
			"push eorwinningteamscreen", "deploy OnFrontendCameraChanged", "push scoreboardtwoteamshud64screen",
			"deploy OnFrontendCameraChanged", "push eordetailstabsscreen", "push eordetailsscoreboardscreen",
			"push scoreboardtwoteamsscreen", "deploy OnFrontendCameraChanged", "deploy MouseInput", "deploy GameInput",
			"deploy OnFrontendCameraChanged" };
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
		CHECK(s.stats().reloads == 1);
		CHECK(std::count(sc.widget_events.begin(), sc.widget_events.end(),
					  "preroundwaitingscreen.HudAlertMessage_03 UIWidgetEventID_Show") == 2);
		for (const std::string &e : s.errors()) {
			std::printf("ui.flow.session error %s\n", e.c_str());
		}
		for (const std::string &e : s.flow().errors()) {
			std::printf("ui.flow.session flow error %s\n", e.c_str());
		}
		CHECK(s.errors().empty());
		CHECK(s.flow().errors().empty());
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
			for (const UiFlowDataWrite &w : s.take_data_writes()) {
				if (w.category == "uisettingscomp") {
					out.push_back(std::to_string(w.key) + "=" + w.value);
				}
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
		s.take_data_writes();
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
