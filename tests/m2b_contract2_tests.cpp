#include <doctest/doctest.h>

#include "bf3/anim/crowd_eval.h"
#include "bf3/core/ui/hud_model.h"
#include "bf3/core/ui/hud_screen.h"
#include "bf3/host/hud_feed.h"
#include "bf3/host/kits.h"
#include "bf3/host/presentation_events.h"
#include "bf3/net/protocol.h"
#include "death_camera.h"
#include "game_crowd.h"
#include "hud_tdm.h"
#include "level_session_tdm.h"
#include "m2b_check.h"
#include "tdm_flow.h"

#include "test_support.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

using namespace bf3;
namespace ui = bf3::core::ui;
namespace gs = bf3::godot_side;

namespace {

constexpr bool kUiLanded = true;
constexpr bool kCamLanded = true;

void pending(const char *p_key, const char *p_row, const char *p_reason) {
	std::printf("m2b.contract2: PENDING (%s): %s -- %s\n", p_key, p_row, p_reason);
	const std::string text = std::string("PENDING (") + p_key + "): " + p_row;
	MESSAGE(text);
}

constexpr size_t kM2aHudModelSize = 9016;
constexpr size_t kHudModelSize = 53920;

static_assert(sizeof(ui::HudKillCard) == 776 && offsetof(ui::HudKillCard, killer_name) == 32);
static_assert(offsetof(ui::HudKillCard, killer) == 5 && offsetof(ui::HudKillCard, item_id) == 12 &&
		offsetof(ui::HudKillCard, distance_m) == 20 && offsetof(ui::HudKillCard, seconds_left) == 28);
static_assert(sizeof(ui::HudEndOfRound) == 284 && offsetof(ui::HudEndOfRound, stage) == 24);
static_assert(offsetof(ui::HudEndOfRound, team_kills) == 4 && offsetof(ui::HudEndOfRound, kill_limit) == 12 &&
		offsetof(ui::HudEndOfRound, top_players) == 16 && offsetof(ui::HudEndOfRound, seconds_to_next_round) == 20);
static_assert(sizeof(ui::HudSpawnScreen) == 96);
static_assert(offsetof(ui::HudSpawnScreen, can_deploy) == 3 && offsetof(ui::HudSpawnScreen, seconds_to_deploy) == 4 &&
		offsetof(ui::HudSpawnScreen, seconds_revive_left) == 12 && offsetof(ui::HudSpawnScreen, kit_items) == 16);
static_assert(sizeof(ui::HudPreRound) == 20 && offsetof(ui::HudPreRound, count_left) == 12);
static_assert(offsetof(ui::HudPreRound, seconds_left) == 4 && offsetof(ui::HudPreRound, input_restricted) == 8);

static_assert(offsetof(ui::HudModel, kill_card) == offsetof(ui::HudModel, team) + sizeof(ui::HudTeam));
static_assert(offsetof(ui::HudModel, kill_card) == 9012 && (offsetof(ui::HudModel, kill_card) + 7) / 8 * 8 == kM2aHudModelSize);
static_assert(offsetof(ui::HudModel, end_of_round) == offsetof(ui::HudModel, kill_card) + sizeof(ui::HudKillCard));
static_assert(offsetof(ui::HudModel, spawn_screen) == offsetof(ui::HudModel, end_of_round) + sizeof(ui::HudEndOfRound));
static_assert(offsetof(ui::HudModel, pre_round) == offsetof(ui::HudModel, spawn_screen) + sizeof(ui::HudSpawnScreen));
static_assert(offsetof(ui::HudModel, damage) == offsetof(ui::HudModel, pre_round) + sizeof(ui::HudPreRound));
static_assert(sizeof(ui::HudDamageHit) == 16 && sizeof(ui::HudDamageFeed) == 4 + 16 * ui::kHudDamageFeedSize);
static_assert(offsetof(ui::HudModel, kit) == offsetof(ui::HudModel, damage) + sizeof(ui::HudDamageFeed));
static_assert(sizeof(ui::HudModel) == kHudModelSize && alignof(ui::HudModel) == 8);
static_assert(std::is_trivially_copyable_v<ui::HudModel>);
static_assert(std::is_trivially_copyable_v<ui::HudKillCard> && std::is_trivially_copyable_v<ui::HudEndOfRound> &&
		std::is_trivially_copyable_v<ui::HudSpawnScreen> && std::is_trivially_copyable_v<ui::HudPreRound>);
static_assert(ui::kHudModelVersion == 1);
static_assert(ui::kHudMaxPlayers == net::kMaxPlayerRows);
static_assert(ui::kHudKitCount == host::kKitCount && ui::kHudTopPlayers == 3 && ui::kHudSpawnKitItems == 5);

static_assert(std::is_trivially_copyable_v<host::HudKillCardSource>);
static_assert(std::is_trivially_copyable_v<host::HudTdmFeedState>);
static_assert(std::is_trivially_copyable_v<gs::TdmHudRecords>);
static_assert(std::is_trivially_copyable_v<gs::DeathCameraParams> && std::is_trivially_copyable_v<gs::DeathCameraStart> &&
		std::is_trivially_copyable_v<gs::DeathCameraView> && std::is_trivially_copyable_v<gs::DeathCameraStats>);
static_assert(std::is_trivially_copyable_v<anim::CrowdSoldierInput>);
static_assert(std::is_trivially_copyable_v<gs::TdmFrame>);
static_assert(!std::is_copy_constructible_v<gs::Bf3TdmHud> && !std::is_copy_constructible_v<gs::M2bCheck>);
static_assert(std::is_same_v<decltype(&gs::Bf3GameCrowd::set_corpse_seconds), void (gs::Bf3GameCrowd::*)(float)>);
static_assert(std::is_same_v<decltype(&host::fill_hud_tdm),
		void (*)(const host::HudFeedInput &, std::span<const host::PresentEvent>, const host::HudStatic &, host::HudTdmFeedState &, ui::HudModel &)>);
static_assert(std::is_same_v<decltype(&gs::Bf3DeathCamera::step),
		gs::DeathCameraView (gs::Bf3DeathCamera::*)(double, const fx::Vec3 *, const sim::CollisionWorld *)>);

static_assert(host::kKillCardSeconds == 3.0 && host::kKillCardDelaySeconds == 0.9);
static_assert(host::kHudSpawnSlots == std::array<uint8_t, 5>{ 0, 1, 2, 6, 7 });
static_assert(host::kHudSpawnSlots[2] == host::kKitGadgetSlot && host::kHudSpawnSlots[0] == host::kKitSlots[0] &&
		host::kHudSpawnSlots[1] == host::kKitSlots[1] && host::kHudSpawnSlots[3] == host::kKitSlots[2] && host::kHudSpawnSlots[4] == host::kKitSlots[3]);
static_assert(anim::kCrowdDeathHideSeconds == 3.0f);
static_assert(anim::kCrowdCorpseSeconds == 15.0f && anim::kCrowdReviveBlendSeconds == 0.5f);
static_assert(gs::kTdmDefaultBots == 31 && gs::kSpawnRequestRetryTicks == 30);
static_assert(uint8_t(gs::M2bCase::Play) == 0 && gs::kM2bCaseCount == 1);
static_assert(gs::m2b_case_name(gs::M2bCase::Play) == "play" && gs::m2b_case_name(gs::M2bCase(7)) == "?");
static_assert(gs::m2b_case_from_name("play") == gs::M2bCase::Play && !gs::m2b_case_from_name("Play").has_value());
static_assert(uint8_t(gs::M2bTrack::Gx) == 0 && uint8_t(gs::M2bTrack::Ui) == 1 && uint8_t(gs::M2bTrack::Cam) == 2 &&
		gs::kM2bTrackNames[1] == "ui" && gs::kM2bTrackNames[2] == "cam");
static_assert(uint8_t(host::PresentKind::Death) == 6 && uint8_t(host::PresentKind::Spawn) == 8 && uint8_t(host::PresentKind::Revive) == 9);
static_assert(host::present_flags::kOwnVictim == 8 && host::present_flags::kOwnShooter == 16 && host::present_flags::kRevived == 32);

}

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
		CHECK(!in.tdm);
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
		CHECK(dp.seconds == float(host::kDeathScreenSeconds));
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

		const gs::TdmOptions o{};
		CHECK(o.deploy_screen);
		CHECK(o.kill_limit == 0);
		CHECK(o.preround_s < 0.0f);
		CHECK(o.bots == 31);
		const gs::TdmState g{};
		CHECK(!g.enabled);
		CHECK(!g.checks());
		CHECK(g.tdm_hud == nullptr);
		CHECK(g.death_camera == nullptr);
		CHECK(g.check == nullptr);
		CHECK(g.frame.game == nullptr);
		CHECK(g.frame.match == nullptr);
		const gs::M2bCheckOptions co{};
		CHECK(co.play_seconds == 90.0);
	}

	TEST_CASE("m2b.contract2: the M2b check's case parser refuses hostile command lines") {
		const auto ok = gs::parse_m2b_cases("play");
		REQUIRE(ok.ok());
		REQUIRE(ok->size() == 1);
		CHECK((*ok)[0] == gs::M2bCase::Play);
		const std::string long_arg(4096, 'p');
		for (const char *bad : { "", "Play", "play,play", "play,", ",play", "all", "play,fx", long_arg.c_str() }) {
			CHECK_MESSAGE(!gs::parse_m2b_cases(bad).ok(), bad);
		}
	}

	TEST_CASE("m2b.contract2: the TDM screens report through UiScreenRecords, and a record set composes sources") {
		gs::TdmHudRecords r{};
		r.model_serial = 9;
		r.draws = 5;
		r.scoreboard_visible = true;
		r.scoreboard_rows = 24;
		r.scoreboard_team_kills = { 23, 19 };
		r.spawn_screen_visible = true;
		r.deploy_enabled = true;
		r.selected_kit = 2;
		r.spawn_screen_shown = 3;
		r.kill_card_visible = true;
		r.kill_card_item = 0x0fd83dfau;
		r.kill_card_weapon_named = true;
		const std::string killer = "Killer";
		std::copy(killer.begin(), killer.end(), r.kill_card_killer.begin());
		r.round_end_visible = true;
		r.round_end_shown = 1;
		r.missing_fonts = 1;
		r.missing_icons = 2;
		r.missing_strings = 3;

		for (std::string_view p : { ui::kScoreboardHud32ScreenPartition, ui::kSpawnScreenPcPartition, ui::kKillScreenPartition, ui::kPreRoundWaitingScreenPartition,
					 ui::kEorWinningTeamScreenPartition }) {
			CHECK_MESSAGE(gs::tdm_hud_reports(p), p);
			CHECK_MESSAGE(gs::tdm_hud_screen_record(r, p).has_value(), p);
		}
		CHECK(!gs::tdm_hud_reports(ui::kHudMpScreenPartition));
		CHECK(!gs::tdm_hud_screen_record(r, ui::kHudMpScreenPartition));
		const gs::UiScreenRecord board = *gs::tdm_hud_screen_record(r, ui::kScoreboardHud32ScreenPartition);
		CHECK(board.visible);
		CHECK(board.number(gs::kUiFactRows) == 24.0);
		CHECK(board.number(gs::kUiFactTeam1Kills) == 23.0);
		CHECK(board.number(gs::kUiFactTeam2Kills) == 19.0);
		const gs::UiScreenRecord spawn = *gs::tdm_hud_screen_record(r, ui::kSpawnScreenPcPartition);
		CHECK(spawn.visible);
		CHECK(spawn.shown == 3);
		CHECK(spawn.number(gs::kUiFactDeployEnabled) == 1.0);
		CHECK(spawn.number(gs::kUiFactSelectedKit) == 2.0);
		const gs::UiScreenRecord card = *gs::tdm_hud_screen_record(r, ui::kKillScreenPartition);
		CHECK(card.visible);
		CHECK(card.text(gs::kUiFactKiller) == killer);
		CHECK(card.number(gs::kUiFactItem) == double(0x0fd83dfau));
		CHECK(card.number(gs::kUiFactWeaponNamed) == 1.0);
		CHECK(card.text("unknown").empty());
		CHECK(card.number("unknown") == 0.0);
		CHECK(!gs::tdm_hud_screen_record(r, ui::kPreRoundWaitingScreenPartition)->visible);
		const gs::UiScreenRecord eor = *gs::tdm_hud_screen_record(r, ui::kEorWinningTeamScreenPartition);
		CHECK(eor.visible);
		CHECK(eor.shown == 1);
		const gs::UiLookupRecord look = gs::tdm_hud_lookup_record(r);
		CHECK(look.model_serial == 9);
		CHECK(look.draws == 5);
		CHECK(look.missing_fonts + look.missing_icons + look.missing_strings == 6);

		class Source final : public gs::UiScreenRecords {
		public:
			Source(std::string_view p_partition, uint64_t p_shown, std::optional<gs::UiLookupRecord> p_lookup) :
					partition_(p_partition), shown_(p_shown), lookup_(p_lookup) {}
			bool reports(std::string_view p_partition) const override { return p_partition == partition_; }
			std::optional<gs::UiScreenRecord> screen_record(std::string_view p_partition) const override {
				if (!reports(p_partition)) {
					return std::nullopt;
				}
				gs::UiScreenRecord out;
				out.visible = true;
				out.shown = shown_;
				return out;
			}
			std::optional<gs::UiLookupRecord> lookup_record() const override { return lookup_; }

		private:
			std::string_view partition_;
			uint64_t shown_ = 0;
			std::optional<gs::UiLookupRecord> lookup_;
		};
		const Source first(ui::kKillScreenPartition, 1, gs::UiLookupRecord{ 4, 10, 1, 0, 0 });
		const Source second(ui::kKillScreenPartition, 2, std::nullopt);
		const Source third(ui::kSpawnScreenPcPartition, 7, gs::UiLookupRecord{ 6, 5, 0, 2, 3 });
		gs::UiScreenRecordSet set;
		CHECK(!set.lookup_record());
		CHECK(!set.screen_record(ui::kKillScreenPartition));
		set.add(&first);
		set.add(&second);
		set.add(&third);
		set.add(&first);
		set.add(nullptr);
		set.add(&set);
		CHECK(set.sources() == 3);
		CHECK(set.reports(ui::kSpawnScreenPcPartition));
		CHECK(!set.reports(ui::kHudMpScreenPartition));
		CHECK(set.screen_record(ui::kKillScreenPartition)->shown == 1);
		CHECK(set.screen_record(ui::kSpawnScreenPcPartition)->shown == 7);
		const gs::UiLookupRecord sum = *set.lookup_record();
		CHECK(sum.model_serial == 6);
		CHECK(sum.draws == 15);
		CHECK(sum.missing_fonts == 1);
		CHECK(sum.missing_icons == 2);
		CHECK(sum.missing_strings == 3);
		set.remove(&first);
		CHECK(set.sources() == 2);
		CHECK(set.screen_record(ui::kKillScreenPartition)->shown == 2);
		CHECK(set.lookup_record()->draws == 5);
		set.remove(&third);
		CHECK(!set.lookup_record());
		CHECK(!set.reports(ui::kSpawnScreenPcPartition));
		{
			const Source brief(ui::kHudMpScreenPartition, 3, std::nullopt);
			set.add(&brief);
			CHECK(set.screen_record(ui::kHudMpScreenPartition)->shown == 3);
		}
		CHECK(set.sources() == 1);
		CHECK(!set.reports(ui::kHudMpScreenPartition));
		auto outer = std::make_unique<gs::UiScreenRecordSet>();
		outer->add(&third);
		outer->add(&set);
		set.add(&third);
		CHECK(outer->sources() == 2);
		outer.reset();
		set.remove(&third);
		CHECK(set.sources() == 1);
		CHECK(set.screen_record(ui::kKillScreenPartition)->shown == 2);
	}

	TEST_CASE("m2b.contract2: ui's fill_hud_tdm (pending until ui lands)") {
		if constexpr (!kUiLanded) {
			pending("ui", "fill_hud_tdm: the kill feed, kill card and spawn screen from a recorded death",
					"src/bf3_host/src/hud_feed_tdm.cpp is ui's");
		} else {

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
			in.tdm = true;
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
			CHECK(!m.kill_card.visible);
			CHECK(!m.spawn_screen.visible);
			in.now_s = 10.0 + host::kKillCardDelaySeconds + 0.1;
			host::fill_hud_tdm(in, {}, st, state, m);
			CHECK(m.kill_card.visible);
			CHECK(m.kill_card.serial == 1);
			CHECK(m.kill_card.killer == 5);
			CHECK(m.kill_card.killer_kit == 2);
			CHECK(m.kill_card.headshot);
			CHECK(m.kill_card.killer_health == doctest::Approx(64.0f));
			CHECK(m.kill_card.distance_m == doctest::Approx(12.5f));
			CHECK(!m.spawn_screen.visible);
			CHECK(m.players[5].team == 2);
			CHECK(m.local_player == 3);
			CHECK(m.kill_counter.team2_kills == 1);
			CHECK(m.kill_counter.team1_max_kills == 100);

			in.now_s = 10.0 + host::kDeathScreenSeconds + 0.1;
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

			in.match.phase = net::MatchPhase::RoundOver;
			in.match.winner = 2;
			host::fill_hud_tdm(in, {}, st, state, m);
			CHECK(m.end_of_round.visible);
			CHECK(m.end_of_round.winner == 2);
			CHECK(!m.end_of_round.local_won);
			CHECK(!m.spawn_screen.visible);

			in.tdm = false;
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
			CHECK((f.x * to.x + f.y * to.y + f.z * to.z) / len > 0.9f);
			const gs::DeathCameraView done = cam.step(100.0 + 4.1, &killer, nullptr);
			CHECK(done.finished);
			cam.end();
			CHECK(!cam.active());

			start.corpse.x = std::nanf("");
			cam.begin(start, 200.0);
			const gs::DeathCameraView bad = cam.step(200.1, nullptr, nullptr);
			CHECK(!bad.active);
			CHECK(cam.stats().bad_inputs >= 1);
		}
	}
}
