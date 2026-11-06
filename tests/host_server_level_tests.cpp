#include <doctest/doctest.h>

#include "bf3/core/cache/disk_cache.h"
#include "bf3/core/level/level.h"
#include "bf3/core/level/mode_areas.h"
#include "bf3/core/thread_pool.h"
#include "bf3/host/bot_fill.h"
#include "bf3/host/map_list.h"
#include "bf3/host/server_level.h"
#include "bf3/host/server_session.h"
#include "bf3/host/tdm_brain.h"
#include "bf3/net/master.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/nav.h"

#include "gunplay_fixture.h"
#include "tdm_fixture.h"
#include "test_support.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace bf3;
namespace tt = bf3::test::tdm;

namespace {

std::filesystem::path scratch_cache(const char *p_tag) {
	const std::filesystem::path root = std::filesystem::temp_directory_path() /
			(std::string("openbf3-") + p_tag + "-" +
					std::to_string(uint64_t(std::chrono::steady_clock::now().time_since_epoch().count())));
	std::error_code ec;
	std::filesystem::remove_all(root, ec);
	return root;
}

std::shared_ptr<core::Cache> open_cache(const std::filesystem::path &p_root, const core::InstallReader &p_reader) {
	auto cache = std::make_shared<core::Cache>();
	core::CacheOptions co;
	co.root = p_root;
	co.guard = p_reader.write_guard();
	std::string detail;
	REQUIRE_MESSAGE(cache->open(std::move(co), &detail) == core::CacheStatus::Ok, detail);
	return cache;
}

host::ServerLevelOptions server_options(bool p_bots) {
	host::ServerLevelOptions o;
	o.shared.tuning = bf3::test::gunplay::shipped_motor_tuning();
	o.shared.gunplay = true;
	o.shared.tdm = true;
	o.shared.tdm_tuning_cfg = OPENBF3_TDM_TUNING_CFG;
	o.shared.threads = std::max(1u, std::thread::hardware_concurrency() / 2u);
	o.bots = p_bots;
	return o;
}

void print_level(const char *p_what, const host::ServerLevel &p_level) {
	std::printf("%s %s %s: database %.2f s, combat areas %.2f s, shared %.2f s, bots %.2f s (nav %s), total %.2f s\n",
			p_what, p_level.entry.level.c_str(), p_level.entry.mode.c_str(), p_level.seconds.database,
			p_level.seconds.combat, p_level.seconds.shared, p_level.seconds.bots,
			p_level.nav_cache_hit ? "cache hit" : "baked", p_level.seconds.total);
}

void check_tiles_equal(const core::level::LevelCombatTiles &p_a, const core::level::LevelCombatTiles &p_b) {
	CHECK(p_a.mode == p_b.mode);
	CHECK(p_a.tiles == p_b.tiles);
	CHECK(p_a.water == p_b.water);
	CHECK(p_a.polygon_tiles == p_b.polygon_tiles);
	CHECK(p_a.point_only_tiles == p_b.point_only_tiles);
	CHECK(p_a.points == p_b.points);
	CHECK(p_a.points_outside_areas == p_b.points_outside_areas);
	const core::level::ModeAreas &x = p_a.areas, &y = p_b.areas;
	REQUIRE(x.combat_areas.size() == y.combat_areas.size());
	if (x.combat_areas.size() != y.combat_areas.size()) {
		return;
	}
	for (size_t k = 0; k < x.combat_areas.size(); ++k) {
		CHECK(x.combat_areas[k].trigger == y.combat_areas[k].trigger);
		CHECK(x.combat_areas[k].shape == y.combat_areas[k].shape);
		CHECK(x.combat_areas[k].team == y.combat_areas[k].team);
		CHECK(x.combat_areas[k].polygon == y.combat_areas[k].polygon);
		CHECK(x.combat_areas[k].height == y.combat_areas[k].height);
	}
	REQUIRE(x.objectives.size() == y.objectives.size());
	if (x.objectives.size() != y.objectives.size()) {
		return;
	}
	for (size_t k = 0; k < x.objectives.size(); ++k) {
		CHECK(x.objectives[k].kind == y.objectives[k].kind);
		CHECK(x.objectives[k].object == y.objectives[k].object);
		CHECK(x.objectives[k].position == y.objectives[k].position);
		CHECK(x.objectives[k].team == y.objectives[k].team);
		CHECK(x.objectives[k].scope_label == y.objectives[k].scope_label);
	}
	CHECK(x.unwired_triggers == y.unwired_triggers);
}

void check_listen_config(const host::GameServerConfig &p_c, const host::ServerLevel &p_level) {
	const host::GameServerConfig d;
	CHECK(p_c.level == p_level.shared);
	CHECK(p_c.snapshot_divisor == d.snapshot_divisor);
	CHECK(p_c.max_slots == 64u);
	CHECK(p_c.jitter_buffer_ticks == d.jitter_buffer_ticks);
	CHECK(p_c.respawn_ticks == 300u);
	CHECK(p_c.walkers == 0u);
	CHECK(p_c.dev_commands == false);
	CHECK(p_c.ghost_scope);
	CHECK(p_c.seed == 1u);
	REQUIRE(p_c.bots != nullptr);
	if (p_c.bots == nullptr) {
		return;
	}
	CHECK(p_c.bots->level == p_level.bots);
	CHECK(p_c.bots->count == 0u);
	CHECK(p_c.bots->spawn_in_main);
	CHECK(p_c.gunplay.mechanics_cfg == d.gunplay.mechanics_cfg);
	CHECK(p_c.gunplay.match_seed == d.gunplay.match_seed);
	CHECK(p_c.gunplay.exact_aim == false);
	CHECK(p_c.gunplay.infinite_magazine == false);
	CHECK(p_c.gunplay.friendly_fire == false);
	CHECK(p_c.gunplay.target_bots == 0u);
	CHECK(p_c.gunplay.player_kit == 0u);
	CHECK(p_c.gunplay.player_team == 0u);
	CHECK(p_c.gunplay.observer == nullptr);
	CHECK(p_c.firefight == nullptr);
	CHECK(p_c.rules.mode == host::MatchRulesConfig::Mode::Tdm);
	CHECK(p_c.rules.deploy_screen);
	CHECK(p_c.rules.revive);
	CHECK(p_c.rules.kill_limit_override == 0);
	CHECK(p_c.rules.preround_override_s == -1.0f);
	CHECK(p_c.rules.min_players_override == -1);
	CHECK(p_c.rules.max_players == 32u);
	REQUIRE(p_c.tdm_bots != nullptr);
	if (p_c.tdm_bots == nullptr) {
		return;
	}
	CHECK(p_c.tdm_bots->count == 31u);
	CHECK(p_c.tdm_bots->tuning_cfg == OPENBF3_BOT_TUNING_CFG);
	REQUIRE(p_c.tdm_bots->stats != nullptr);
	CHECK(p_c.tdm_bots->stats->bots.load() == 0u);
	CHECK(p_c.tdm_bots->stats->bf2_bots.load() == 0u);
	CHECK(p_c.tdm_bots->observer == nullptr);
}

}

