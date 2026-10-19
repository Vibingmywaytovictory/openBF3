// m2b.contract (docs/design/m2b.md §11, §2.6; written in W1.0 by the integrator, docs/design/m2b-w1.md): compiles every
// wave-1 interface header together with tests/tdm_fixture.h and pins
//   * the shapes the tracks share: PresentEvent 64 B, net::PlayerRow 32 B without padding, the trivially copyable
//     MatchState / PlayerRow / ReviveEvent / SpawnRequest / DownedSoldier / RulesStats / PresentationFrame;
//   * the enum values of §2.6 #5 (NativePortMeaning 16 .. 29) and #11 (MessageType 25 .. 27), PresentKind::Revive 9,
//     RoundPhase, SpawnRefusal, RulesOrder::Kind, rules_flags, restrict_bits, the W1.0 constants (kKitGadgetSlot,
//     kKeepKit, kMaxPlayerRows, kMaxPlayerNameBytes, kKitSlots unchanged);
//   * the defaults of every field W1.0 added (an M2a config, load and server stay M2a: §11.1 "Without rules");
//   * net::restrict_command, the one pure function both ends apply to the pre-round freeze (§3.7);
//   * the hashes that must not move without rules: motor_params_hash of a default MotorParams and kit_table_hash of a
//     default KitTable, recorded at W1.0 (motor_tuning.cpp and kits.cpp are main's 36eb5af there);
//   * the W1.0 stubs: the v3 codecs (empty / "v3 codec: nt") and add_tdm_bots ("W1.0 stub"), and GameServer::init
//     with the W1.0 hooks on the synthetic floor (no rules, no TDM bots: unchanged; ServerView's defaults).
// No install, no session thread, no wall-clock row.
//
// Pending rows (the M2a pattern, m2a_contract_tests.cpp): a row whose subject is a track's body runs only when its
// owner sets the switch below in the commit that lands the body; until then it prints "PENDING (<key>)" and checks
// what the contract alone defines. Calls of declared-only functions sit in `if constexpr` branches (a discarded
// statement does not odr-use them), so the test links before their bodies exist.

#include <doctest/doctest.h>

// Every wave-1 interface header, compiled together.
#include "bf3/audio/hdr_window.h"
#include "bf3/audio/mixer_core.h"
#include "bf3/audio/voice_cull.h"
#include "bf3/core/audio/hdr_setting.h"
#include "bf3/core/audio/sound_scope.h"
#include "bf3/host/client_session.h"
#include "bf3/host/game_server.h"
#include "bf3/host/gunplay.h"
#include "bf3/host/kits.h"
#include "bf3/host/level_load.h"
#include "bf3/host/presentation_events.h"
#include "bf3/host/server_view.h"
#include "bf3/host/tdm_brain.h"
#include "bf3/host/tdm_rules.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/bots/aim_model.h"
#include "bf3/sim/bots/perception.h"
#include "bf3/sim/logic/logic_native.h"
#include "bf3/sim/logic/round_natives.h"
#include "bf3/sim/soldier_motor.h"
#include "bf3/sim/spawn.h"

#include "gunplay_fixture.h"
#include "tdm_fixture.h"
#include "test_support.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <type_traits>

using namespace bf3;
namespace tg = bf3::test::gunplay;
namespace sl = bf3::sim::logic;

