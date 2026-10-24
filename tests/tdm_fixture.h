#pragma once

#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/install_snapshot.h"
#include "bf3/core/result.h"
#include "bf3/core/util/elapsed.h"
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

struct TdmLevel {
	std::unique_ptr<core::InstallReader> reader;
	std::shared_ptr<core::Install> install;
	core::InstallSnapshot before;
	std::shared_ptr<const host::SharedLevel> shared;
	std::shared_ptr<const host::BotLevel> bots;
	std::filesystem::path cache_root;
	double seconds_load = 0;
};

TdmLevel *load_tdm_level_once();

void check_install_unchanged(TdmLevel &r_level);

class ScriptedPeer {
public:
	ScriptedPeer(host::GameServer &r_server, uint32_t p_peer, std::string p_name = "peer");

	void hello();
	void ready();
	void spawn_request(uint8_t p_kit, uint8_t p_spawn_choice = 0);
	void kit_select(const net::KitSelect &p_select);
	void debug(const net::DebugRequest &p_request);

	void command(sim::Command p_command);

	void receive(std::span<const host::OutDatagram> p_out);

	uint32_t peer() const { return peer_; }
	int slot() const;

	std::optional<net::MatchInfo> match_info;
	std::optional<net::MatchState> match_state;
	std::vector<net::MatchState> match_states;
	std::vector<net::PlayerRow> players;
	std::vector<net::Spawned> spawned;
	std::vector<net::CombatEvents> combat;
	std::optional<net::Snapshot> last_snapshot;
	std::optional<net::Kick> kick;
	uint64_t decode_errors = 0;

private:
	host::GameServer *server_ = nullptr;
	uint32_t peer_ = 0;
	std::string name_;
	uint32_t sequence_ = 0;
};

struct TdmRunConfig {
	uint64_t seed = 1;
	uint32_t bots = 31;
	bool peer = true;
	uint8_t peer_kit = 0;
	host::MatchRulesConfig rules{ host::MatchRulesConfig::Mode::Tdm, false, true, 0, 0.0f, 32 };
	uint32_t max_ticks = 21600;
	uint32_t after_round_ticks = 600;
	bool hashes = true;
	bool record_decisions = false;

	bool keep_trace_records = false;
	uint32_t stuck_window_ticks = 1200;
	float stuck_metres = 1.0f;
	std::string mechanics_cfg;
	std::function<void(host::GameServerConfig &)> configure_server;
	std::function<void(host::TdmBotConfig &)> configure_bots;

	std::function<void(sim::Tick, host::GameServer &, ScriptedPeer *)> on_tick;
};

struct TdmRunResult {
	uint32_t ticks = 0;
	bool round_over = false;
	sim::Tick round_over_tick = 0;
	uint8_t winner = 0;
	std::array<int32_t, 3> kills{};
	std::array<uint32_t, 3> deaths{};
	double team_kd = 0;
	uint32_t revives = 0;
	uint32_t stuck_bots = 0;
	uint32_t spawns_scored = 0, spawns_fallback = 0;
	uint32_t spawns_enemy_within_10m = 0;
	uint32_t sight_violations = 0;
	uint32_t stale_aim_shots = 0;
	uint32_t blind_fire = 0;
	bool runtime_faulted = false;
	std::vector<uint64_t> world_hashes;
	uint64_t trace_hash = 0;
	uint64_t match_bytes_hash = 0;
	std::vector<uint64_t> decision_hashes;
	uint64_t decision_hash = 0;
	uint64_t decisions = 0;
	host::RulesStats rules;
	std::vector<double> tick_ms;
	std::vector<double> perception_ms, brains_ms;
	double seconds_init = 0, seconds_run = 0;
	std::string failure;
};

core::Result<TdmRunResult> run_tdm(const TdmLevel &p_level, const TdmRunConfig &p_config);

}

#include <doctest/doctest.h>

#include "bf3/core/cache/disk_cache.h"
#include "bf3/host/gunplay.h"
#include "bf3/sim/weapons/weapon_sim.h"
#include "gunplay_fixture.h"
#include "test_support.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