TEST_SUITE("host-map-list-unit") {
	TEST_CASE("map list: rows of level, mode and rounds; comments; format round trip; the shipped maplist.cfg") {
		core::Result<host::MapList> list = host::parse_map_list(
				"MP_Subway TeamDeathMatch0 1\n# rotation\n\nMP_011 TeamDeathMatch0\n  MP_003\tTeamDeathMatch0  2  \n");
		REQUIRE_MESSAGE(list.ok(), (list.ok() ? std::string() : list.error().message));
		if (!list.ok()) {
			return;
		}
		REQUIRE(list->entries.size() == 3u);
		if (list->entries.size() != 3u) {
			return;
		}
		CHECK(list->entries[0] == host::LevelEntry{ "MP_Subway", "TeamDeathMatch0", 1 });
		CHECK(list->entries[1] == host::LevelEntry{ "MP_011", "TeamDeathMatch0", 1 });
		CHECK(list->entries[2] == host::LevelEntry{ "MP_003", "TeamDeathMatch0", 2 });
		core::Result<host::MapList> again = host::parse_map_list(host::format_map_list(*list));
		REQUIRE(again.ok());
		if (!again.ok()) {
			return;
		}
		CHECK(*again == *list);

		core::Result<host::MapList> shipped = host::load_map_list(OPENBF3_MAPLIST_CFG);
		REQUIRE_MESSAGE(shipped.ok(), (shipped.ok() ? std::string() : shipped.error().message));
		if (!shipped.ok()) {
			return;
		}
		REQUIRE(shipped->entries.size() == 3u);
		if (shipped->entries.size() != 3u) {
			return;
		}
		CHECK(shipped->entries[0] == host::LevelEntry{ "MP_Subway", "TeamDeathMatch0", 1 });
		CHECK(shipped->entries[1] == host::LevelEntry{ "MP_011", "TeamDeathMatch0", 1 });
		CHECK(shipped->entries[2] == host::LevelEntry{ "MP_003", "TeamDeathMatch0", 1 });
	}
	TEST_CASE("map list: every malformed file fails with its line") {
		const auto error_of = [](const std::string &p_text) {
			core::Result<host::MapList> r = host::parse_map_list(p_text);
			return r.ok() ? std::string() : r.error().message;
		};
		CHECK(error_of("MP_Subway") == "map list line 1: expected `level mode [rounds]`");
		CHECK(error_of("MP_Subway TeamDeathMatch0\nMP_011 TeamDeathMatch0 1 2") ==
				"map list line 2: expected `level mode [rounds]`, got more words");
		CHECK(error_of("MP_Subway TeamDeathMatch0 0") == "map list line 1: rounds is not a whole number 1..100");
		CHECK(error_of("MP_Subway TeamDeathMatch0 101") == "map list line 1: rounds is not a whole number 1..100");
		CHECK(error_of("MP_Subway TeamDeathMatch0 1.5") == "map list line 1: rounds is not a whole number 1..100");
		CHECK(error_of("MP-Subway TeamDeathMatch0") ==
				"map list line 1: a level or mode name is not 1..64 letters, digits or underscores");
		CHECK(error_of("level = MP_Subway") == "map list line 1: unknown key `level`");
		CHECK(error_of("# nothing\n\n") == "map list: no entry (each line is `level mode [rounds]`)");
		std::string many;
		for (int i = 0; i < 65; ++i) {
			many += "MP_Subway TeamDeathMatch0\n";
		}
		CHECK(error_of(many) == "map list line 65: more than 64 entries");
		CHECK(error_of(std::string(host::kMaxMapListBytes + 1, ' ')) == "map list: 16385 bytes (limit 16384)");
		CHECK(error_of(std::string(300, 'a') + " b") == "map list line 1: longer than 256 bytes");
		const core::Result<host::MapList> missing = host::load_map_list("no/such/maplist.cfg");
		CHECK_FALSE(missing.ok());
	}
	TEST_CASE("map list cursor: rounds per entry, the wrap, and a one-entry list that restarts in place") {
		core::Result<host::MapList> list =
				host::parse_map_list("MP_Subway TeamDeathMatch0 2\nMP_011 TeamDeathMatch0 1\n");
		REQUIRE(list.ok());
		if (!list.ok()) {
			return;
		}
		host::MapListCursor c(*list);
		CHECK(c.current().level == "MP_Subway");
		CHECK(c.next_level().level == "MP_Subway");
		CHECK_FALSE(c.round_over());
		CHECK(c.current().level == "MP_Subway");
		CHECK(c.next_level().level == "MP_011");
		CHECK(c.round_over());
		CHECK(c.current().level == "MP_011");
		CHECK(c.index() == 1u);
		CHECK(c.round_over());
		CHECK(c.current().level == "MP_Subway");
		core::Result<host::MapList> one = host::parse_map_list("MP_Subway TeamDeathMatch0\n");
		REQUIRE(one.ok());
		if (!one.ok()) {
			return;
		}
		host::MapListCursor o(*one);
		CHECK_FALSE(o.round_over());
		CHECK_FALSE(o.round_over());
		CHECK(o.index() == 0u);
	}
}

