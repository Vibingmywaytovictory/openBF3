#include "spawns_commands.h"

#include "bf3/core/level/level.h"
#include "bf3/core/level/spawn_points.h"
#include "bf3/core/level/world.h"
#include "bf3/core/util/elapsed.h"
#include "bf3/sim/command.h"
#include "bf3/sim/spawn.h"

#include <chrono>
#include <cstdio>

using namespace bf3::core;
using namespace bf3::core::level;

namespace bf3dump {

namespace {

using bf3::core::seconds_since;

std::string partition_of(const EbxResolver &p_db, const EbxInstance &p_entity) {
	return std::string(p_db.name_of(p_entity.partition().guid()).value_or("?")) + "#" + std::to_string(p_entity.index());
}

}

int cmd_spawns(const std::shared_ptr<Install> &p_install, const std::string &p_level, const std::string &p_mode) {
	const auto start = std::chrono::steady_clock::now();
	Result<std::unique_ptr<LoadedLevel>> loaded = load_level(p_install, p_level, p_mode);
	if (!loaded) {
		std::fprintf(stderr, "spawns %s %s: %s\n", p_level.c_str(), p_mode.c_str(), loaded.error().message.c_str());
		return 1;
	}
	const LoadedLevel &level = **loaded;
	Result<std::unique_ptr<World>> world = World::traverse(level);
	if (!world) {
		std::fprintf(stderr, "spawns %s %s: traverse: %s\n", p_level.c_str(), p_mode.c_str(), world.error().message.c_str());
		return 1;
	}
	const Result<LevelSpawns> spawns = read_level_spawns(**world);
	if (!spawns) {
		std::fprintf(stderr, "spawns %s %s: %s\n", p_level.c_str(), p_mode.c_str(), spawns.error().message.c_str());
		return 1;
	}
	const std::vector<bf3::sim::SpawnPoint> points = bf3::sim::to_spawn_points(*spawns);
	const EbxResolver &db = level.db();
	std::printf("# point\tindex\tobject\tx\ty\tz\tyaw_deg\tteam\tenabled\tpriority\tentity\n");
	for (size_t i = 0; i < points.size(); ++i) {
		const bf3::sim::SpawnPoint &p = points[i];
		const SpawnPointDef &d = spawns->points[i];
		std::printf("point\t%u\t%u\t%.3f\t%.3f\t%.3f\t%.2f\t%d\t%d\t%g\t%s\n", p.index, d.object, p.position[0], p.position[1], p.position[2],
				double(bf3::sim::yaw_radians(p.yaw)) * 180.0 / 3.14159265358979323846, p.team, d.enabled ? 1 : 0, double(p.priority),
				partition_of(db, (*world)->objects[d.object].entity).c_str());
	}
	std::printf("# spawner\tobject\tteam\tenabled\tprotection_radius\tfriendly_killed_count\tfriendly_killed_time\tcheck_all_teams\tout_of_sight\t"
				"location_text_sid\tentity\n");
	for (const CharacterSpawnerDef &s : spawns->spawners) {
		std::printf("spawner\t%u\t%d\t%d\t%g\t%lld\t%g\t%d\t%d\t%s\t%s\n", s.object, s.team, s.enabled ? 1 : 0, double(s.protection_radius),
				static_cast<long long>(s.friendly_killed_count), double(s.friendly_killed_time), s.check_all_teams ? 1 : 0, s.out_of_sight ? 1 : 0,
				s.location_text_sid.c_str(), partition_of(db, (*world)->objects[s.object].entity).c_str());
	}
	std::fprintf(stderr, "spawns %s %s: %zu points, %zu spawners in %.3f s\n", p_level.c_str(), p_mode.c_str(), spawns->points.size(),
			spawns->spawners.size(), seconds_since(start));
	return 0;
}

}
