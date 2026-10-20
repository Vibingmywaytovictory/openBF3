#pragma once

#include "test_support.h"
#include "weapons_support.h"

#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/install_snapshot.h"
#include "bf3/core/level/collision_soup.h"
#include "bf3/core/level/spawn_points.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/spawn.h"
#include "bf3/sim/static_world.h"
#include "bf3/sim/weapons/weapon_install.h"

#include <doctest/doctest.h>

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace bf3::test::weapons {

struct WeaponsInstall {
	bf3::core::InstallReader reader;
	bf3::core::InstallSnapshot before;
	std::shared_ptr<bf3::core::Install> install;
	sw::InstallWeapons weapons;
};

inline double seconds_since(std::chrono::steady_clock::time_point p_start) {
	return std::chrono::duration<double>(std::chrono::steady_clock::now() - p_start).count();
}

inline WeaponsInstall *weapons_install() {
	static std::unique_ptr<WeaponsInstall> s_install;
	static bool s_tried = false;
	static std::string s_why;
	if (!s_tried) {
		s_tried = true;
		auto w = std::make_unique<WeaponsInstall>();
		if (!w->reader.locate({})) {
			s_why = w->reader.describe();
		} else {
			w->before = w->reader.snapshot();
			bf3::core::Result<std::shared_ptr<bf3::core::Install>> opened = bf3::core::Install::open(w->reader);
			REQUIRE_MESSAGE(opened.ok(), (opened.ok() ? std::string() : opened.error().message));
			w->install = *opened;
			bf3::core::Result<sw::InstallWeapons> weapons = sw::open_install_weapons(w->install);
			REQUIRE_MESSAGE(weapons.ok(), (weapons.ok() ? std::string() : weapons.error().message));
			w->weapons = std::move(*weapons);
			std::printf("MP_Subway weapons: %zu variants, %zu grid pairs, %zu capsules, health %.0f, eye %.2f m, sprint recover %.2f s, opened in %.2f s\n",
					w->weapons.catalog->variants().size(), w->weapons.grid_pairs.size(), w->weapons.capsules.size(), double(w->weapons.max_health),
					double(w->weapons.stand_eye_height), double(w->weapons.sprint_recover_seconds), w->weapons.seconds);
			s_install = std::move(w);
		}
	}
	if (s_install == nullptr) {
		bf3::test::skip_install_case(s_why.c_str());
	}
	return s_install.get();
}

inline void check_install_unchanged(const WeaponsInstall &p_w) {
	const bf3::core::SnapshotDiff diff = bf3::core::diff_snapshots(p_w.before, p_w.reader.snapshot());
	const bool clean = p_w.before.valid && diff.clean();
	std::printf("install.unchanged %s\n", clean ? ("yes (" + std::to_string(p_w.before.files.size()) + " files: sizes and mtimes equal)").c_str()
												: ("NO: " + diff.summary()).c_str());
	CHECK(p_w.before.valid);
	CHECK_MESSAGE(diff.clean(), diff.summary());
}

struct MetroWorld {
	bf3::core::level::LevelCollision collision;
	std::shared_ptr<const bf3::sim::StaticWorldShapes> shapes;
	std::unique_ptr<bf3::sim::CollisionWorld> world;
	std::vector<bf3::sim::SpawnPoint> points;
	double seconds = 0;
};
inline MetroWorld *metro_world(WeaponsInstall &p_w, const char *p_mode = "TeamDeathMatch0") {
	static std::unique_ptr<MetroWorld> s_metro;
	static std::string s_mode;
	if (s_metro != nullptr && s_mode == p_mode) {
		return s_metro.get();
	}
	const auto start = std::chrono::steady_clock::now();
	auto m = std::make_unique<MetroWorld>();
	bf3::core::Result<bf3::core::level::LevelCollision> c = bf3::core::level::load_level_collision(p_w.install, "MP_Subway", p_mode);
	REQUIRE_MESSAGE(c.ok(), (c.ok() ? std::string() : c.error().message));
	m->collision = std::move(*c);
	bf3::core::Result<std::shared_ptr<const bf3::sim::StaticWorldShapes>> shapes = bf3::sim::build_static_world(m->collision.soup);
	REQUIRE_MESSAGE(shapes.ok(), (shapes.ok() ? std::string() : shapes.error().message));
	m->shapes = *shapes;
	bf3::core::Result<std::unique_ptr<bf3::sim::CollisionWorld>> world = bf3::sim::CollisionWorld::create(m->shapes);
	REQUIRE_MESSAGE(world.ok(), (world.ok() ? std::string() : world.error().message));
	m->world = std::move(*world);
	bf3::core::Result<bf3::core::level::LevelSpawns> spawns = bf3::core::level::read_level_spawns(*m->collision.world);
	REQUIRE_MESSAGE(spawns.ok(), (spawns.ok() ? std::string() : spawns.error().message));
	m->points = bf3::sim::to_spawn_points(*spawns);
	m->seconds = seconds_since(start);
	std::printf("Metro %s: %zu soup triangles, %zu tiles, %zu spawn points, loaded in %.2f s\n", p_mode, m->collision.soup.triangles, m->shapes->tile_count(),
			m->points.size(), m->seconds);
	s_metro = std::move(m);
	s_mode = p_mode;
	return s_metro.get();
}

}