namespace bf3::test::tdm {

namespace detail {

using bf3::core::seconds_since;

inline net::ShotQuantization &frame() {
	static net::ShotQuantization s_frame;
	return s_frame;
}

struct Fnv {
	uint64_t h = 0xcbf29ce484222325ull;
	void bytes(const void *p_data, size_t p_size) {
		const uint8_t *b = static_cast<const uint8_t *>(p_data);
		for (size_t i = 0; i < p_size; ++i) {
			h ^= b[i];
			h *= 0x100000001b3ull;
		}
	}
	template <typename T>
	void add(const T &p_value) {
		bytes(&p_value, sizeof(T));
	}
};

inline std::vector<uint8_t> echo_hold_mask() {
	net::Snapshot s;
	const std::vector<uint8_t> zero = net::encode_snapshot(s);
	s.echo_hold_100us = 0xFFFFu;
	const std::vector<uint8_t> ones = net::encode_snapshot(s);
	REQUIRE(zero.size() == ones.size());
	std::vector<uint8_t> mask(zero.size());
	int bits = 0;
	for (size_t i = 0; i < mask.size(); ++i) {
		mask[i] = uint8_t(zero[i] ^ ones[i]);
		bits += std::popcount(mask[i]);
	}
	CHECK(bits == 16);
	return mask;
}

struct DecisionRecord final : host::TdmBotObserver {
	std::shared_ptr<host::TdmBotObserver> inner;
	std::vector<uint64_t> per_tick;
	sim::Tick last_tick = 0;
	Fnv all;
	uint64_t count = 0;

