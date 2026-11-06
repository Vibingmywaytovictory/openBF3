#include "bf3/host/server_session.h"

#include "bf3/core/cache/disk_cache.h"
#include "bf3/core/install.h"
#include "bf3/host/bot_fill.h"
#include "bf3/host/tdm_brain.h"
#include "bf3/sim/sim_types.h"
#include "bf3/sim/soldier_state.h"

#include <algorithm>
#include <optional>
#include <utility>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <psapi.h>

namespace bf3::host {

namespace {

PROCESS_MEMORY_COUNTERS_EX memory_counters() {
	PROCESS_MEMORY_COUNTERS_EX pmc{};
	pmc.cb = sizeof(pmc);
	if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&pmc), sizeof(pmc))) {
		return {};
	}
	return pmc;
}

}

uint64_t process_private_bytes() {
	return uint64_t(memory_counters().PrivateUsage);
}

uint64_t process_peak_private_bytes() {
	return uint64_t(memory_counters().PeakPagefileUsage);
}

bool release_free_heap_memory() {
	HEAP_OPTIMIZE_RESOURCES_INFORMATION info{};
	info.Version = HEAP_OPTIMIZE_RESOURCES_CURRENT_VERSION;
	return HeapSetInformation(nullptr, HeapOptimizeResources, &info, sizeof(info)) != FALSE;
}

const char *prebake_state_name(PrebakeEntry::State p_state) {
	switch (p_state) {
		case PrebakeEntry::State::Pending:
			return "pending";
		case PrebakeEntry::State::Running:
			return "running";
		case PrebakeEntry::State::Done:
			return "done";
		case PrebakeEntry::State::Failed:
			return "failed";
	}
	return "?";
}

const char *match_phase_name(net::MatchPhase p_phase) {
	switch (p_phase) {
		case net::MatchPhase::Loading:
			return "loading";
		case net::MatchPhase::PreRound:
			return "pre-round";
		case net::MatchPhase::InRound:
			return "in round";
		case net::MatchPhase::RoundOver:
			return "round over";
		case net::MatchPhase::Restarting:
			return "restarting";
		case net::MatchPhase::LevelChange:
			return "level change";
	}
	return "?";
}

ServerSession::ServerSession() = default;

ServerSession::~ServerSession() {
	stop();
}

core::Status ServerSession::start(ServerSessionConfig p_config, LoadProgress *p_progress) {
	if (level_ != nullptr) {
		return core::make_error("server session: already started");
	}
	if (p_config.install == nullptr) {
		return core::make_error("server session: no install");
	}
	if (p_config.maps.entries.empty()) {
		return core::make_error("server session: the map list is empty");
	}
	std::optional<net::Endpoint> master;
	if (!p_config.master.empty()) {
		core::Result<net::Endpoint> at = net::parse_endpoint(p_config.master);
		if (!at) {
			return core::make_error("server session: --master: " + at.error().message);
		}
		if (!net::valid_server_name(p_config.name)) {
			return core::make_error("server session: --name=" + p_config.name +
					": a server name is 1 to 32 of A-Z a-z 0-9 space _ - .");
		}
		master = std::move(*at);
	}
	config_ = std::move(p_config);
	config_.level.bots = config_.rules.bots > 0;
	config_.level.keep_database = false;
	config_.level.stop = &stop_;
	config_.level.after_database = nullptr;
	config_.level.after_shared = nullptr;
	cursor_ = std::make_unique<MapListCursor>(config_.maps);
	stop_.store(false, std::memory_order_release);

	core::Result<ServerLevel> level =
			load_server_level(config_.install, cursor_->current(), config_.level, config_.cache.get(), p_progress);
	if (!level) {
		cursor_.reset();
		return core::error_at(
				"server session " + config_.maps.entries.front().level + " " + config_.maps.entries.front().mode,
				level.error());
	}
	level_ = std::make_unique<ServerLevel>(std::move(*level));
	game_ = std::make_shared<const GameServerConfig>(rotation_config(*level_));
	RotationConfig rc;
	rc.maps = config_.maps;
	rc.first = game_;
	rc.intermission_override_s = config_.intermission_s;
	rc.load = [this](const LevelEntry &p_entry) -> core::Result<std::shared_ptr<const GameServerConfig>> {
		core::Result<ServerLevel> next =
				load_server_level(config_.install, p_entry, config_.level, config_.cache.get(), nullptr);
		if (!next) {
			return next.error();
		}
		return std::make_shared<const GameServerConfig>(rotation_config(*next));
	};
	rotation_ = std::make_shared<Rotation>(std::move(rc));

	ServerConfig sc;
	sc.tick_hz = int(sim::kSimHz);
	sc.bind_ip = config_.bind_ip.c_str();
	sc.port = config_.port;
	sc.game = game_;
	sc.clock = config_.clock;
	sc.rotation = rotation_;
	std::string error;
	if (!host_.start(sc, &error)) {
		level_.reset();
		game_.reset();
		rotation_.reset();
		cursor_.reset();
		return core::make_error("server session: " + error);
	}

	{
		std::lock_guard lock(prebake_mutex_);
		prebake_.clear();
		for (size_t i = 1; i < config_.maps.entries.size(); ++i) {
			PrebakeEntry e;
			e.entry = config_.maps.entries[i];
			prebake_.push_back(std::move(e));
		}
		prebake_finished_ = !config_.prebake || prebake_.empty();
	}
	if (!prebake_finished_) {
		prebake_thread_ = std::thread([this] { run_prebake(); });
	}
	if (master.has_value()) {
		if (const core::Status s = heartbeater_.start(
					*master, [this] { return heartbeat(); }, config_.master_heartbeat_ms);
				!s.ok()) {
			stop();
			return core::make_error("server session: " + s.error().message);
		}
	}
	return {};
}

