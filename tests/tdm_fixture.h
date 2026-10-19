#pragma once
// The M2b TDM test fixture (docs/design/m2b.md §1, §2.6 #21, §11; declared in W1.0, docs/design/m2b-w1.md). ru writes
// the bodies (its host.tdm.server cases need them first); bb adds counters to TdmRunResult and its run in a block
// headed "// M2b bb" at the end of this file. m2b.contract compiles it and calls nothing in it.
//
// The headless host.tdm.* rows step the real GameServer back to back on the test thread, as M2a's run_firefight does
// (bf3/host/firefight_run.h: no ServerHost, no sockets, no wall-clock wait), with a datagram-level scripted peer:
//   load_tdm_level_once  the shared Metro TDM level, once per process: load_shared_level with SharedLoadOptions::gunplay
//                        and ::tdm (the logic graph, the TDM kits, the immunity abort) and S1's BotLevel (nav through the
//                        cache). Read-only on the install (InstallReader / Install, the install.unchanged check below);
//                        skips the case (exit 77) without an install. Needs OPENBF3_MOTOR_TUNING_CFG like
//                        gunplay_fixture.h.
//   ScriptedPeer         one peer on GameServer::on_connect / on_receive: Hello, Ready, SpawnRequest, KitSelect, Debug
//                        and CommandPackets stamped for the tick about to run (delivered before the tick, so the jitter
//                        buffer and the simulation never depend on the wall clock, m2b.md §9.2); it decodes MatchInfo,
//                        MatchState, PlayerList, Spawned, CombatEvents and its snapshots from the tick's OutDatagrams.
//   run_tdm              GameServer::init with GameServerConfig::rules (Tdm), tdm_bots and bots (count 0: the BotLevel
//                        for walking), the optional peer, then tick() back to back on the caller's thread until
//                        RoundOver + after_round_ticks or max_ticks (a hard stop: the row fails as a bot behaviour bug).
// Until a part lands (the v3 codecs: nt; the rules: ru; the brains: bb), a case that needs it uses a test-local fake in
// namespace m2b_fake, never a real symbol (m2b.md §11).
//
// Everything here is declared only in W1.0. ru defines it, inline in this header (several executables may include it)
// or in a tests/tdm_fixture.cpp listed with test_host_tdm.

#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/install_snapshot.h"
#include "bf3/core/result.h"
#include "bf3/host/bot_fill.h"
#include "bf3/host/game_server.h"
#include "bf3/host/level_load.h"
#include "bf3/host/tdm_brain.h"
#include "bf3/host/tdm_rules.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/command.h"
#include "bf3/sim/sim_types.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace bf3::test::tdm {

// The shared level of one process (load_tdm_level_once).
struct TdmLevel {
	std::unique_ptr<core::InstallReader> reader;
	std::shared_ptr<core::Install> install;
	core::InstallSnapshot before; // the install.unchanged check
	std::shared_ptr<const host::SharedLevel> shared; // gunplay + logic + TDM kits + immunity abort
	std::shared_ptr<const host::BotLevel> bots; // S1's nav, main-component spawn list
	std::filesystem::path cache_root;
	double seconds_load = 0;
};
// Null (and the case marked skipped, exit 77) without an install. Loaded on the first call, then shared.
TdmLevel *load_tdm_level_once();
// The install-unchanged check (call it in the last install case of an executable).
void check_install_unchanged(TdmLevel &r_level);

// A datagram-level peer (no ENet): what a ClientSession would send and receive, scripted.
class ScriptedPeer {
public:
	ScriptedPeer(host::GameServer &r_server, uint32_t p_peer, std::string p_name = "peer");
	// Requests (each goes through GameServer::on_receive at once, channel 1).
	void hello(); // Hello {kProtocolVersion, name}: the server answers MatchInfo
	void ready();
	void spawn_request(uint8_t p_kit, uint8_t p_spawn_choice = 0);
	void kit_select(const net::KitSelect &p_select);
	void debug(const net::DebugRequest &p_request);
	// One command for the tick about to run (GameServer::current_tick() + 1), channel 0.
	void command(sim::Command p_command);
	// Decodes the OutDatagrams of one tick addressed to this peer (others are ignored).
	void receive(std::span<const host::OutDatagram> p_out);

