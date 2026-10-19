// m2b.contract2 (docs/design/m2b.md §11, §2.6 #16-#18; written in W2.0 by the integrator, docs/design/m2b-w2.md):
// compiles every wave-2 interface header together and pins
//   * the HUD model's new size and the layouts of its four new structs (HudKillCard, HudEndOfRound, HudSpawnScreen,
//     HudPreRound), with the M2a part of HudModel unchanged in front of them (the new members follow `team`);
//   * the shapes the tracks share: HudKillCardSource, HudTdmFeedState, TdmHudRecords, the death camera's structs, all
//     trivially copyable; kHudMaxPlayers == net::kMaxPlayerRows (the M2b feed fills HudModel::players by slot);
//   * the W2.0 constants (kKillFeedSeconds, kKillCardSeconds, kHudSpawnSlots, kCrowdCorpseSeconds,
//     kCrowdReviveBlendSeconds, kCrowdDeathHideSeconds unchanged, kM2bPlayableDefaultBots, kM2bSpawnRequestRetryTicks),
//     the M2bCase / M2bTrack values and the check's case parser;
//   * the defaults of every field W2.0 added (an M2a frame, crowd and HUD stay M2a).
// No install, no session thread, no wall-clock row, no Godot (every header here is Godot-free).
//
// Pending rows (the W1.0 pattern, m2b_contract_tests.cpp): a row whose subject is a track's body runs only when its
// owner sets the switch below in the commit that lands the body; until then it prints "PENDING (<key>)" and checks
// what the contract alone defines. Calls of declared-only functions sit in `if constexpr` branches, so the test links
// before their bodies exist. cam's death_camera.cpp is Godot-free and lives in src/bf3_godot/src: the tests CMake
// block compiles it into this test when the file exists.

#include <doctest/doctest.h>

// Every wave-2 interface header, compiled together.
#include "bf3/anim/crowd_eval.h"
#include "bf3/core/ui/hud_model.h"
#include "bf3/host/hud_feed.h"
#include "bf3/host/kits.h"
#include "bf3/host/presentation_events.h"
#include "bf3/net/protocol.h"
#include "death_camera.h"
#include "game_crowd.h"
#include "hud_tdm.h"
#include "level_session_m2b.h"
#include "m2b_check.h"
#include "m2b_game.h"

#include "test_support.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <type_traits>
#include <vector>

using namespace bf3;
namespace ui = bf3::core::ui;
namespace gs = bf3::godot_side;