TEST_SUITE("host-server-level") {
	TEST_CASE("load_server_level: Metro TDM cold then warm on a scratch cache, the bot region from the mode's own "
			  "combat tiles, "
			  "the listen config field for field") {
		bf3::test::Mounted m;
		if (!bf3::test::open_or_skip(m)) {
			return;
		}
		const std::filesystem::path root = scratch_cache("server-level");
		std::shared_ptr<core::Cache> cache = open_cache(root, m.reader);
		const host::LevelEntry metro{ "MP_Subway", "TeamDeathMatch0", 1 };
		host::ServerLevelOptions options = server_options(true);
		options.keep_database = true;
		core::Result<host::ServerLevel> cold = host::load_server_level(m.install, metro, options, cache.get(), nullptr);
		REQUIRE_MESSAGE(cold.ok(), (cold.ok() ? std::string() : cold.error().message));
		if (!cold.ok()) {
			return;
		}
		print_level("host.server.level cold", *cold);
		CHECK_FALSE(cold->nav_cache_hit);
		REQUIRE(cold->shared != nullptr);
		if (cold->shared == nullptr) {
			return;
		}
		REQUIRE(cold->bots != nullptr);
		if (cold->bots == nullptr) {
			return;
		}
		CHECK(cold->shared->level == "MP_Subway");
		CHECK(cold->shared->logic != nullptr);
		CHECK(cold->shared->gunplay != nullptr);
		REQUIRE(cold->database.level != nullptr);
		if (cold->database.level == nullptr) {
			return;
		}

		core::Result<host::ServerLevel> warm = host::load_server_level(m.install, metro, options, cache.get(), nullptr);
		REQUIRE_MESSAGE(warm.ok(), (warm.ok() ? std::string() : warm.error().message));
		if (!warm.ok()) {
			return;
		}
		print_level("host.server.level warm", *warm);
		CHECK(warm->nav_cache_hit);
		CHECK(warm->shared->source_digest == cold->shared->source_digest);
		CHECK(warm->shared->logic_digest == cold->shared->logic_digest);
		CHECK(warm->bots->region == cold->bots->region);
		CHECK(cold->shared->collision_digest != 0);
		CHECK(warm->shared->collision_digest == cold->shared->collision_digest);
		core::Result<host::ServerLevel> client =
				host::load_server_level(m.install, metro, server_options(false), cache.get(), nullptr);
		REQUIRE_MESSAGE(client.ok(), (client.ok() ? std::string() : client.error().message));
		if (!client.ok()) {
			return;
		}
		CHECK(client->bots == nullptr);
		CHECK(client->shared->collision_digest == warm->shared->collision_digest);
		std::printf(
				"host.server.level: collision digest %016llx with bots, %016llx without (%zu tiles, %zu terrains)\n",
				(unsigned long long)warm->shared->collision_digest,
				(unsigned long long)client->shared->collision_digest, client->shared->static_world->tile_count(),
				client->shared->static_world->terrain_count());

		const float margin = sim::NavParams().region_margin;
		core::Result<core::level::LevelCombatTiles> direct =
				core::level::level_combat_tiles(m.install, "MP_Subway", "TeamDeathMatch0", margin);
		REQUIRE(direct.ok());
		if (!direct.ok()) {
			return;
		}
		CHECK(direct->mode == "TeamDeathMatch0");
		CHECK_FALSE(direct->tiles.empty());
		CHECK(cold->bots->region == direct->tiles);
		REQUIRE(cold->database.world != nullptr);
		if (cold->database.world == nullptr) {
			return;
		}
		core::Result<core::level::LevelCombatTiles> from_world =
				core::level::mode_combat_tiles(*cold->database.level, *cold->database.world, margin);
		REQUIRE(from_world.ok());
		if (!from_world.ok()) {
			return;
		}
		check_tiles_equal(*direct, *from_world);
		std::printf("host.server.level: combat tiles %zu tiles for %s, direct %.2f s, from the loaded world %.3f s\n",
				direct->tiles.size(), direct->mode.c_str(), direct->seconds, from_world->seconds);
		core::Result<std::unique_ptr<core::level::LoadedLevel>> cql_level =
				core::level::load_level(m.install, "MP_Subway", "ConquestLarge0");
		REQUIRE(cql_level.ok());
		if (!cql_level.ok()) {
			return;
		}
		core::Result<std::unique_ptr<core::level::World>> cql_world = core::level::World::traverse(**cql_level);
		REQUIRE(cql_world.ok());
		if (!cql_world.ok()) {
			return;
		}
		core::Result<core::level::LevelCombatTiles> cql =
				core::level::mode_combat_tiles(**cql_level, **cql_world, margin);
		REQUIRE(cql.ok());
		if (!cql.ok()) {
			return;
		}
		CHECK(cql->mode == "ConquestLarge0");
		CHECK(cql->tiles != direct->tiles);
		std::printf("host.server.level: combat tiles %zu for ConquestLarge0 against %zu for TeamDeathMatch0\n",
				cql->tiles.size(), direct->tiles.size());

		host::ServerRules rules;
		rules.bot_tuning_cfg = OPENBF3_BOT_TUNING_CFG;
		check_listen_config(host::tdm_game_server_config(*cold, rules), *cold);
		rules.max_players = 64;
		rules.bots = 63;
		const host::GameServerConfig big = host::tdm_game_server_config(*cold, rules);
		CHECK(big.max_slots == 64u);
		CHECK(big.rules.max_players == 64u);
		REQUIRE(big.tdm_bots != nullptr);
		if (big.tdm_bots == nullptr) {
			return;
		}
		CHECK(big.tdm_bots->count == 63u);
		CHECK(big.tdm_bots->squads);
		rules.bot_squads = false;
		CHECK_FALSE(host::tdm_game_server_config(*cold, rules).tdm_bots->squads);
		rules.max_players = 200;
		CHECK(host::tdm_game_server_config(*cold, rules).rules.max_players == host::kServerMaxPlayers);
		rules.bots = 0;
		CHECK(host::tdm_game_server_config(*cold, rules).tdm_bots == nullptr);
		rules.ghost_scope = false;
		CHECK_FALSE(host::tdm_game_server_config(*cold, rules).ghost_scope);

		cache->close();
		std::error_code ec;
		std::filesystem::remove_all(root, ec);
		const core::SnapshotDiff diff = core::diff_snapshots(m.before, m.reader.snapshot());
		CHECK_MESSAGE(diff.clean(), diff.summary());
		std::printf("install.unchanged: %s (%zu files)\n", diff.clean() ? "clean" : diff.summary().c_str(),
				m.before.files.size());
	}

	TEST_CASE(
			"XP2_Office TeamDeathMatchC0: a spawn on point 27 stands on a floor, and a soldier below the static world "
			"dies a fall death") {
		bf3::test::Mounted m;
		if (!bf3::test::open_or_skip(m)) {
			return;
		}
		const host::ServerLevelOptions options = server_options(false);
		core::Result<host::LevelDatabase> db =
				host::open_level_database(m.install, "XP2_Office", "TeamDeathMatchC0", options.shared.threads, nullptr);
		REQUIRE_MESSAGE(db.ok(), (db.ok() ? std::string() : db.error().message));
		if (!db.ok()) {
			return;
		}
		core::Result<std::shared_ptr<const host::SharedLevel>> loaded =
				host::load_shared_level(*db, options.shared, nullptr);
		REQUIRE_MESSAGE(loaded.ok(), (loaded.ok() ? std::string() : loaded.error().message));
		if (!loaded.ok()) {
			return;
		}
		const std::shared_ptr<const host::SharedLevel> level = *loaded;
		constexpr uint32_t kPoint = 27;
		REQUIRE(level->spawns.size() > kPoint);
		host::GameServerConfig config;
		config.level = level;
		config.max_slots = 2;
		config.dev_commands = true;
		config.gunplay.mechanics_cfg = OPENBF3_MECHANICS_CFG;
		config.rules.mode = host::MatchRulesConfig::Mode::Tdm;
		config.rules.deploy_screen = false;
		config.rules.preround_override_s = 0.0f;
		config.rules.min_players_override = 1;
		host::GameServer server;
		const core::Status started = server.init(config);
		REQUIRE_MESSAGE(started.ok(), (started.ok() ? std::string() : started.error().message));
		if (!started.ok()) {
			return;
		}
		tt::ScriptedPeer peer(server, 1, "office");
		std::vector<host::OutDatagram> out;
		const auto own = [&] { return peer.slot() >= 0 ? server.soldier(uint32_t(peer.slot())) : nullptr; };
		const auto tick = [&] {
			if (const sim::SoldierState *s = own(); tt::is_alive(s)) {
				peer.command(sim::idle_command(server.current_tick() + 1u, s->yaw, s->pitch, s->target_pose));
			}
			out.clear();
			server.tick(out);
			peer.receive(out);
		};
		const auto until = [&](const auto &p_done, uint32_t p_ticks) {
			for (uint32_t i = 0; i < p_ticks && !p_done(); ++i) {
				tick();
			}
			return p_done();
		};
		peer.hello();
		tick();
		REQUIRE(until([&] { return server.rules()->phase() == net::MatchPhase::InRound; }, 240));
		peer.ready();
		REQUIRE(until([&] { return tt::is_alive(own()); }, 10));
		const uint32_t life = own()->life;
		net::DebugRequest spawn_at;
		spawn_at.kind = net::DebugKind::SpawnAt;
		spawn_at.id = 1;
		spawn_at.index = kPoint;
		peer.debug(spawn_at);
		REQUIRE(until([&] { return own()->life != life; }, 5));
		for (uint32_t i = 0; i < 60; ++i) {
			tick();
		}
		const sim::SoldierState spawned = *own();
		core::Result<std::unique_ptr<sim::CollisionWorld>> world = sim::CollisionWorld::create(level->static_world);
		REQUIRE(world.ok());
		if (!world.ok()) {
			return;
		}
		sim::StaticHit floor;
		const bool has_floor =
				(*world)->ground_below({ spawned.position[0], spawned.position[1] + 0.5f, spawned.position[2] }, 1.0f,
						sim::query_mask::kMovement, &floor);
		std::printf("host.server.level XP2_Office: point %u at (%.2f %.2f %.2f), soldier 1 s after the spawn at (%.2f "
					"%.2f %.2f), "
					"grounded %d, floor %s %.3f m below\n",
				kPoint, double(level->spawns[kPoint].position[0]), double(level->spawns[kPoint].position[1]),
				double(level->spawns[kPoint].position[2]), double(spawned.position[0]), double(spawned.position[1]),
				double(spawned.position[2]), int((spawned.flags & sim::soldier_flags::kGrounded) != 0),
				has_floor ? "hit" : "missed", has_floor ? double(spawned.position[1] - floor.position[1]) : 0.0);
		CHECK(tt::is_alive(&spawned));
		CHECK((spawned.flags & sim::soldier_flags::kGrounded) != 0);
		CHECK(has_floor);
		CHECK(spawned.position[1] - floor.position[1] < 0.1f);

		const float lowest = level->static_world->min()[1];
		const float limit = lowest - level->motor.poses[size_t(sim::Pose::Stand)].height;
		net::DebugRequest teleport;
		teleport.kind = net::DebugKind::Teleport;
		teleport.id = 2;
		teleport.position = { spawned.position[0], lowest - 0.5f, spawned.position[2] };
		teleport.yaw = spawned.yaw;
		peer.debug(teleport);
		const size_t deaths_before = peer.combat.size();
		uint32_t ticks = 0;
		while (ticks < 120 && tt::is_alive(own())) {
			tick();
			++ticks;
		}
		const sim::SoldierState dead = *own();
		bool fall_death = false;
		for (size_t i = deaths_before; i < peer.combat.size(); ++i) {
			for (const net::DeathEvent &d : peer.combat[i].deaths) {
				fall_death = fall_death ||
						(d.victim == uint8_t(peer.slot()) && d.killer == d.victim && d.def == sim::weapons::kNoWeapon);
			}
		}
		std::printf("host.server.level XP2_Office: lowest static bound %.2f m, fall-out height %.2f m, teleported to "
					"%.2f m, dead "
					"after %u ticks at %.2f m, fall death %d\n",
				double(lowest), double(limit), double(lowest - 0.5f), ticks, double(dead.position[1]), int(fall_death));
		CHECK(limit < lowest);
		CHECK_FALSE(tt::is_alive(&dead));
		CHECK(fall_death);
		CHECK(dead.position[1] < limit);
		CHECK(ticks <= 60u);

		const core::SnapshotDiff diff = core::diff_snapshots(m.before, m.reader.snapshot());
		CHECK_MESSAGE(diff.clean(), diff.summary());
		std::printf("install.unchanged: %s (%zu files)\n", diff.clean() ? "clean" : diff.summary().c_str(),
				m.before.files.size());
	}
}