	uint32_t peer() const { return peer_; }
	int slot() const; // from MatchInfo; -1 before it

	// What arrived (newest last; bounded by the run's length).
	std::optional<net::MatchInfo> match_info;
	std::optional<net::MatchState> match_state; // the latest
	std::vector<net::MatchState> match_states; // every one, in order (E6 hashes their bytes)
	std::vector<net::PlayerRow> players; // the applied PlayerList (full replaces, changed rows update), ascending slot
	std::vector<net::Spawned> spawned;
	std::vector<net::CombatEvents> combat;
	std::optional<net::Snapshot> last_snapshot;
	std::optional<net::Kick> kick;
	uint64_t decode_errors = 0; // datagrams addressed to this peer that failed to decode

private:
	host::GameServer *server_ = nullptr;
	uint32_t peer_ = 0;
	std::string name_;
	uint32_t sequence_ = 0;
};

struct TdmRunConfig {
	uint64_t seed = 1; // GameServerConfig::seed (the match seed derives from it)
	uint32_t bots = 31; // TdmBotConfig::count
	bool peer = true; // one ScriptedPeer (Hello, Ready, SpawnRequest when deploy_screen)
	uint8_t peer_kit = 0;
	host::MatchRulesConfig rules{ host::MatchRulesConfig::Mode::Tdm, false, true, 0, 0.0f, 32 }; // --preround=0
	uint32_t max_ticks = 21600; // the hard stop (6 simulated minutes, E1)
	uint32_t after_round_ticks = 600; // ticks run after RoundOver (E6: + 600; E1: the 60 s round end + restart)
	bool hashes = true; // per-tick world hash (firefight_run.h's: soldier_state_hash, soldier_weapons_hash, projectiles,
	                    // the tick's damage and detonations)
	bool keep_trace_records = false;
	uint32_t stuck_window_ticks = 1200; // 20 s (M2a's stuck metric)
	float stuck_metres = 1.0f;
	std::string mechanics_cfg; // GunplayConfig::mechanics_cfg (empty: the shipped file in bf3-opensource/config)
	std::function<void(host::GameServerConfig &)> configure_server; // after the defaults
	std::function<void(host::TdmBotConfig &)> configure_bots;
	// Called after every tick with the server and the peer (null without one): scripted damage, deaths, requests.
	std::function<void(sim::Tick, host::GameServer &, ScriptedPeer *)> on_tick;
};

struct TdmRunResult {
	uint32_t ticks = 0; // ticks run
	bool round_over = false;
	sim::Tick round_over_tick = 0;
	uint8_t winner = 0;
	std::array<int32_t, 3> kills{}; // KillCounter.Kills of team 1 / 2 ([0] unused)
	std::array<uint32_t, 3> deaths{}; // by the victim's team
	double team_kd = 0; // kills[1] / kills[2]
	uint32_t revives = 0;
	uint32_t stuck_bots = 0;
	uint32_t spawns_scored = 0, spawns_fallback = 0;
	uint32_t spawns_enemy_within_10m = 0; // scored spawns with an enemy within 10 m (E2: 0)
	uint32_t sight_violations = 0; // E2: detections without a clear eye-to-head / chest ray at that tick
	uint32_t stale_aim_shots = 0; // E2: aimed shots at a target whose last clear ray is > 30 ticks old
	uint32_t blind_fire = 0; // counted apart (E2)
	bool runtime_faulted = false;
	std::vector<uint64_t> world_hashes; // per tick (E6)
	uint64_t trace_hash = 0; // TdmRules::trace().hash (E6)
	uint64_t match_bytes_hash = 0; // the peer's decoded MatchState / PlayerList bytes (E6)
	host::RulesStats rules;
	std::vector<double> tick_ms; // GameServerTickTimes::tick_ms per tick (reported, never waited on)
	std::vector<double> perception_ms, brains_ms; // per tick, from TdmBotStats (E8)
	double seconds_init = 0, seconds_run = 0;
	std::string failure; // set when init or a tick step failed
};

// init, then tick() back to back on the caller's thread. Fails for a level without logic or gunplay.
core::Result<TdmRunResult> run_tdm(const TdmLevel &p_level, const TdmRunConfig &p_config);

} // namespace bf3::test::tdm