namespace {

// ---- the tracks' switches (each set to true by its owner, in the commit that lands the body) ------------------
constexpr bool kUiLanded = true; // ui: fill_hud_tdm (src/bf3_host/src/hud_feed_tdm.cpp)
constexpr bool kCamLanded = false; // cam: Bf3DeathCamera (src/bf3_godot/src/death_camera.cpp)

void pending(const char *p_key, const char *p_row, const char *p_reason) {
	std::printf("m2b.contract2: PENDING (%s): %s -- %s\n", p_key, p_row, p_reason);
	const std::string text = std::string("PENDING (") + p_key + "): " + p_row;
	MESSAGE(text);
}

// ---- shapes: the HUD model (§2.6 #16) -------------------------------------------------------------------------
// The sizes are MSVC x64's (the only toolchain openBF3 builds with, docs/testing.md); recorded at W2.0.
constexpr size_t kM2aHudModelSize = 8864; // sizeof(HudModel) on main 545640c: `team` ends at 8,860, alignment 8
constexpr size_t kM2bHudModelSize = 9024; // with the four M2b members (8,860 + 32 + 24 + 96 + 12)

static_assert(sizeof(ui::HudKillCard) == 32);
static_assert(offsetof(ui::HudKillCard, killer) == 5 && offsetof(ui::HudKillCard, item_id) == 12 &&
		offsetof(ui::HudKillCard, distance_m) == 20 && offsetof(ui::HudKillCard, seconds_left) == 28);
static_assert(sizeof(ui::HudEndOfRound) == 24);
static_assert(offsetof(ui::HudEndOfRound, team_kills) == 4 && offsetof(ui::HudEndOfRound, kill_limit) == 12 &&
		offsetof(ui::HudEndOfRound, top_players) == 16 && offsetof(ui::HudEndOfRound, seconds_to_next_round) == 20);
static_assert(sizeof(ui::HudSpawnScreen) == 96);
static_assert(offsetof(ui::HudSpawnScreen, can_deploy) == 3 && offsetof(ui::HudSpawnScreen, seconds_to_deploy) == 4 &&
		offsetof(ui::HudSpawnScreen, seconds_revive_left) == 12 && offsetof(ui::HudSpawnScreen, kit_items) == 16);
static_assert(sizeof(ui::HudPreRound) == 12);
static_assert(offsetof(ui::HudPreRound, seconds_left) == 4 && offsetof(ui::HudPreRound, input_restricted) == 8);
// The M2a part of HudModel is unchanged: the new members follow `team`, in this order.
static_assert(offsetof(ui::HudModel, kill_card) == offsetof(ui::HudModel, team) + sizeof(ui::HudTeam));
static_assert(offsetof(ui::HudModel, kill_card) == 8860 && (offsetof(ui::HudModel, kill_card) + 7) / 8 * 8 == kM2aHudModelSize);
static_assert(offsetof(ui::HudModel, end_of_round) == offsetof(ui::HudModel, kill_card) + sizeof(ui::HudKillCard));
static_assert(offsetof(ui::HudModel, spawn_screen) == offsetof(ui::HudModel, end_of_round) + sizeof(ui::HudEndOfRound));
static_assert(offsetof(ui::HudModel, pre_round) == offsetof(ui::HudModel, spawn_screen) + sizeof(ui::HudSpawnScreen));
static_assert(sizeof(ui::HudModel) == kM2bHudModelSize && alignof(ui::HudModel) == 8);
static_assert(std::is_trivially_copyable_v<ui::HudModel>);
static_assert(std::is_trivially_copyable_v<ui::HudKillCard> && std::is_trivially_copyable_v<ui::HudEndOfRound> &&
		std::is_trivially_copyable_v<ui::HudSpawnScreen> && std::is_trivially_copyable_v<ui::HudPreRound>);
static_assert(ui::kHudModelVersion == 1); // not bumped: the M2a fields keep their offsets (m2b-w2.md §3)
static_assert(ui::kHudMaxPlayers == net::kMaxPlayerRows); // HudModel::players[slot]
static_assert(ui::kHudKitCount == host::kKitCount && ui::kHudTopPlayers == 3 && ui::kHudSpawnKitItems == 5);

// ---- shapes: the feed, the screens, the camera, the M2b game ----------------------------------------------------

static_assert(std::is_trivially_copyable_v<host::HudKillCardSource>);
static_assert(std::is_trivially_copyable_v<host::HudTdmFeedState>);
static_assert(std::is_trivially_copyable_v<gs::TdmHudRecords>);
static_assert(std::is_trivially_copyable_v<gs::DeathCameraParams> && std::is_trivially_copyable_v<gs::DeathCameraStart> &&
		std::is_trivially_copyable_v<gs::DeathCameraView> && std::is_trivially_copyable_v<gs::DeathCameraStats>);
static_assert(std::is_trivially_copyable_v<anim::CrowdSoldierInput>);
static_assert(std::is_trivially_copyable_v<gs::M2bFrame>);
static_assert(!std::is_copy_constructible_v<gs::Bf3TdmHud> && !std::is_copy_constructible_v<gs::M2bCheck>);
static_assert(std::is_same_v<decltype(&gs::Bf3GameCrowd::set_corpse_seconds), void (gs::Bf3GameCrowd::*)(float)>);
static_assert(std::is_same_v<decltype(&host::fill_hud_tdm),
		void (*)(const host::HudFeedInput &, std::span<const host::PresentEvent>, const host::HudStatic &, host::HudTdmFeedState &, ui::HudModel &)>);
static_assert(std::is_same_v<decltype(&gs::Bf3DeathCamera::step),
		gs::DeathCameraView (gs::Bf3DeathCamera::*)(double, const fx::Vec3 *, const sim::CollisionWorld *)>);

// ---- constants and enum values ------------------------------------------------------------------------------------

static_assert(host::kKillFeedSeconds == 5.0 && host::kKillCardSeconds == 4.0);
static_assert(host::kHudSpawnSlots == std::array<uint8_t, 5>{ 0, 1, 2, 6, 7 });
static_assert(host::kHudSpawnSlots[2] == host::kKitGadgetSlot && host::kHudSpawnSlots[0] == host::kKitSlots[0] &&
		host::kHudSpawnSlots[1] == host::kKitSlots[1] && host::kHudSpawnSlots[3] == host::kKitSlots[2] && host::kHudSpawnSlots[4] == host::kKitSlots[3]);
static_assert(anim::kCrowdDeathHideSeconds == 3.0f); // M2a's stays
static_assert(anim::kCrowdCorpseSeconds == 15.0f && anim::kCrowdReviveBlendSeconds == 0.5f);
static_assert(gs::kM2bPlayableDefaultBots == 31 && gs::kM2bSpawnRequestRetryTicks == 30);
static_assert(uint8_t(gs::M2bCase::Play) == 0 && gs::kM2bCaseCount == 1);
static_assert(gs::m2b_case_name(gs::M2bCase::Play) == "play" && gs::m2b_case_name(gs::M2bCase(7)) == "?");
static_assert(gs::m2b_case_from_name("play") == gs::M2bCase::Play && !gs::m2b_case_from_name("Play").has_value());
static_assert(uint8_t(gs::M2bTrack::Gx) == 0 && uint8_t(gs::M2bTrack::Ui) == 1 && uint8_t(gs::M2bTrack::Cam) == 2 &&
		gs::kM2bTrackNames[1] == "ui" && gs::kM2bTrackNames[2] == "cam");
static_assert(uint8_t(host::PresentKind::Death) == 6 && uint8_t(host::PresentKind::Spawn) == 8 && uint8_t(host::PresentKind::Revive) == 9);
static_assert(host::present_flags::kOwnVictim == 8 && host::present_flags::kOwnShooter == 16 && host::present_flags::kRevived == 32);

} // namespace