TEST_SUITE("host-server-level-prebake") {
	TEST_CASE("ServerSession: Metro first, then a below-normal pre-bake makes MP_011 and MP_003 load as cache hits") {
		bf3::test::Mounted m;
		if (!bf3::test::open_or_skip(m)) {
			return;
		}
		const std::filesystem::path root = scratch_cache("server-prebake");
		host::ServerSessionConfig config;
		config.install = m.install;
		config.cache = open_cache(root, m.reader);
		core::Result<host::MapList> maps = host::load_map_list(OPENBF3_MAPLIST_CFG);
		REQUIRE(maps.ok());
		if (!maps.ok()) {
			return;
		}
		config.maps = *maps;
		config.level = server_options(true);
		config.rules.bot_tuning_cfg = OPENBF3_BOT_TUNING_CFG;
		config.rules.mechanics_cfg = OPENBF3_MECHANICS_CFG;
		config.rules.bots = 31;
		host::ServerSession session;
		const auto start = std::chrono::steady_clock::now();
		const core::Status started = session.start(config);
		REQUIRE_MESSAGE(started.ok(), (started.ok() ? std::string() : started.error().message));
		if (!started.ok()) {
			return;
		}
		const double seconds_first = core::seconds_since(start);
		print_level("host.server.level.prebake first", session.level());
		CHECK(session.port() != 0u);
		CHECK(session.level().entry.level == "MP_Subway");
		CHECK(session.game_config().tdm_bots != nullptr);
		REQUIRE(session.wait_prebake(std::chrono::seconds(240)));
		if (!session.wait_prebake(std::chrono::seconds(240))) {
			return;
		}
		const std::vector<host::PrebakeEntry> baked = session.prebake();
		REQUIRE(baked.size() == 2u);
		if (baked.size() != 2u) {
			return;
		}
		for (const host::PrebakeEntry &e : baked) {
			std::printf("host.server.level.prebake: %s %s %s in %.2f s (database %.2f, combat %.2f, shared %.2f, bots "
						"%.2f)%s%s\n",
					e.entry.level.c_str(), e.entry.mode.c_str(), host::prebake_state_name(e.state), e.seconds.total,
					e.seconds.database, e.seconds.combat, e.seconds.shared, e.seconds.bots, e.error.empty() ? "" : ": ",
					e.error.c_str());
			CHECK(e.state == host::PrebakeEntry::State::Done);
			CHECK_FALSE(e.nav_cache_hit);
		}
		const host::ServerSessionStats running = session.stats();
		CHECK(running.server.ticks > 0u);
		CHECK(running.private_bytes > 0u);
		CHECK(running.game.rules_active);
		for (size_t i = 1; i < maps->entries.size(); ++i) {
			core::Result<host::ServerLevel> warm =
					host::load_server_level(m.install, maps->entries[i], config.level, config.cache.get(), nullptr);
			REQUIRE_MESSAGE(warm.ok(), (warm.ok() ? std::string() : warm.error().message));
			if (!warm.ok()) {
				return;
			}
			print_level("host.server.level.prebake warm", *warm);
			CHECK(warm->nav_cache_hit);
		}
		session.stop();
		CHECK_FALSE(session.running());
		std::printf("host.server.level.prebake: first level and port in %.2f s\n", seconds_first);
		config.cache->close();
		std::error_code ec;
		std::filesystem::remove_all(root, ec);
		const core::SnapshotDiff diff = core::diff_snapshots(m.before, m.reader.snapshot());
		CHECK_MESSAGE(diff.clean(), diff.summary());
		std::printf("install.unchanged: %s (%zu files)\n", diff.clean() ? "clean" : diff.summary().c_str(),
				m.before.files.size());
	}
}