namespace {

// ---- the tracks' switches (each set to true by its owner, in the commit that lands the body) ------------------
constexpr bool kNtLanded = true; // nt: the v3 codecs (R9), kProtocolVersion 3
constexpr bool kRuLanded = false; // ru: [abort hash], the KitTable defib hash, the rules in GameServer::init ([init])
constexpr bool kBbLanded = false; // bb: add_tdm_bots
constexpr bool kAuLanded = false; // au: HdrWindow (E10's window numbers)

void pending(const char *p_key, const char *p_row, const char *p_reason) {
	std::printf("m2b.contract: PENDING (%s): %s -- %s\n", p_key, p_row, p_reason);
	const std::string text = std::string("PENDING (") + p_key + "): " + p_row;
	MESSAGE(text);
}

// The hashes recorded at W1.0 (see the file comment).
constexpr uint64_t kDefaultMotorParamsHash = 0x346282435cd97947ull; // motor_tuning.cpp as on main 36eb5af
constexpr uint64_t kDefaultKitTableHash = 0xa23fb7a00909ad6dull; // kits.cpp as on main 36eb5af

// ---- shapes ----------------------------------------------------------------------------------------------------

static_assert(sizeof(host::PresentEvent) == 64);
static_assert(sizeof(net::PlayerRow) == 32);
static_assert(offsetof(net::PlayerRow, ping_ms) == 6 && offsetof(net::PlayerRow, score) == 12 && offsetof(net::PlayerRow, name) == 16);
static_assert(std::is_trivially_copyable_v<net::PlayerRow>);
static_assert(std::is_trivially_copyable_v<net::MatchState>);
static_assert(std::is_trivially_copyable_v<net::ReviveEvent>);
static_assert(std::is_trivially_copyable_v<net::SpawnRequest>);
static_assert(std::is_trivially_copyable_v<host::DownedSoldier>);
static_assert(std::is_trivially_copyable_v<host::RulesStats>);
static_assert(std::is_trivially_copyable_v<host::RulesOrder>);
static_assert(std::is_trivially_copyable_v<host::PresentationFrame>);
static_assert(std::is_trivially_copyable_v<sim::MotorParams>);
static_assert(std::is_trivially_copyable_v<sim::FriendlyDeath>);
static_assert(std::is_abstract_v<host::ServerView>);
static_assert(std::is_base_of_v<host::RespawnPolicy, host::TdmRules> && std::is_base_of_v<host::CombatEventSink, host::TdmRules>);
static_assert(std::is_base_of_v<host::CommandSource, host::TdmBrain>);
static_assert(std::tuple_size_v<decltype(host::PresentationFrame::players)> == net::kMaxPlayerRows);

// ---- enum values and constants (§2.6 #5, #11, #12) --------------------------------------------------------------

using M = sl::NativePortMeaning;
static_assert(uint8_t(M::Activate) == 15); // the last S4 value: the M2b ones follow it
static_assert(uint8_t(M::Kills) == 16 && uint8_t(M::KillLimit) == 17 && uint8_t(M::TeamWins) == 18 && uint8_t(M::StopTime) == 19);
static_assert(uint8_t(M::UnSpawnAllSoldiers) == 20 && uint8_t(M::OnPlayerSpawned) == 21 && uint8_t(M::OnPlayerDied) == 22);
static_assert(uint8_t(M::OnSpectatorEnter) == 23 && uint8_t(M::PlayerEnter) == 24 && uint8_t(M::DisableAllowedToSpawnOn) == 25);
static_assert(uint8_t(M::Deactivate) == 26 && uint8_t(M::RestartLevel) == 27 && uint8_t(M::StartRound) == 28);
static_assert(uint8_t(M::EnablePlayerMovement) == 29);

static_assert(uint8_t(net::MessageType::MatchState) == 25 && uint8_t(net::MessageType::PlayerList) == 26 &&
		uint8_t(net::MessageType::SpawnRequest) == 27);
static_assert(uint8_t(net::MessageType::KitSelect) == 24 && uint8_t(net::MessageType::Debug) == 30); // the neighbours stay
static_assert(uint8_t(host::PresentKind::Spawn) == 8 && uint8_t(host::PresentKind::Revive) == 9);

static_assert(uint8_t(sl::RoundPhase::PreRound) == 0 && uint8_t(sl::RoundPhase::InRound) == 1 && uint8_t(sl::RoundPhase::RoundOver) == 2 &&
		uint8_t(sl::RoundPhase::Restarting) == 3);
static_assert(uint8_t(host::SpawnRefusal::None) == 0 && uint8_t(host::SpawnRefusal::Alive) == 1 && uint8_t(host::SpawnRefusal::TooEarly) == 2 &&
		uint8_t(host::SpawnRefusal::BadKit) == 3 && uint8_t(host::SpawnRefusal::Phase) == 4 && uint8_t(host::SpawnRefusal::RateLimited) == 5 &&
		uint8_t(host::SpawnRefusal::Spectator) == 6 && host::kSpawnRefusalCount == 7);
static_assert(uint8_t(host::RulesOrder::Kind::Spawn) == 0 && uint8_t(host::RulesOrder::Kind::Revive) == 1 &&
		uint8_t(host::RulesOrder::Kind::Unspawn) == 2);
static_assert(uint8_t(host::MatchRulesConfig::Mode::None) == 0 && uint8_t(host::MatchRulesConfig::Mode::Tdm) == 1);
static_assert(uint8_t(net::MatchPhase::Loading) == 0 && uint8_t(net::MatchPhase::PreRound) == 1 && uint8_t(net::MatchPhase::InRound) == 2 &&
		uint8_t(net::MatchPhase::RoundOver) == 3);

static_assert(net::rules_flags::kTdm == 1 && net::rules_flags::kDeployScreen == 2 && net::rules_flags::kRevive == 4);
static_assert(net::restrict_bits::kThrottle == 1u && net::restrict_bits::kThrowGrenade == (1u << 12) && net::restrict_bits::kAll == 0x1FFFu);
static_assert(net::kMaxPlayerRows == 64 && net::kMaxPlayerNameBytes == 16 && net::kPlayerListIntervalTicks == 15);
static_assert(host::kKitGadgetSlot == 2 && host::kKeepKit == 0xFF && host::kKitCount == 4);
static_assert(host::kKitSlots == std::array<uint8_t, 4>{ 0, 1, 6, 7 }); // §2.6 #9: not changed
static_assert(core::audio::kNoScope == 0xFFFF);

// ---- net::restrict_command (§3.7: the same function on both ends) ------------------------------------------------

// Metro TDM's InputRestriction (m2b.md §3.1): Yaw, Pitch, Jump, Prone, Zoom, Reload allowed.
constexpr uint32_t kMetroAllowed = net::restrict_bits::kYaw | net::restrict_bits::kPitch | net::restrict_bits::kJump | net::restrict_bits::kProne |
		net::restrict_bits::kZoom | net::restrict_bits::kReload;

constexpr sim::Command busy_command(sim::Tick p_tick) {
	sim::Command c;
	c.tick = p_tick;
	c.sequence = 7;
	c.buttons = sim::buttons::kJump | sim::buttons::kSprint | sim::buttons::kFire | sim::buttons::kZoom | sim::buttons::kReload |
			(3u << sim::weapons::weapon_buttons::kSlotShift);
	c.move_forward = 127;
	c.move_right = -127;
	c.yaw = 1234;
	c.pitch = -56;
	c.pose = sim::Pose::Crouch;
	c.view_back_q8 = 99;
	return c;
}
// Restricted: the axes 0, Fire and Sprint released, everything else as it was.
constexpr sim::Command kRestricted = net::restrict_command(busy_command(100), 101, kMetroAllowed);
static_assert(kRestricted.move_forward == 0 && kRestricted.move_right == 0);
static_assert(kRestricted.buttons ==
		(sim::buttons::kJump | sim::buttons::kZoom | sim::buttons::kReload | (3u << sim::weapons::weapon_buttons::kSlotShift)));
static_assert(kRestricted.yaw == 1234 && kRestricted.pitch == -56 && kRestricted.pose == sim::Pose::Crouch && kRestricted.sequence == 7 &&
		kRestricted.view_back_q8 == 99);
// Not restricted: until 0, the command at or after until, or everything allowed.
static_assert(net::restrict_command(busy_command(100), 0, kMetroAllowed) == busy_command(100));
static_assert(net::restrict_command(busy_command(101), 101, kMetroAllowed) == busy_command(101));
static_assert(net::restrict_command(busy_command(100), 101, net::restrict_bits::kAll) == busy_command(100));
// No known end.
static_assert(net::restrict_command(busy_command(5000000), UINT32_MAX, kMetroAllowed).move_forward == 0);

} // namespace