TEST_SUITE("m2b-contract2") {
	TEST_CASE("m2b.contract2: the defaults of every W2.0 field keep M2a") {
		const ui::HudModel m{};
		CHECK(!m.kill_card.visible);
		CHECK(m.kill_card.serial == 0);
		CHECK(m.kill_card.killer == ui::kHudNoPlayer);
		CHECK(m.kill_card.killer_kit == -1);
		CHECK(m.kill_card.distance_m < 0.0f);
		CHECK(!m.end_of_round.visible);
		CHECK(m.end_of_round.winner == 0);
		for (uint8_t p : m.end_of_round.top_players) {
			CHECK(p == ui::kHudNoPlayer);
		}
		CHECK(!m.spawn_screen.visible);
		CHECK(!m.spawn_screen.can_deploy);
		CHECK(m.spawn_screen.selected_kit == 0);
		for (const auto &kit : m.spawn_screen.kit_items) {
			for (uint32_t item : kit) {
				CHECK(item == 0);
			}
		}
		CHECK(!m.pre_round.visible);
		CHECK(!m.pre_round.input_restricted);

		const host::HudFeedInput in{};
		CHECK(!in.m2b);
		CHECK(!in.match_valid);
		CHECK(in.players.empty());
		CHECK(in.own_slot == 0xFF);
		CHECK(in.own_team == 0);
		CHECK(in.kits == nullptr);
		CHECK(!in.scoreboard_held);
		CHECK(!in.kill_card_source.killer_seen);
		CHECK(in.kill_card_source.distance_m < 0.0f);

		const host::HudTdmFeedState st{};
		CHECK(st.kill_serial == 0);
		CHECK(st.own_deaths == 0);
		CHECK(st.bad_inputs == 0);

		const anim::CrowdSoldierInput c{};
		CHECK(!c.revived);
		CHECK(!c.dead);
		const anim::CrowdStanceSet s{};
		CHECK(s.corpse_seconds == anim::kCrowdDeathHideSeconds);

		const gs::DeathCameraParams dp{};
		CHECK(dp.seconds == float(host::kKillCardSeconds));
		CHECK(dp.orbit_seconds + dp.turn_seconds < dp.seconds);
		CHECK(dp.camera_radius_m > 0.0f);
		const gs::DeathCameraStart ds{};
		CHECK(ds.killer_slot == 0xFF);
		CHECK(ds.killer_camera);
		const gs::DeathCameraView dv{};
		CHECK(!dv.active);
		CHECK(!dv.finished);

		const gs::TdmHudRecords r{};
		CHECK(r.missing_fonts == 0);
		CHECK(r.missing_icons == 0);
		CHECK(r.missing_strings == 0);
		CHECK(!r.spawn_screen_visible);

		const gs::M2bOptions o{};
		CHECK(o.deploy_screen);
		CHECK(o.kill_limit == 0);
		CHECK(o.preround_s < 0.0f);
		CHECK(o.bots == 31);
		const gs::M2bGame g{};
		CHECK(!g.enabled);
		CHECK(!g.checks());
		CHECK(g.tdm_hud == nullptr);
		CHECK(g.death_camera == nullptr);
		CHECK(g.check == nullptr);
		CHECK(g.frame.m2a == nullptr);
		CHECK(g.frame.match == nullptr);
		const gs::M2bCheckOptions co{};
		CHECK(co.play_seconds == 90.0);
	}

	TEST_CASE("m2b.contract2: the M2b check's case parser refuses hostile command lines") {
		const auto ok = gs::parse_m2b_cases("play");
		REQUIRE(ok.ok());
		REQUIRE(ok->size() == 1);
		CHECK((*ok)[0] == gs::M2bCase::Play);
		const std::string long_arg(4096, 'p'); // named: a temporary in the range-init would die before the loop body (C++20)
		for (const char *bad : { "", "Play", "play,play", "play,", ",play", "all", "play,fx", long_arg.c_str() }) {
			CHECK_MESSAGE(!gs::parse_m2b_cases(bad).ok(), bad);
		}
	}

	TEST_CASE("m2b.contract2: ui's fill_hud_tdm (pending until ui lands)") {
		if constexpr (!kUiLanded) {
			pending("ui", "fill_hud_tdm: the kill feed, kill card and spawn screen from a recorded death",
					"src/bf3_host/src/hud_feed_tdm.cpp is ui's");
		} else {
			// One own death by slot 5 (team 2) while the own soldier (slot 3, team 1) has a life; the match InRound.
			std::array<net::PlayerRow, 2> rows{};
			rows[0].slot = 3;
			rows[0].team = 1;
			rows[0].alive = false;
			rows[1].slot = 5;
			rows[1].team = 2;
			rows[1].alive = true;
			rows[1].kit = 2;
			rows[1].kills = 1;
			rows[1].score = 100;
			host::HudFeedInput in{};
			in.m2b = true;
			in.alive = false;
			in.match_valid = true;
			in.match.phase = net::MatchPhase::InRound;
			in.match.kills = { 0, 1 };
			in.match.kill_limit = 100;
			in.players = rows;
			in.own_slot = 3;
			in.own_team = 1;
			in.server_tick = 1000;
			in.own_respawn_tick = 1300;
			in.own_revive_until = 1780;
			in.spawned_this_round = true;
			in.spawn_kit = 1;
			in.now_s = 10.0;
			in.kill_card_source.killer_seen = true;
			in.kill_card_source.distance_m = 12.5f;
			host::PresentEvent death{};
			death.kind = host::PresentKind::Death;
			death.timeline = host::PresentTimeline::Render;
			death.slot = 5;
			death.aux = 3;
			death.flags = host::present_flags::kOwnVictim | host::present_flags::kHeadshot;
			death.value = 64.0f;
			death.extra = 1300;
			death.seed = 1780;
			const host::HudStatic st{};
			host::HudTdmFeedState state{};
			ui::HudModel m{};
			host::fill_hud_tdm(in, std::span<const host::PresentEvent>(&death, 1), st, state, m);
			CHECK(m.messages.kill_message.newest_serial == 1);
			CHECK(m.kill_card.visible);
			CHECK(m.kill_card.serial == 1);
			CHECK(m.kill_card.killer == 5);
			CHECK(m.kill_card.killer_kit == 2);
			CHECK(m.kill_card.headshot);
			CHECK(m.kill_card.killer_health == doctest::Approx(64.0f));
			CHECK(m.kill_card.distance_m == doctest::Approx(12.5f));
			CHECK(!m.spawn_screen.visible); // the kill card is up
			CHECK(m.players[5].team == 2);
			CHECK(m.local_player == 3);
			CHECK(m.kill_counter.team2_kills == 1);
			CHECK(m.kill_counter.team1_max_kills == 100);
			// After the kill card: the spawn screen, not yet deployable, revive possible.
			in.now_s = 10.0 + host::kKillCardSeconds + 0.1;
			in.server_tick = 1250;
			host::fill_hud_tdm(in, {}, st, state, m);
			CHECK(!m.kill_card.visible);
			CHECK(m.spawn_screen.visible);
			CHECK(!m.spawn_screen.at_join);
			CHECK(m.spawn_screen.selected_kit == 1);
			CHECK(!m.spawn_screen.can_deploy);
			CHECK(m.spawn_screen.seconds_to_deploy == doctest::Approx(50.0f / 60.0f));
			CHECK(m.spawn_screen.revive_possible);
			in.server_tick = 1300;
			host::fill_hud_tdm(in, {}, st, state, m);
			CHECK(m.spawn_screen.can_deploy);
			// RoundOver: the round end instead of the spawn screen.
			in.match.phase = net::MatchPhase::RoundOver;
			in.match.winner = 2;
			host::fill_hud_tdm(in, {}, st, state, m);
			CHECK(m.end_of_round.visible);
			CHECK(m.end_of_round.winner == 2);
			CHECK(!m.end_of_round.local_won);
			CHECK(!m.spawn_screen.visible);
			// m2b off: every M2b screen hidden.
			in.m2b = false;
			host::fill_hud_tdm(in, {}, st, state, m);
			CHECK(!m.end_of_round.visible);
			CHECK(!m.spawn_screen.visible);
			CHECK(!m.kill_card.visible);
			CHECK(!m.pre_round.visible);
		}
	}

	TEST_CASE("m2b.contract2: cam's Bf3DeathCamera (pending until cam lands)") {
		if constexpr (!kCamLanded) {
			pending("cam", "Bf3DeathCamera: the corpse view, the turn to the killer, the end", "src/bf3_godot/src/death_camera.cpp is cam's");
		} else {
			gs::Bf3DeathCamera cam;
			CHECK(!cam.active());
			gs::DeathCameraStart start;
			start.corpse = { 0.0f, 0.0f, 0.0f };
			start.eye = { 0.0f, 1.6f, 0.0f };
			start.yaw_rad = 0.0f;
			start.killer_slot = 5;
			start.killer_camera = true;
			cam.begin(start, 100.0);
			CHECK(cam.active());
			const fx::Vec3 killer{ 20.0f, 1.0f, 0.0f };
			const gs::DeathCameraView early = cam.step(100.1, &killer, nullptr);
			CHECK(early.active);
			CHECK(!early.finished);
			const gs::DeathCameraView turned = cam.step(100.0 + 3.5, &killer, nullptr);
			CHECK(turned.on_killer);
			const fx::Vec3 f = turned.camera.forward;
			const fx::Vec3 to{ killer.x - turned.camera.trans.x, killer.y - turned.camera.trans.y, killer.z - turned.camera.trans.z };
			const float len = std::sqrt(to.x * to.x + to.y * to.y + to.z * to.z);
			REQUIRE(len > 0.0f);
			CHECK((f.x * to.x + f.y * to.y + f.z * to.z) / len > 0.9f); // looks at the killer
			const gs::DeathCameraView done = cam.step(100.0 + 4.1, &killer, nullptr);
			CHECK(done.finished);
			cam.end();
			CHECK(!cam.active());
			// A non-finite corpse ends the view and is counted, never a NaN transform.
			start.corpse.x = std::nanf("");
			cam.begin(start, 200.0);
			const gs::DeathCameraView bad = cam.step(200.1, nullptr, nullptr);
			CHECK(!bad.active);
			CHECK(cam.stats().bad_inputs >= 1);
		}
	}
}