TEST_SUITE("host-tdm-levels") {
	TEST_CASE("bots-only TDM rounds to 20 kills on MP_011 and MP_003: round over, no stuck bot, no sight through "
			  "walls, no runtime fault") {
		bf3::test::Mounted m;
		if (!bf3::test::open_or_skip(m)) {
			return;
		}
		const std::filesystem::path root = scratch_cache("tdm-levels");
		std::shared_ptr<core::Cache> cache = open_cache(root, m.reader);
		const std::vector<host::LevelEntry> levels{ { "MP_011", "TeamDeathMatch0", 1 },
			{ "MP_003", "TeamDeathMatch0", 1 } };
		struct Run {
			std::string error;
			tt::TdmRunResult result;
			uint32_t stuck_m2a = 0;
		};
		const std::vector<Run> runs = core::parallel_map<Run>(
				levels.size(),
				[&](size_t i) {
					Run run;
					core::Result<host::ServerLevel> loaded =
							host::load_server_level(m.install, levels[i], server_options(true), cache.get(), nullptr);
					if (!loaded) {
						run.error = loaded.error().message;
						return run;
					}
					print_level("host.tdm.levels", *loaded);
					tt::TdmLevel level;
					level.install = m.install;
					level.shared = loaded->shared;
					level.bots = loaded->bots;
					tt::TdmRunConfig c;
					c.seed = 1;
					c.bots = 32;
					c.peer = false;
					c.rules.mode = host::MatchRulesConfig::Mode::Tdm;
					c.rules.deploy_screen = false;
					c.rules.revive = true;
					c.rules.kill_limit_override = 20;
					c.rules.preround_override_s = 0.0f;
					c.rules.max_players = 32;
					c.after_round_ticks = 0;
					c.configure_bots = [](host::TdmBotConfig &r_bots) { r_bots.tuning_cfg = OPENBF3_BOT_TUNING_CFG; };
					const std::shared_ptr<tt::BotChecks> checks = tt::attach_bot_checks(c, &level);
					core::Result<tt::TdmRunResult> r = tt::run_tdm(level, c);
					if (!r) {
						run.error = r.error().message;
						return run;
					}
					run.result = std::move(*r);
					tt::apply_bot_checks(*checks, run.result, &run.stuck_m2a);
					return run;
				},
				2);
		for (size_t i = 0; i < runs.size(); ++i) {
			CHECK_MESSAGE(runs[i].error.empty(), (levels[i].level + ": " + runs[i].error));
			if (!runs[i].error.empty()) {
				continue;
			}
			const tt::TdmRunResult &r = runs[i].result;
			std::printf("host.tdm.levels %s: ticks %u, round over %d, winner %u, kills %d / %d, revives %u, stuck %u "
						"(run_tdm's own %u), sight %u, stale aim %u, "
						"spawns enemy within 10 m %u, runtime faults %llu, run %.2f s\n",
					levels[i].level.c_str(), r.ticks, int(r.round_over), unsigned(r.winner), r.kills[1], r.kills[2],
					r.revives, r.stuck_bots, runs[i].stuck_m2a, r.sight_violations, r.stale_aim_shots,
					r.spawns_enemy_within_10m, (unsigned long long)r.rules.runtime_faults, r.seconds_run);
			CHECK_MESSAGE(r.failure.empty(), r.failure);
			CHECK(r.round_over);
			CHECK(std::max(r.kills[1], r.kills[2]) >= 20);
			CHECK_FALSE(r.runtime_faulted);
			CHECK(r.rules.runtime_faults == 0u);
			CHECK(r.stuck_bots == 0u);
			CHECK(r.sight_violations == 0u);
			CHECK(r.stale_aim_shots == 0u);
		}
		cache->close();
		std::error_code ec;
		std::filesystem::remove_all(root, ec);
		const core::SnapshotDiff diff = core::diff_snapshots(m.before, m.reader.snapshot());
		CHECK_MESSAGE(diff.clean(), diff.summary());
		std::printf("install.unchanged: %s (%zu files)\n", diff.clean() ? "clean" : diff.summary().c_str(),
				m.before.files.size());
	}
}