net::MasterHeartbeat ServerSession::heartbeat() const {
	const GameServerStats g = host_.game_stats();
	net::MasterHeartbeat h;
	h.name = config_.name;
	h.port = host_.port();
	const LevelEntry current = rotation_ != nullptr ? rotation_->stats().current : level_->entry;
	h.level = current.level;
	h.mode = current.mode;
	uint32_t players = 0, bots = 0;
	for (const SlotStats &s : g.slots) {
		players += (s.used && s.remote) ? 1u : 0u;
		bots += (s.used && !s.remote) ? 1u : 0u;
	}
	h.max_players = uint8_t(std::max<uint32_t>(config_.rules.max_players, 1u));
	h.players = uint8_t(std::min<uint32_t>(players, h.max_players));
	h.bots = uint8_t(std::min<uint32_t>(bots, h.max_players - h.players));
	h.protocol = net::kProtocolVersion;
	h.build = game_ != nullptr && game_->game_build.has_value() ? *game_->game_build : net::game_build();
	return h;
}

GameServerConfig ServerSession::rotation_config(const ServerLevel &p_level) const {
	GameServerConfig c = tdm_game_server_config(p_level, config_.rules);
	c.rules.hold_level_end = true;
	c.load = config_.load;
	return c;
}

void ServerSession::run_prebake() {
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
	ServerLevelOptions options = config_.level;
	size_t count = 0;
	{
		std::lock_guard lock(prebake_mutex_);
		count = prebake_.size();
	}
	for (size_t i = 0; i < count && !stop_.load(std::memory_order_acquire); ++i) {
		LevelEntry entry;
		{
			std::lock_guard lock(prebake_mutex_);
			prebake_[i].state = PrebakeEntry::State::Running;
			entry = prebake_[i].entry;
		}
		core::Result<ServerLevel> baked =
				load_server_level(config_.install, entry, options, config_.cache.get(), nullptr);
		std::lock_guard lock(prebake_mutex_);
		PrebakeEntry &e = prebake_[i];
		if (baked) {
			e.state = PrebakeEntry::State::Done;
			e.seconds = baked->seconds;
			e.nav_cache_hit = baked->nav_cache_hit;
		} else {
			e.state = PrebakeEntry::State::Failed;
			e.error = baked.error().message;
		}
	}
	{
		std::lock_guard lock(prebake_mutex_);
		prebake_finished_ = true;
	}
	prebake_done_.notify_all();
}

void ServerSession::stop() {
	stop_.store(true, std::memory_order_release);
	heartbeater_.stop();
	if (prebake_thread_.joinable()) {
		prebake_thread_.join();
	}
	host_.stop();
	rotation_.reset();
}

bool ServerSession::running() const {
	return host_.running();
}

uint16_t ServerSession::port() const {
	return host_.port();
}

std::vector<PrebakeEntry> ServerSession::prebake() const {
	std::lock_guard lock(prebake_mutex_);
	return prebake_;
}

bool ServerSession::wait_prebake(std::chrono::milliseconds p_timeout) const {
	std::unique_lock lock(prebake_mutex_);
	return prebake_done_.wait_for(lock, p_timeout, [this] { return prebake_finished_; });
}

ServerSessionStats ServerSession::stats() {
	ServerSessionStats out;
	out.server = host_.stats();
	out.game = host_.game_stats();
	if (rotation_ != nullptr) {
		out.rotation = rotation_->stats();
	}
	out.prebake = prebake();
	out.private_bytes = process_private_bytes();
	out.peak_private_bytes = process_peak_private_bytes();
	stuck_.sample(out.game, out.rotation.level_changes);
	out.stuck_bots = stuck_.count();
	for (const SlotStats &s : out.game.slots) {
		out.bots += (s.used && !s.remote) ? 1u : 0u;
	}
	if (rotation_ != nullptr) {
		out.kills = int32_t(std::min<int64_t>(out.rotation.kills, INT32_MAX));
	} else if (out.game.rules_active) {
		out.kills = out.game.match.kills[0] + out.game.match.kills[1];
	}
	if (game_ != nullptr && game_->tdm_bots != nullptr && game_->tdm_bots->stats != nullptr) {
		const TdmBotStats &b = *game_->tdm_bots->stats;
		const uint32_t made = b.bots.load(std::memory_order_relaxed);
		const uint32_t bf2 = b.bf2_bots.load(std::memory_order_relaxed);
		out.brain = made == 0 ? "none" : bf2 == made ? "bf2" : "mixed";
		out.bot_decisions = b.decisions.load(std::memory_order_relaxed);
		out.squad_samples = b.squad_samples.load(std::memory_order_relaxed);
		out.squad_near = b.squad_near.load(std::memory_order_relaxed);
		out.squad_orders = b.squad_orders.load(std::memory_order_relaxed);
		uint64_t below = 0;
		for (size_t m = 0; m < ai::kCohesionBins && out.squad_samples > 0; ++m) {
			below += b.squad_metres[m].load(std::memory_order_relaxed);
			if (2u * below >= out.squad_samples) {
				out.squad_median_m = float(m + 1u);
				break;
			}
		}
	}
	out.squad_spawns = rotation_ != nullptr ? out.rotation.squad_spawns : out.game.rules.squad_spawns;
	out.master = heartbeater_.running();
	out.heartbeat = heartbeater_.stats();
	return out;
}

}