TEST_SUITE("m2b-contract") {
	TEST_CASE("m2b.contract: the defaults of every W1.0 field keep M2a (§11.1 'Without rules')") {
		const host::MatchRulesConfig rules;
		CHECK(rules.mode == host::MatchRulesConfig::Mode::None);
		CHECK_FALSE(rules.deploy_screen);
		CHECK(rules.revive);
		CHECK(rules.kill_limit_override == 0);
		CHECK(rules.preround_override_s == doctest::Approx(-1.0f));
		CHECK(rules.max_players == 32);
		const host::GameServerConfig server;
		CHECK(server.rules.mode == host::MatchRulesConfig::Mode::None);
		CHECK(server.tdm_bots == nullptr);
		CHECK(server.respawn_ticks == 60); // M2a's fallback stays
		const host::SharedLoadOptions load;
		CHECK_FALSE(load.tdm);
		const host::SharedLevel level;
		CHECK(level.logic == nullptr);
		CHECK(level.logic_digest == 0);
		CHECK(host::GunplayConfig().defib_def == sim::weapons::kNoWeapon);
		CHECK_FALSE(host::GunplayLoadOptions().tdm_kits);
		CHECK(host::KitTableOptions().assault_gadget.empty());
		CHECK(host::KitTable().defib_def == sim::weapons::kNoWeapon);
		const sim::MotorParams motor;
		CHECK(motor.protect_abort_buttons == 0);
		CHECK_FALSE(motor.protect_abort_move);
		CHECK(motor.immortal_ticks == 120);
		const audio::MixerConfig mixer;
		CHECK(mixer.max_voices == 128);
		CHECK_FALSE(mixer.hdr.has_value());
		CHECK(mixer.scopes == nullptr);
		CHECK(mixer.max_instances == 64);

		const net::DeathEvent death;
		CHECK(death.respawn_tick == 0);
		CHECK(death.revive_until == 0);
		CHECK(death.killer_health == 0.0f);
		CHECK_FALSE(net::Spawned().revived);
		CHECK(net::MatchInfo().logic_digest == 0);
		CHECK(net::MatchInfo().rules_flags == 0);
		CHECK(net::CombatEvents().revives.empty());
		const net::SpawnRequest request;
		CHECK(request.kit == 0);
		CHECK(request.spawn_choice == 0);
		const net::MatchState match;
		CHECK(match.phase == net::MatchPhase::Loading);
		CHECK(match.restrict_until_tick == 0);
		CHECK(match.restrict_mask == net::restrict_bits::kAll);
		CHECK(match.killer_camera);
		const host::PresentationFrame frame;
		CHECK_FALSE(frame.match_valid);
		CHECK(frame.player_count == 0);
		CHECK(frame.own_respawn_tick == 0);
		CHECK(frame.own_revive_until == 0);
		CHECK_FALSE(frame.own_unspawned);

		const sl::RoundNativeConfig natives;
		CHECK(natives.max_players == 32);
		CHECK(natives.kill_limit_override == 0);
		CHECK(natives.preround_override_s == doctest::Approx(-1.0f));
		const sim::SpawnQuery query; // §3.5's data and named defaults
		CHECK(query.protection_radius == doctest::Approx(10.0f));
		CHECK(query.friendly_block_radius == doctest::Approx(30.0f));
		CHECK(query.friendly_block_count == 3);
		CHECK(query.friendly_block_ticks == 1800);
		CHECK(query.max_los_rays == 16);
		CHECK(query.world == nullptr);

		const sim::bots::PerceptionParams perception; // §5.1
		CHECK(perception.fov_h_deg == doctest::Approx(160.0f));
		CHECK(perception.fov_v_deg == doctest::Approx(90.0f));
		CHECK(perception.max_range_m == doctest::Approx(120.0f));
		CHECK(perception.update_ticks == 6);
		CHECK(perception.max_candidates == 4);
		CHECK(perception.forget_s == doctest::Approx(3.0f));
		CHECK(perception.hearing_radius_m == doctest::Approx(60.0f));
		const sim::bots::AimParams aim; // §5.3
		CHECK(aim.e0_deg == doctest::Approx(3.0f));
		CHECK(aim.e_inf_deg == doctest::Approx(0.5f));
		CHECK(aim.tau_s == doctest::Approx(0.6f));
		const host::TdmBotConfig bots; // §5.3, §5.6, J5, J6
		CHECK(bots.count == 31);
		CHECK(bots.skill_min == doctest::Approx(0.3f));
		CHECK(bots.skill_max == doctest::Approx(0.8f));
		CHECK(bots.reaction_min_s == doctest::Approx(0.25f));
		CHECK(bots.reaction_max_s == doctest::Approx(0.6f));
		CHECK(bots.revive_radius_m == doctest::Approx(30.0f));

		const core::audio::HdrSetting hdr; // §6.1: the data's values
		CHECK(hdr.window_size_db == doctest::Approx(45.0f));
		CHECK(hdr.window_min_top_db == doctest::Approx(80.0f));
		CHECK(hdr.window_min_bottom_db == doctest::Approx(35.0f));
		CHECK(hdr.top_attack_s == doctest::Approx(0.05f));
		CHECK(hdr.headroom_db == doctest::Approx(5.0f));
		CHECK(hdr.bottom_release_s == doctest::Approx(0.025f));
	}

	TEST_CASE("m2b.contract: the hashes an M2a load keeps (motor params, kit table)") {
		const uint64_t motor = sim::motor_params_hash(sim::MotorParams{});
		const uint64_t kits = host::kit_table_hash(host::KitTable{});
		std::printf("m2b.contract: motor_params_hash(default) = 0x%016llx, kit_table_hash(default) = 0x%016llx\n", (unsigned long long)motor,
				(unsigned long long)kits);
		CHECK(motor == kDefaultMotorParamsHash);
		CHECK(kits == kDefaultKitTableHash);
		if constexpr (kRuLanded) {
			sim::MotorParams aborting;
			aborting.protect_abort_buttons = sim::buttons::kFire;
			CHECK(sim::motor_params_hash(aborting) != motor); // [abort hash]: set fields are hashed
			aborting.protect_abort_buttons = 0;
			aborting.protect_abort_move = true;
			CHECK(sim::motor_params_hash(aborting) != motor);
			host::KitTable with_defib;
			with_defib.defib_def = 3;
			CHECK(host::kit_table_hash(with_defib) != kits);
		} else {
			pending("ru", "[abort hash] and the KitTable defib hash", "set fields must change the hashes once ru lands them");
		}
	}

	TEST_CASE("m2b.contract: the v3 codecs (W1.0 stubs until nt)") {
		net::MatchState match;
		match.phase = net::MatchPhase::InRound;
		match.kills = { 42, 17 };
		match.kill_limit = 100;
		net::PlayerList list;
		list.full = true;
		net::PlayerRow row;
		row.slot = 3;
		row.team = 2;
		row.name_bytes = 4;
		row.name = { 'b', 'o', 't', '3' };
		list.rows.push_back(row);
		const net::SpawnRequest request{ 2, 0 };
		if constexpr (kNtLanded) {
			CHECK(net::kProtocolVersion == 3);
			const std::vector<uint8_t> m = net::encode_match_state(match);
			CHECK(net::peek_type(m) == net::MessageType::MatchState);
			core::Result<net::MatchState> md = net::decode_match_state(m);
			REQUIRE(md.ok());
			CHECK(md->kills[0] == 42);
			CHECK(md->kills[1] == 17);
			CHECK(md->kill_limit == 100);
			const std::vector<uint8_t> p = net::encode_player_list(list);
			CHECK(net::peek_type(p) == net::MessageType::PlayerList);
			core::Result<net::PlayerList> pd = net::decode_player_list(p);
			REQUIRE(pd.ok());
			REQUIRE(pd->rows.size() == 1);
			CHECK(pd->rows[0].slot == 3);
			CHECK(pd->rows[0].name_bytes == 4);
			const std::vector<uint8_t> s = net::encode_spawn_request(request);
			CHECK(net::peek_type(s) == net::MessageType::SpawnRequest);
			core::Result<net::SpawnRequest> sd = net::decode_spawn_request(s);
			REQUIRE(sd.ok());
			CHECK(sd->kit == 2);
		} else {
			CHECK(net::kProtocolVersion == 2);
			CHECK(net::encode_match_state(match).empty());
			CHECK(net::encode_player_list(list).empty());
			CHECK(net::encode_spawn_request(request).empty());
			const std::vector<uint8_t> bytes{ 25, 0, 0, 0 };
			const core::Result<net::MatchState> md = net::decode_match_state(bytes);
			const core::Result<net::PlayerList> pd = net::decode_player_list(bytes);
			const core::Result<net::SpawnRequest> sd = net::decode_spawn_request(bytes);
			REQUIRE_FALSE(md.ok());
			REQUIRE_FALSE(pd.ok());
			REQUIRE_FALSE(sd.ok());
			CHECK(md.error().message == "v3 codec: nt");
			CHECK(pd.error().message == "v3 codec: nt");
			CHECK(sd.error().message == "v3 codec: nt");
			pending("nt", "v3 round trips, kProtocolVersion 3", "the W1.0 stubs answer until nt's codecs land (R9)");
		}
	}

	TEST_CASE("m2b.contract: GameServer::init with the W1.0 hooks ([tdm bots], ServerView's defaults)") {
		const std::shared_ptr<const host::SharedLevel> level = tg::synthetic_floor();
		REQUIRE(level != nullptr);
		if (level == nullptr) {
			return;
		}
		{
			host::GameServer server; // no rules, no TDM bots: M2a's init
			host::GameServerConfig cfg;
			cfg.level = level;
			cfg.max_slots = 8;
			REQUIRE(server.init(cfg).ok());
			const host::ServerView &view = server.view();
			CHECK(view.downed().empty());
			CHECK(view.phase() == net::MatchPhase::InRound);
			CHECK_FALSE(view.input_restricted());
			CHECK(view.kit(0) == 0);
		}
		{
			host::GameServer server; // a config with no bots adds nothing
			host::GameServerConfig cfg;
			cfg.level = level;
			cfg.max_slots = 8;
			auto bots = std::make_shared<host::TdmBotConfig>();
			bots->count = 0;
			cfg.tdm_bots = bots;
			CHECK(server.init(cfg).ok());
		}
		host::GameServer server;
		host::GameServerConfig cfg;
		cfg.level = level;
		cfg.max_slots = 8;
		auto bots = std::make_shared<host::TdmBotConfig>();
		bots->count = 4;
		cfg.tdm_bots = bots;
		const core::Status st = server.init(cfg);
		if constexpr (kBbLanded) {
			// bb's add_tdm_bots: TDM bots need the rules; without them bb refuses or adds them (bb states which).
			MESSAGE("add_tdm_bots without rules: " << (st.ok() ? std::string("ok") : st.error().message));
		} else {
			REQUIRE_FALSE(st.ok());
			CHECK(st.error().message.find("W1.0 stub") != std::string::npos);
			pending("bb", "add_tdm_bots adds the brains", "the W1.0 stub refuses a config with bots");
		}
		if constexpr (kRuLanded) {
			host::GameServer tdm; // rules on a level without a logic graph: [init] refuses
			host::GameServerConfig c;
			c.level = level;
			c.max_slots = 8;
			c.rules.mode = host::MatchRulesConfig::Mode::Tdm;
			CHECK_FALSE(tdm.init(c).ok());
		} else {
			pending("ru", "[init] refuses rules without SharedLevel::logic", "init ignores GameServerConfig::rules until ru's [init]");
		}
	}

	TEST_CASE("m2b.contract: the HDR window (E10's numbers, once au lands)") {
		if constexpr (kAuLanded) {
			audio::HdrWindow window;
			window.configure(core::audio::HdrSetting{}, audio::kMixRate);
			window.reset();
			for (int i = 0; i < 100; ++i) {
				window.step(71.0f, audio::kBlockFrames); // a 71 dB footstep alone
			}
			CHECK(window.top_db() == doctest::Approx(80.0f).epsilon(0.006));
			CHECK(window.bottom_db() == doctest::Approx(35.0f).epsilon(0.015));
			for (int i = 0; i < 100; ++i) {
				window.step(110.0f, audio::kBlockFrames); // a 110 dB shot
			}
			CHECK(window.top_db() == doctest::Approx(110.0f).epsilon(0.005));
			CHECK(window.bottom_db() == doctest::Approx(65.0f).epsilon(0.008));
			CHECK(window.suspended(63.0f));
		} else {
			pending("au", "HdrWindow 71 dB -> 80 / 35, 110 dB -> 110 / 65", "declared only until au's hdr_window.cpp");
		}
	}
}