// M2b bb (docs/design/m2b.md §1 E2, E8, §5; docs/design/m2b-bb.md): additive counters for the bots' rows. A BotChecks
// is the TDM brains' observer (tdm_brain.h TdmBotObserver) plus an after-tick hook, attached to a TdmRunConfig by
// attach_bot_checks (it chains any configure_bots / on_tick already set) and read back with apply_bot_checks after
// run_tdm. Everything it counts it checks itself, independently of the brains:
//   sight     every detection re-casts the eye -> head and eye -> chest rays (query_mask::kProjectile) on the
//             server's world at that tick from the target's state; neither clear = a sight violation (E2: 0);
//   aim       every aimed fire tick whose target's last clear ray is more than kStaleAimTicks old = stale (E2: 0);
//             blind fire at a last-known position is counted apart;
//   stuck     per bot, windows of TdmRunConfig::stuck_window_ticks on one life; a window whose farthest horizontal
//             distance from its start stays under stuck_metres is stuck (M2a's firefight metric). The bot's
//             Restricted (freeze, round end), RegenWait and Revive ticks restart its window (E2 leaves them out);
//   timing    TdmBotStats::last_perception_ns / last_brains_ns after every tick (E8).
// Server thread only (the observer runs inside GameServer::tick, the after-tick hook right after it).
#include "bf3/sim/collision_world.h"
#include "bf3/sim/static_world.h"

#include <cmath>
#include <set>

namespace bf3::test::tdm {

inline constexpr uint32_t kStaleAimTicks = 30; // E2

struct BotChecks final : host::TdmBotObserver {
	float eye_heights[3] = { 1.6f, 1.0f, 0.3f };
	sim::bots::PerceptionParams perception;
	std::shared_ptr<host::TdmBotStats> stats;
	uint32_t window_ticks = 1200;
	float window_metres = 1.0f;

	uint64_t detections = 0, sight_violations = 0;
	uint64_t aimed_fire_ticks = 0, stale_aim_shots = 0, blind_fire_ticks = 0;
	uint64_t windows = 0, stuck_windows = 0;
	std::set<uint8_t> stuck_slots;
	std::array<uint64_t, host::kTdmBotActivityCount> activity_ticks{};
	std::vector<double> perception_ms, brains_ms;

	struct Window {
		bool open = false;
		uint32_t life = 0;
		sim::Tick start = 0;
		sim::Vec3 anchor{};
		float farthest = 0.0f;
	};
	std::array<Window, 256> win{};
	std::array<host::TdmBotActivity, 256> last{};
	std::array<uint8_t, 256> is_bot{};