	void detected(const host::ServerView &p_view, sim::Tick p_tick, uint8_t p_bot, uint8_t p_target, const sim::Vec3 &p_eye) override {
		if (inner != nullptr) {
			inner->detected(p_view, p_tick, p_bot, p_target, p_eye);
		}
	}
	void fired(const host::ServerView &p_view, sim::Tick p_tick, uint8_t p_bot, int p_target, sim::Tick p_last_visible, bool p_blind) override {
		if (inner != nullptr) {
			inner->fired(p_view, p_tick, p_bot, p_target, p_last_visible, p_blind);
		}
	}
	void activity(sim::Tick p_tick, uint8_t p_bot, host::TdmBotActivity p_activity) override {
		if (inner != nullptr) {
			inner->activity(p_tick, p_bot, p_activity);
		}
	}
	void commanded(sim::Tick p_tick, uint8_t p_bot, host::TdmBotActivity p_activity, const sim::Command &p_command) override {
		if (inner != nullptr) {
			inner->commanded(p_tick, p_bot, p_activity, p_command);
		}
		if (per_tick.empty() || p_tick != last_tick) {
			per_tick.push_back(Fnv{}.h);
			last_tick = p_tick;
		}
		Fnv tick{ per_tick.back() };
		for (Fnv *f : { &tick, &all }) {
			f->add(p_tick);
			f->add(p_bot);
			f->add(p_activity);
			f->add(p_command.tick);
			f->add(p_command.sequence);
			f->add(p_command.buttons);
			f->add(p_command.move_forward);
			f->add(p_command.move_right);
			f->add(p_command.yaw);
			f->add(p_command.pitch);
			f->add(p_command.pose);
			f->add(p_command.flags);
			f->add(p_command.view_back_q8);
			f->add(p_command.view_quality);
			f->add(p_command.view_seq8);
		}
		per_tick.back() = tick.h;
		++count;
	}
};

inline std::string default_mechanics_cfg() {
#ifdef OPENBF3_MECHANICS_CFG
	return OPENBF3_MECHANICS_CFG;
#else
	return std::string(sim::weapons::kMechanicsTuningPath);
#endif
}

inline std::string default_tdm_tuning_cfg() {
#ifdef OPENBF3_TDM_TUNING_CFG
	return OPENBF3_TDM_TUNING_CFG;
#else
	return std::string();
#endif
}

}

#ifdef OPENBF3_MOTOR_TUNING_CFG
inline TdmLevel *load_tdm_level_once() {
	static std::unique_ptr<TdmLevel> s_level;
	static bool s_tried = false;
	if (s_tried) {
		if (s_level == nullptr) {
			bf3::test::skip_install_case("no BF3 install or no TDM load (see the first TDM case)");
		}
		return s_level.get();
	}
	s_tried = true;
	auto l = std::make_unique<TdmLevel>();
	l->reader = std::make_unique<core::InstallReader>();
	if (!l->reader->locate({})) {
		bf3::test::skip_install_case(l->reader->describe().c_str());
		return nullptr;
	}
	l->before = l->reader->snapshot();
	core::Result<std::shared_ptr<core::Install>> opened = core::Install::open(*l->reader);
	REQUIRE_MESSAGE(opened.ok(), (opened.ok() ? std::string() : opened.error().message));
	if (!opened.ok()) {
		return nullptr;
	}
	l->install = *opened;

	l->cache_root = std::filesystem::temp_directory_path() /
			("openbf3-tdm-" + std::to_string(uint64_t(std::chrono::steady_clock::now().time_since_epoch().count())));
	std::error_code ec;
	std::filesystem::remove_all(l->cache_root, ec);
	core::Cache cache;
	core::CacheOptions co;
	co.root = l->cache_root;
	co.guard = l->reader->write_guard();
	std::string detail_text;
	REQUIRE_MESSAGE(cache.open(std::move(co), &detail_text) == core::CacheStatus::Ok, detail_text);
	host::SharedLoadOptions options;
	options.cache = &cache;
	options.tuning = bf3::test::gunplay::shipped_motor_tuning();
	options.gunplay = true;
	options.tdm = true;
	options.tdm_tuning_cfg = detail::default_tdm_tuning_cfg();
	host::LoadProgress progress;
	const auto start = std::chrono::steady_clock::now();
	core::Result<host::LevelDatabase> db = host::open_level_database(l->install, "MP_Subway", "TeamDeathMatch0", 0, &progress);
	REQUIRE_MESSAGE(db.ok(), (db.ok() ? std::string() : db.error().message));
	if (!db.ok()) {
		return nullptr;
	}
	core::Result<std::shared_ptr<const host::SharedLevel>> shared = host::load_shared_level(*db, options, &progress);
	REQUIRE_MESSAGE(shared.ok(), (shared.ok() ? std::string() : shared.error().message));
	if (!shared.ok()) {
		return nullptr;
	}
	l->shared = *shared;
	const double seconds_shared = detail::seconds_since(start);
	host::BotLoadOptions bo;
	bo.cache = &cache;
	bo.threads = std::max(1u, std::thread::hardware_concurrency() / 2u);
	bo.seed = 1;
	core::Result<std::shared_ptr<const host::BotLevel>> bots = host::load_bot_level(*db, *l->shared, bo, nullptr);
	REQUIRE_MESSAGE(bots.ok(), (bots.ok() ? std::string() : bots.error().message));
	if (!bots.ok()) {
		return nullptr;
	}
	l->bots = *bots;
	cache.close();
	l->seconds_load = detail::seconds_since(start);
	detail::frame() = host::shot_frame(*l->shared->static_world);
	std::printf("tdm fixture: Metro TDM loaded in %.2f s (shared %.2f s: gunplay %.2f s, logic %.2f s; bots %.2f s), %zu spawns, logic digest "
				"%016llx\n",
			l->seconds_load, seconds_shared, l->shared->seconds_gunplay, l->shared->seconds_logic, l->seconds_load - seconds_shared,
			l->shared->spawns.size(), (unsigned long long)l->shared->logic_digest);
	for (const std::string &n : l->shared->tdm_notes) {
		std::printf("tdm fixture: note: %s\n", n.c_str());
	}
	if (l->shared->logic == nullptr || l->shared->gunplay == nullptr) {
		FAIL("the TDM load made no logic graph or no gunplay tables");
		return nullptr;
	}
	s_level = std::move(l);
	return s_level.get();
}
#endif

inline void check_install_unchanged(TdmLevel &r_level) {
	const core::SnapshotDiff diff = core::diff_snapshots(r_level.before, r_level.reader->snapshot());
	CHECK_MESSAGE(diff.clean(), diff.summary());
	std::printf("install.unchanged: %s (%zu files)\n", diff.clean() ? "clean" : diff.summary().c_str(), r_level.before.files.size());
	std::error_code ec;
	std::filesystem::remove_all(r_level.cache_root, ec);
}

inline ScriptedPeer::ScriptedPeer(host::GameServer &r_server, uint32_t p_peer, std::string p_name) :
		server_(&r_server), peer_(p_peer), name_(std::move(p_name)) {
	server_->on_connect(peer_);
}

inline void ScriptedPeer::hello() {
	net::Hello h;
	h.name = name_;
	server_->on_receive(peer_, net::kChannelReliable, net::encode_hello(h));
}

inline void ScriptedPeer::ready() {
	server_->on_receive(peer_, net::kChannelReliable, net::encode_ready({}));
}

inline void ScriptedPeer::spawn_request(uint8_t p_kit, uint8_t p_spawn_choice) {
	net::SpawnRequest q;
	q.kit = p_kit;
	q.spawn_choice = p_spawn_choice;
	const std::vector<uint8_t> bytes = net::encode_spawn_request(q);
	if (bytes.empty()) {
		return;
	}
	server_->on_receive(peer_, net::kChannelReliable, bytes);
}

inline void ScriptedPeer::kit_select(const net::KitSelect &p_select) {
	server_->on_receive(peer_, net::kChannelReliable, net::encode_kit_select(p_select));
}

inline void ScriptedPeer::debug(const net::DebugRequest &p_request) {
	server_->on_receive(peer_, net::kChannelReliable, net::encode_debug(p_request));
}

inline void ScriptedPeer::command(sim::Command p_command) {
	p_command.tick = server_->current_tick() + 1u;
	p_command.sequence = ++sequence_;
	net::CommandPacket p;
	p.count = 1;
	p.commands[0] = p_command;
	if (last_snapshot) {
		p.newest_snapshot = last_snapshot->seq;
	}
	server_->on_receive(peer_, net::kChannelUnreliable, net::encode_command_packet(p));
}

inline int ScriptedPeer::slot() const {
	return match_info ? int(match_info->slot) : -1;
}

inline void ScriptedPeer::receive(std::span<const host::OutDatagram> p_out) {
	for (const host::OutDatagram &d : p_out) {
		if (d.peer != peer_) {
			continue;
		}
		const std::optional<net::MessageType> type = net::peek_type(d.bytes);
		if (!type) {
			++decode_errors;
			continue;
		}
		bool ok = true;
		switch (*type) {
			case net::MessageType::MatchInfo: {
				core::Result<net::MatchInfo> m = net::decode_match_info(d.bytes);
				ok = m.ok();
				if (ok) {
					match_info = *m;
				}
				break;
			}
			case net::MessageType::MatchState: {
				core::Result<net::MatchState> m = net::decode_match_state(d.bytes);
				ok = m.ok();
				if (ok) {
					match_state = *m;
					match_states.push_back(*m);
				}
				break;
			}
			case net::MessageType::PlayerList: {
				core::Result<net::PlayerList> m = net::decode_player_list(d.bytes);
				ok = m.ok();
				if (!ok) {
					break;
				}
				if (m->full) {
					players = m->rows;
				} else {
					for (const uint8_t gone : m->removed) {
						std::erase_if(players, [&](const net::PlayerRow &r) { return r.slot == gone; });
					}
					for (const net::PlayerRow &row : m->rows) {
						auto at = std::find_if(players.begin(), players.end(), [&](const net::PlayerRow &r) { return r.slot == row.slot; });
						if (at != players.end()) {
							*at = row;
						} else {
							players.push_back(row);
						}
					}
				}
				std::sort(players.begin(), players.end(), [](const net::PlayerRow &a, const net::PlayerRow &b) { return a.slot < b.slot; });
				break;
			}
			case net::MessageType::Spawned: {
				core::Result<net::Spawned> m = net::decode_spawned(d.bytes);
				ok = m.ok();
				if (ok) {
					spawned.push_back(*m);
				}
				break;
			}
			case net::MessageType::CombatEvents: {
				core::Result<net::CombatEvents> m = net::decode_combat_events(d.bytes, detail::frame());
				ok = m.ok();
				if (ok) {
					combat.push_back(std::move(*m));
				}
				break;
			}
			case net::MessageType::Snapshot: {
				core::Result<net::Snapshot> m = net::decode_snapshot(d.bytes);
				ok = m.ok();
				if (ok) {
					last_snapshot = std::move(*m);
				}
				break;
			}
			case net::MessageType::Kick: {
				core::Result<net::Kick> m = net::decode_kick(d.bytes);
				ok = m.ok();
				if (ok) {
					kick = *m;
				}
				break;
			}
			default:
				break;
		}
		if (!ok) {
			++decode_errors;
		}
	}
}

inline core::Result<TdmRunResult> run_tdm(const TdmLevel &p_level, const TdmRunConfig &p_config) {
	if (p_level.shared == nullptr || p_level.shared->logic == nullptr || p_level.shared->gunplay == nullptr) {
		return core::make_error("run_tdm: the level has no logic graph or no gunplay tables (SharedLoadOptions::tdm / ::gunplay)");
	}
	TdmRunResult r;
	const auto start = std::chrono::steady_clock::now();
	auto server = std::make_unique<host::GameServer>();
	host::GameServerConfig cfg;
	cfg.level = p_level.shared;
	cfg.max_slots = 64;
	cfg.dev_commands = true;
	cfg.seed = p_config.seed;
	cfg.gunplay.mechanics_cfg = p_config.mechanics_cfg.empty() ? detail::default_mechanics_cfg() : p_config.mechanics_cfg;
	cfg.rules = p_config.rules;
	if (p_level.bots != nullptr) {
		auto fill = std::make_shared<host::BotFillConfig>();
		fill->level = p_level.bots;
		fill->count = 0;
		cfg.bots = fill;
	}
	std::shared_ptr<host::TdmBotStats> bot_stats;
	std::shared_ptr<detail::DecisionRecord> decisions;
	if (p_config.bots > 0) {
		auto b = std::make_shared<host::TdmBotConfig>();
		b->count = p_config.bots;
		b->stats = std::make_shared<host::TdmBotStats>();
		if (p_config.configure_bots) {
			p_config.configure_bots(*b);
		}
		if (p_config.record_decisions) {
			decisions = std::make_shared<detail::DecisionRecord>();
			decisions->inner = b->observer;
			b->observer = decisions;
		}
		bot_stats = b->stats;
		cfg.tdm_bots = b;
	}
	if (p_config.configure_server) {
		p_config.configure_server(cfg);
	}
	const core::Status st = server->init(cfg);
	if (!st.ok()) {
		r.failure = "init: " + st.error().message;
		return r;
	}
	const host::TdmRules *rules = server->rules();
	if (rules == nullptr) {
		r.failure = "init made no rules (GameServerConfig::rules.mode)";
		return r;
	}
	std::vector<host::OutDatagram> out;
	std::unique_ptr<ScriptedPeer> peer;
	detail::Fnv match_bytes;
	const auto step = [&] {
		out.clear();
		server->tick(out);
		if (peer != nullptr) {
			peer->receive(out);
			for (const host::OutDatagram &d : out) {
				const std::optional<net::MessageType> t = net::peek_type(d.bytes);
				if (d.peer == peer->peer() && t && (*t == net::MessageType::MatchState || *t == net::MessageType::PlayerList)) {
					match_bytes.bytes(d.bytes.data(), d.bytes.size());
				}
			}
		}
	};
	if (p_config.peer) {
		peer = std::make_unique<ScriptedPeer>(*server, 1u, "peer");
		peer->hello();
		step();
		peer->ready();
		if (p_config.rules.deploy_screen) {
			peer->spawn_request(p_config.peer_kit);
		}
	}
	r.seconds_init = detail::seconds_since(start);
	const auto run_start = std::chrono::steady_clock::now();

	struct Track {
		uint32_t life = 0;
		sim::Tick since = 0;
		sim::Vec3 at{};
		bool counted = false;
	};
	std::vector<Track> tracks(cfg.max_slots);
	sim::Tick peer_respawn = 0;
	sim::Tick last_request = 0;
	size_t combat_seen = 0;
	const sim::Tick first = server->current_tick();
	for (uint32_t i = 0; i < p_config.max_ticks; ++i) {
		if (peer != nullptr && p_config.rules.deploy_screen && peer->slot() >= 0) {
			const sim::SoldierState *own = server->soldier(uint32_t(peer->slot()));
			const bool alive = own != nullptr && (own->flags & sim::soldier_flags::kAlive) != 0;
			const sim::Tick next = server->current_tick() + 1u;
			if (!alive && next >= peer_respawn && next >= last_request + 30u) {
				peer->spawn_request(p_config.peer_kit);
				last_request = next;
			}
		}
		step();
		const sim::Tick now = server->current_tick();
		r.ticks = now - first;
		const host::GameServerTickTimes &tt = server->last_tick_times();
		r.tick_ms.push_back(tt.tick_ms);
		if (bot_stats != nullptr) {
			r.perception_ms.push_back(double(bot_stats->last_perception_ns.load()) * 1e-6);
			r.brains_ms.push_back(double(bot_stats->last_brains_ns.load()) * 1e-6);
		}
		const host::Gunplay *gunplay = server->gunplay();
		const host::ServerView &view = server->view();
		if (p_config.hashes) {
			detail::Fnv h;
			h.add(now);
			for (uint32_t s = 0; s < cfg.max_slots; ++s) {
				const sim::SoldierState *state = server->soldier(s);
				if (state == nullptr) {
					continue;
				}
				h.add(s);
				h.add(sim::soldier_state_hash(*state));
				if (gunplay != nullptr) {
					h.add(sim::weapons::soldier_weapons_hash(gunplay->weapons(uint8_t(s))));
				}
				h.add(view.team(uint8_t(s)));
			}
			const net::MatchState &ms = rules->match_state();
			h.add(ms);
			r.world_hashes.push_back(h.h);
		}
		if (peer != nullptr) {
			for (; combat_seen < peer->combat.size(); ++combat_seen) {
				for (const net::DeathEvent &d : peer->combat[combat_seen].deaths) {
					if (int(d.victim) == peer->slot()) {
						peer_respawn = std::max(peer_respawn, d.respawn_tick);
					}
				}
			}
		}
		for (const net::PlayerRow &row : rules->players()) {
			if (!row.bot || row.slot >= tracks.size()) {
				continue;
			}
			const sim::SoldierState *state = server->soldier(row.slot);
			Track &t = tracks[row.slot];
			const bool alive = state != nullptr && (state->flags & sim::soldier_flags::kAlive) != 0;
			if (!alive || state->life != t.life) {
				t = Track{};
				if (alive) {
					t.life = state->life;
					t.since = now;
					t.at = state->position;
				}
				continue;
			}
			if (now - t.since >= p_config.stuck_window_ticks) {
				const float dx = state->position[0] - t.at[0], dz = state->position[2] - t.at[2];
				if (std::sqrt(dx * dx + dz * dz) < p_config.stuck_metres && !t.counted) {
					++r.stuck_bots;
					t.counted = true;
				}
				t.since = now;
				t.at = state->position;
			}
		}
		if (p_config.on_tick) {
			p_config.on_tick(now, *server, peer.get());
		}
		if (!r.round_over && rules->phase() == net::MatchPhase::RoundOver) {
			r.round_over = true;
			r.round_over_tick = now;
			r.winner = rules->match_state().winner;
		}
		if (r.round_over && now >= r.round_over_tick + p_config.after_round_ticks) {
			break;
		}
	}
	r.seconds_run = detail::seconds_since(run_start);
	const net::MatchState &ms = rules->match_state();
	r.kills[1] = ms.kills[0];
	r.kills[2] = ms.kills[1];
	for (const net::PlayerRow &row : rules->players()) {
		if (row.team == 1 || row.team == 2) {
			r.deaths[row.team] += row.deaths;
		}
	}
	r.team_kd = r.kills[2] != 0 ? double(r.kills[1]) / double(r.kills[2]) : double(r.kills[1]);
	r.rules = rules->stats();
	r.revives = uint32_t(r.rules.revives);
	r.spawns_scored = uint32_t(r.rules.spawns_scored);
	r.spawns_fallback = uint32_t(r.rules.spawn_fallbacks);
	r.spawns_enemy_within_10m = uint32_t(r.rules.spawns_enemy_within_10m);
	r.blind_fire = bot_stats != nullptr ? uint32_t(bot_stats->blind_fire.load()) : 0u;
	r.runtime_faulted = rules->faulted();
	r.trace_hash = rules->trace().hash;
	r.match_bytes_hash = match_bytes.h;
	if (decisions != nullptr) {
		r.decision_hashes = decisions->per_tick;
		r.decision_hash = decisions->all.h;
		r.decisions = decisions->count;
	}
	return r;
}

}