TEST_SUITE("host-server-session-master") {
	TEST_CASE("ServerSession: a bad --master or --name is refused before the load, and given --master it heartbeats "
			  "its name, port, level, mode and players to the master, which lists it and keeps it fresh") {
		bf3::test::Mounted m;
		if (!bf3::test::open_or_skip(m)) {
			return;
		}
		{
			host::ServerSessionConfig bad;
			bad.install = m.install;
			bad.maps.entries.push_back(host::LevelEntry{ "MP_Subway", "TeamDeathMatch0", 1 });
			bad.master = "127.0.0.1";
			bad.name = "alpha";
			host::ServerSession refused;
			const core::Status no_port = refused.start(bad);
			REQUIRE_FALSE(no_port.ok());
			CHECK(no_port.error().message.find("--master") != std::string::npos);
			bad.master = "127.0.0.1:25250";
			bad.name = "al@pha";
			const core::Status bad_name = refused.start(bad);
			REQUIRE_FALSE(bad_name.ok());
			CHECK(bad_name.error().message.find("--name=al@pha") != std::string::npos);
			CHECK_FALSE(refused.running());
		}
		net::MasterServerConfig mc;
		mc.port = 0;
		core::Result<std::unique_ptr<net::MasterServer>> made = net::MasterServer::start(mc);
		REQUIRE_MESSAGE(made.ok(), (made.ok() ? std::string() : made.error().message));
		net::MasterServer &master = **made;
		std::mutex master_mutex;
		std::atomic<bool> stop{ false };
		std::thread service([&] {
			while (!stop.load(std::memory_order_acquire)) {
				std::lock_guard lock(master_mutex);
				master.service(5,
						uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
								std::chrono::steady_clock::now().time_since_epoch())
										.count()));
			}
		});
		const std::filesystem::path root = scratch_cache("server-master");
		host::ServerSessionConfig config;
		config.install = m.install;
		config.cache = open_cache(root, m.reader);
		config.maps.entries.push_back(host::LevelEntry{ "MP_Subway", "TeamDeathMatch0", 1 });
		config.level = server_options(false);
		config.rules.bot_tuning_cfg = OPENBF3_BOT_TUNING_CFG;
		config.rules.mechanics_cfg = OPENBF3_MECHANICS_CFG;
		config.master = "127.0.0.1:" + std::to_string(master.port());
		config.name = "session test";
		config.master_heartbeat_ms = 200;
		host::ServerSession session;
		const core::Status started = session.start(config);
		REQUIRE_MESSAGE(started.ok(), (started.ok() ? std::string() : started.error().message));
		const auto listed = [&] {
			std::lock_guard lock(master_mutex);
			return master.list(uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::steady_clock::now().time_since_epoch())
							.count()));
		};
		CHECK(bf3::test::wait_until([&] { return session.stats().heartbeat.acked >= 3; }, std::chrono::seconds(10)));
		const std::vector<net::MasterEntry> entries = listed();
		REQUIRE(entries.size() == 1u);
		const net::MasterHeartbeat &h = entries[0].server;
		CHECK(h.name == "session test");
		CHECK(h.port == session.port());
		CHECK(h.level == "MP_Subway");
		CHECK(h.mode == "TeamDeathMatch0");
		CHECK(h.players == 0u);
		CHECK(h.bots == config.rules.bots);
		CHECK(h.max_players == host::kListenMaxPlayers);
		CHECK(h.protocol == net::kProtocolVersion);
		CHECK(h.build == net::game_build());
		CHECK(entries[0].address == "127.0.0.1");
		const host::ServerSessionStats s = session.stats();
		CHECK(s.master);
		CHECK(s.heartbeat.failed == 0u);
		CHECK(s.heartbeat.refused == 0u);
		{
			std::lock_guard lock(master_mutex);
			CHECK(master.stats().registry.registered == 1u);
			CHECK(master.stats().registry.refreshed >= 2u);
		}
		session.stop();
		CHECK_FALSE(session.stats().master);
		stop.store(true, std::memory_order_release);
		service.join();
		config.cache->close();
		std::error_code ec;
		std::filesystem::remove_all(root, ec);
		const core::SnapshotDiff diff = core::diff_snapshots(m.before, m.reader.snapshot());
		CHECK_MESSAGE(diff.clean(), diff.summary());
		std::printf("install.unchanged: %s (%zu files)\n", diff.clean() ? "clean" : diff.summary().c_str(),
				m.before.files.size());
	}
}