	void detected(const host::ServerView &p_view, sim::Tick, uint8_t, uint8_t p_target, const sim::Vec3 &p_eye) override {
		++detections;
		const std::span<const sim::SoldierState> states = p_view.states();
		if (p_target >= states.size()) {
			++sight_violations;
			return;
		}
		const sim::SoldierState &t = states[p_target];
		const sim::Vec3 head = sim::bots::perception_head(perception, t, eye_heights);
		const sim::Vec3 chest = sim::bots::perception_chest(perception, t, eye_heights);
		const auto clear = [&](const sim::Vec3 &p_to) {
			return !p_view.world().ray(p_eye, { p_to[0] - p_eye[0], p_to[1] - p_eye[1], p_to[2] - p_eye[2] }, sim::query_mask::kProjectile);
		};
		if (!clear(head) && !clear(chest)) {
			++sight_violations;
		}
	}
	void fired(const host::ServerView &, sim::Tick p_tick, uint8_t, int, sim::Tick p_last_visible, bool p_blind) override {
		if (p_blind) {
			++blind_fire_ticks;
			return;
		}
		++aimed_fire_ticks;
		if (p_tick > p_last_visible + kStaleAimTicks) {
			++stale_aim_shots;
		}
	}
	void activity(sim::Tick, uint8_t p_bot, host::TdmBotActivity p_activity) override {
		last[p_bot] = p_activity;
		is_bot[p_bot] = 1;
		++activity_ticks[size_t(p_activity)];
	}
	// After every tick: the stuck windows and the timing.
	void after_tick(sim::Tick p_tick, host::GameServer &r_server) {
		const std::span<const sim::SoldierState> states = r_server.view().states();
		for (size_t s = 0; s < states.size() && s < win.size(); ++s) {
			if (is_bot[s] == 0) {
				continue;
			}
			Window &w = win[s];
			const host::TdmBotActivity a = last[s];
			const bool exempt = a == host::TdmBotActivity::Dead || a == host::TdmBotActivity::Restricted || a == host::TdmBotActivity::RegenWait ||
					a == host::TdmBotActivity::Revive || !r_server.view().alive(uint8_t(s));
			if (exempt || !w.open || w.life != states[s].life) {
				w.open = !exempt;
				w.life = states[s].life;
				w.start = p_tick;
				w.anchor = states[s].position;
				w.farthest = 0.0f;
				continue;
			}
			const float dx = states[s].position[0] - w.anchor[0], dz = states[s].position[2] - w.anchor[2];
			w.farthest = std::max(w.farthest, std::sqrt(dx * dx + dz * dz));
			if (p_tick - w.start >= window_ticks) {
				++windows;
				if (w.farthest < window_metres) {
					++stuck_windows;
					stuck_slots.insert(uint8_t(s));
				}
				w.start = p_tick;
				w.anchor = states[s].position;
				w.farthest = 0.0f;
			}
		}
		if (stats != nullptr) {
			perception_ms.push_back(double(stats->last_perception_ns.load(std::memory_order_relaxed)) * 1e-6);
			brains_ms.push_back(double(stats->last_brains_ns.load(std::memory_order_relaxed)) * 1e-6);
		}
	}
};

// Chains the checks into p_config (configure_bots sets the observer and the stats; on_tick runs after_tick).
inline std::shared_ptr<BotChecks> attach_bot_checks(TdmRunConfig &r_config, const TdmLevel *p_level) {
	auto checks = std::make_shared<BotChecks>();
	if (p_level != nullptr && p_level->shared != nullptr) {
		for (size_t i = 0; i < 3; ++i) {
			if (p_level->shared->motor.poses[i].eye_height > 0.0f) {
				checks->eye_heights[i] = p_level->shared->motor.poses[i].eye_height;
			}
		}
	}
	checks->window_ticks = r_config.stuck_window_ticks;
	checks->window_metres = r_config.stuck_metres;
	checks->stats = std::make_shared<host::TdmBotStats>();
	std::function<void(host::TdmBotConfig &)> bots_before = r_config.configure_bots;
	r_config.configure_bots = [checks, bots_before](host::TdmBotConfig &r_bots) {
		if (bots_before) {
			bots_before(r_bots);
		}
		checks->perception = r_bots.perception;
		r_bots.observer = checks;
		r_bots.stats = checks->stats;
	};
	std::function<void(sim::Tick, host::GameServer &, ScriptedPeer *)> tick_before = r_config.on_tick;
	r_config.on_tick = [checks, tick_before](sim::Tick p_tick, host::GameServer &r_server, ScriptedPeer *p_peer) {
		checks->after_tick(p_tick, r_server);
		if (tick_before) {
			tick_before(p_tick, r_server, p_peer);
		}
	};
	return checks;
}

// The checks' counts into the result's E2 / E8 fields (the brains' own view of stuck bots replaces run_tdm's, which
// cannot tell a regen wait from a stuck bot; run_tdm's value is kept in r_result_stuck_m2a when given).
inline void apply_bot_checks(const BotChecks &p_checks, TdmRunResult &r_result, uint32_t *r_result_stuck_m2a = nullptr) {
	if (r_result_stuck_m2a != nullptr) {
		*r_result_stuck_m2a = r_result.stuck_bots;
	}
	r_result.sight_violations = uint32_t(std::min<uint64_t>(p_checks.sight_violations, UINT32_MAX));
	r_result.stale_aim_shots = uint32_t(std::min<uint64_t>(p_checks.stale_aim_shots, UINT32_MAX));
	r_result.blind_fire = uint32_t(std::min<uint64_t>(p_checks.blind_fire_ticks, UINT32_MAX));
	r_result.stuck_bots = uint32_t(p_checks.stuck_slots.size());
	if (!p_checks.perception_ms.empty()) {
		r_result.perception_ms = p_checks.perception_ms;
		r_result.brains_ms = p_checks.brains_ms;
	}
}

} // namespace bf3::test::tdm
