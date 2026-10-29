#include "bf3/host/server_level.h"

#include "bf3/core/install.h"
#include "bf3/core/level/combat_tiles_cache.h"
#include "bf3/core/level/level.h"
#include "bf3/core/util/elapsed.h"
#include "bf3/host/bot_fill.h"
#include "bf3/host/tdm_brain.h"
#include "bf3/host/tdm_rules.h"
#include "bf3/sim/nav.h"
#include "bf3/sim/sim_types.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>

namespace bf3::host {

namespace {

bool stopped(const ServerLevelOptions &p_options) {
	return p_options.stop != nullptr && p_options.stop->load(std::memory_order_acquire);
}

}

core::Result<ServerLevel> load_server_level(std::shared_ptr<const core::Install> p_install, const LevelEntry &p_entry,
		const ServerLevelOptions &p_options, core::Cache *p_cache, LoadProgress *p_progress) {
	const auto start = std::chrono::steady_clock::now();
	ServerLevel out;
	out.entry = p_entry;
	core::Result<LevelDatabase> db = open_level_database(p_install, p_entry.level, p_entry.mode, 0, p_progress);
	if (!db) {
		return db.error();
	}
	out.seconds.database = core::seconds_since(start);
	if (stopped(p_options)) {
		return core::make_error(kServerLevelStopped);
	}
	SharedLoadOptions shared = p_options.shared;
	shared.cache = p_cache;
	if (p_options.after_database) {
		BF3_TRY(p_options.after_database(*db, shared));
	}
	std::optional<core::level::CombatTilesLoad> combat;
	if (p_options.bots) {
		const auto t = std::chrono::steady_clock::now();
		core::Result<core::level::CombatTilesLoad> c =
				core::level::load_level_combat_tiles(p_cache, p_install, *db->level, sim::NavParams().region_margin);
		if (!c) {
			return core::error_at("combat areas", c.error());
		}
		combat = std::move(*c);
		out.combat_cache_hit = combat->cache_hit;
		out.seconds.combat = core::seconds_since(t);
	}
	const auto t_shared = std::chrono::steady_clock::now();
	core::Result<std::shared_ptr<const SharedLevel>> level = load_shared_level(*db, shared, p_progress);
	if (!level) {
		return level.error();
	}
	out.shared = *level;
	out.seconds.shared = core::seconds_since(t_shared);
	if (p_options.after_shared) {
		p_options.after_shared(out.shared);
	}
	if (stopped(p_options)) {
		return core::make_error(kServerLevelStopped);
	}
	if (p_options.bots) {
		const auto t = std::chrono::steady_clock::now();
		BotLoadOptions bot_options;
		bot_options.cache = p_cache;
		bot_options.threads = shared.threads;
		bot_options.combat = &combat->tiles;
		bot_options.seed = p_options.seed;
		bot_options.synthetic_spawns = p_options.synthetic_spawns;
		core::Result<std::shared_ptr<const BotLevel>> bots = load_bot_level(*db, *out.shared, bot_options, p_progress);
		if (!bots) {
			return core::error_at("bots", bots.error());
		}
		out.bots = *bots;
		out.nav_cache_hit = out.bots->nav != nullptr && out.bots->nav->stats().world_hit;
		out.seconds.bots = core::seconds_since(t);
	}
	if (p_options.keep_database) {
		out.database = std::move(*db);
	}
	out.seconds.total = core::seconds_since(start);
	return out;
}

GameServerConfig tdm_game_server_config(const ServerLevel &p_level, const ServerRules &p_rules) {
	GameServerConfig c;
	c.level = p_level.shared;
	const uint8_t max_players = std::clamp<uint8_t>(p_rules.max_players, 1, kServerMaxPlayers);
	c.max_slots = std::max<uint32_t>(kServerSlots, max_players);
	c.dev_commands = p_rules.dev_commands;
	c.seed = p_rules.seed;
	c.respawn_ticks =
			uint32_t(std::clamp(std::lround(p_rules.respawn_s * double(sim::kSimHz)), 1L, 120L * long(sim::kSimHz)));
	c.gunplay.mechanics_cfg = p_rules.mechanics_cfg;
	if (p_level.bots != nullptr) {
		auto fill = std::make_shared<BotFillConfig>();
		fill->level = p_level.bots;
		fill->count = 0;
		fill->spawn_in_main = true;
		c.bots = fill;
	}
	c.rules.mode = MatchRulesConfig::Mode::Tdm;
	c.rules.deploy_screen = p_rules.deploy_screen;
	c.rules.revive = p_rules.revive;
	c.rules.kill_limit_override = p_rules.kill_limit;
	c.rules.preround_override_s = p_rules.preround_s;
	c.rules.min_players_override = p_rules.min_players;
	c.rules.max_players = max_players;
	if (p_rules.bots > 0 && p_level.bots != nullptr) {
		auto bots = std::make_shared<TdmBotConfig>();
		bots->count = std::min<uint32_t>(p_rules.bots, max_players);
		bots->tuning_cfg = p_rules.bot_tuning_cfg;
		c.tdm_bots = bots;
	}
	return c;
}

}
