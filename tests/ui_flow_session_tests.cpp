#include <doctest/doctest.h>

#include "bf3/core/install_snapshot.h"
#include "bf3/core/level/level.h"
#include "bf3/core/ui/ui_input.h"
#include "bf3/host/ui_flow_session.h"
#include "test_support.h"

#include <algorithm>
#include <cstdio>
#include <memory>
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

	void collect() {
		for (const std::string &l : session.log()) {
			if (is_step(l)) {
				timeline.emplace_back(tick, l);
			}
		}
		session.clear_log();
		for (const ui::UiFlowAction &a : session.take_actions()) {
			timeline.emplace_back(tick, "deploy " + a.name);
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

void tdm_pc_settings(UiFlowSession &r_session) {
	r_session.set_data("ui/uicomponents/uisettingscomp", -668821714, "win32");
	r_session.set_data("ui/uicomponents/uisettingscomp", 1733092213, "TeamDeathMatch0");
	r_session.set_data("ui/uicomponents/uisettingscomp", 1870459195, "TeamDeathMatch");
	r_session.set_data("ui/uicomponents/uispawnpointcomp", -990983020, "True");
	r_session.set_data("ui/uicomponents/uispawnpointcomp", -225037516, "True");
	r_session.set_data("ui/uicomponents/uispectatorcomp", -366910738, "False");
	r_session.set_data("ui/uicomponents/uiendofroundcomp", -1857162541, "False");
	r_session.set_data("ui/uicomponents/uiendofroundcomp", 1450248605, "False");
	r_session.set_data("ui/uicomponents/uisoldiercomp", 347110003, "True");
	r_session.set_data("ui/uicomponents/uikitcomp", 1985842909, "0");
	r_session.set_data("ui/uicomponents/uispawnlogiccomp", -1005581981, "0");
}

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
		tdm_pc_settings(s);
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
		sc.run_to(1000);
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
		CHECK(sc.at("push eordetailstabsscreen", 1701) - banner == int64_t(sim::logic::logic_delay_ticks(12.0)) - 1);
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
