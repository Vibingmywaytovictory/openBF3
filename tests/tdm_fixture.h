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