#include "bf3/sim/collision_world.h"
#include "bf3/sim/static_world.h"

#include <cmath>
#include <set>

namespace bf3::test::tdm {

inline constexpr uint32_t kStaleAimTicks = 30;

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
		std::array<uint32_t, host::kTdmBotActivityCount> acts{};
	};
	std::array<Window, 256> win{};

	struct StuckReport {
		uint8_t slot = 0;
		sim::Tick tick = 0;
		sim::Vec3 position{};
		float farthest = 0.0f;
		std::array<uint32_t, host::kTdmBotActivityCount> acts{};
	};
	static constexpr size_t kMaxStuckReports = 16;
	std::vector<StuckReport> stuck_reports;
	std::array<uint32_t, 3> team_slots{};
	host::GunplayStats gunplay{};
	std::vector<net::PlayerRow> players;

	std::array<double, 3> spawn_x{}, spawn_z{}, death_x{}, death_z{};
	std::array<uint32_t, 3> spawn_n{}, death_n{};
	std::array<uint32_t, 256> seen_life{};
	std::array<uint8_t, 256> was_alive{};
	std::array<sim::Vec3, 256> last_position{};
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

	void after_tick(sim::Tick p_tick, host::GameServer &r_server) {
		const std::span<const sim::SoldierState> states = r_server.view().states();
		team_slots = {};
		if (const host::Gunplay *g = r_server.gunplay(); g != nullptr) {
			gunplay = g->stats();
		}
		if (const host::TdmRules *rules = r_server.rules(); rules != nullptr) {
			const std::span<const net::PlayerRow> rows = rules->players();
			players.assign(rows.begin(), rows.end());
		}
		for (size_t s = 0; s < states.size() && s < win.size(); ++s) {
			if (is_bot[s] != 0) {
				const uint8_t t = r_server.view().team(uint8_t(s));
				++team_slots[t < 3 ? t : 0];
				const bool alive = r_server.view().alive(uint8_t(s));
				if (t == 1 || t == 2) {
					if (alive && seen_life[s] != states[s].life + 1u) {
						seen_life[s] = states[s].life + 1u;
						spawn_x[t] += states[s].position[0];
						spawn_z[t] += states[s].position[2];
						++spawn_n[t];
					}
					if (!alive && was_alive[s] != 0) {
						death_x[t] += states[s].position[0];
						death_z[t] += states[s].position[2];
						++death_n[t];
					}
				}
				was_alive[s] = alive ? 1 : 0;
				last_position[s] = states[s].position;
			}
		}
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
				w.acts = {};
				continue;
			}
			++w.acts[size_t(a)];
			const float dx = states[s].position[0] - w.anchor[0], dz = states[s].position[2] - w.anchor[2];
			w.farthest = std::max(w.farthest, std::sqrt(dx * dx + dz * dz));
			if (p_tick - w.start >= window_ticks) {
				++windows;
				if (w.farthest < window_metres) {
					++stuck_windows;
					stuck_slots.insert(uint8_t(s));
					if (stuck_reports.size() < kMaxStuckReports) {
						stuck_reports.push_back({ uint8_t(s), p_tick, states[s].position, w.farthest, w.acts });
					}
				}
				w.start = p_tick;
				w.anchor = states[s].position;
				w.farthest = 0.0f;
				w.acts = {};
			}
		}
		if (stats != nullptr) {
			perception_ms.push_back(double(stats->last_perception_ns.load(std::memory_order_relaxed)) * 1e-6);
			brains_ms.push_back(double(stats->last_brains_ns.load(std::memory_order_relaxed)) * 1e-6);
		}
	}
};

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

}
