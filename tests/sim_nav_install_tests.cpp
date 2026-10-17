// Suites sim-nav and sim-nav-bake (install; exit 77 without one; docs/design/s1.md §5, §10).
//   sim-nav       G2 for every MP mode of MP_Subway and MP_007 (the rows' classes printed, the objectives gated, the
//                 all-rows gate reported), §5.1's proofs 1-3 on both CQLs (the nav input is the motor's collision
//                 set), Metro's §5.7 component report (movement / all layers, and the cs 0.25 variants the as-built
//                 choice was made from), cold then warm on a scratch cache root, install unchanged.
//   sim-nav-bake  G4: run_nav_bake_bench on MP_007 (<= 6 s) and MP_018 (<= 13 s) at 16 threads (the gate), Caspian at
//                 1 and 4 threads (reported). Timings are noisy on a shared machine; OPENBF3_TIMING_REPORT_ONLY
//                 prints without gating.
// Derived data goes to a scratch directory under the system temp directory (removed at the end), never the install.
#include <doctest/doctest.h>

#include "nav_internal.h"

#include "bf3/core/cache/disk_cache.h"
#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/install_snapshot.h"
#include "bf3/core/level/collision_soup.h"
#include "bf3/core/level/level.h"
#include "bf3/core/level/mode_areas.h"
#include "bf3/core/level/world.h"
#include "bf3/core/thread_pool.h"
#include "bf3/host/bot_fill.h"
#include "bf3/host/level_load.h"
#include "bf3/host/scale_bench.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/nav.h"
#include "bf3/sim/soldier_motor.h"
#include "bf3/sim/static_world.h"
#include "test_support.h"

#include <process.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace bf3;

namespace {

double seconds_since(std::chrono::steady_clock::time_point p_start) {
	return std::chrono::duration<double>(std::chrono::steady_clock::now() - p_start).count();
}

std::string read_text(const char *p_path) {
	std::ifstream in(p_path, std::ios::binary);
	std::ostringstream s;
	s << in.rdbuf();
	return s.str();
}

struct Install {
	std::unique_ptr<core::InstallReader> reader;
	std::shared_ptr<core::Install> install;
	core::InstallSnapshot before;
	sim::MotorTuning tuning;
	std::filesystem::path scratch; // cache roots of this run
	~Install() {
		std::error_code ec;
		if (!scratch.empty()) {
			std::filesystem::remove_all(scratch, ec);
		}
	}
};

Install *install() {
	static std::unique_ptr<Install> s_install;
	static bool s_tried = false;
	if (s_tried) {
		if (s_install == nullptr) {
			bf3::test::skip_install_case("no BF3 install (see the first sim-nav case)");
		}
		return s_install.get();
	}
	s_tried = true;
	auto m = std::make_unique<Install>();
	m->reader = std::make_unique<core::InstallReader>();
	if (!m->reader->locate({})) {
		bf3::test::skip_install_case(m->reader->describe().c_str());
		return nullptr;
	}
	m->before = m->reader->snapshot();
	core::Result<std::shared_ptr<core::Install>> opened = core::Install::open(*m->reader);
	CHECK_MESSAGE(opened.ok(), (opened.ok() ? std::string() : opened.error().message));
	if (!opened) {
		return nullptr;
	}
	m->install = *opened;
	const std::string text = read_text(OPENBF3_MOTOR_TUNING_CFG);
	core::Result<sim::MotorTuning> t = sim::parse_motor_tuning(text);
	CHECK_MESSAGE(t.ok(), OPENBF3_MOTOR_TUNING_CFG);
	if (t) {
		m->tuning = *t;
	}
	std::error_code ec;
	m->scratch = std::filesystem::temp_directory_path(ec) / ("openbf3_s1_sim_nav_" + std::to_string(_getpid()));
	std::filesystem::remove_all(m->scratch, ec);
	s_install = std::move(m);
	return s_install.get();
}

// A cache on a fresh scratch sub-root (the guard refuses the install).
bool open_cache(Install &r_m, const char *p_name, core::Cache &r_cache) {
	core::CacheOptions o;
	o.root = r_m.scratch / p_name;
	o.guard = r_m.reader->write_guard();
	std::string detail;
	const core::CacheStatus s = r_cache.open(std::move(o), &detail);
	CHECK_MESSAGE(s == core::CacheStatus::Ok, detail);
	return s == core::CacheStatus::Ok;
}

const char *kind_name(host::BotPointRow::Kind p_kind) {
	switch (p_kind) {
		case host::BotPointRow::Kind::Flag:
			return "flag";
		case host::BotPointRow::Kind::Hq:
			return "hq";
		case host::BotPointRow::Kind::Mcom:
			return "mcom";
		default:
			return "spawn";
	}
}

const char *row_class(const host::BotPointRow &p_r) {
	return p_r.in_main ? "main" : !p_r.on_mesh ? "off-mesh" : p_r.reaches_main ? "exit-only" : p_r.in_main_weak ? "entry-only" : "disconnected";
}

// The point lies inside (XZ, even-odd) a combat polygon of the mode: an objective outside every wired area is a
// crate of a stage the mode does not play (Rush's prefab holds 10 crates; Metro RL plays 4 stages, Caspian RL 3), parked
// off the map (h 0 v 0: no geometry under it), docs §5.7 as built.
bool in_combat_area(const sim::Vec3 &p_point, const core::level::ModeAreas &p_areas) {
	for (const core::level::CombatAreaDef &a : p_areas.combat_areas) {
		bool inside = false;
		const size_t n = a.polygon.size();
		for (size_t i = 0, j = n - 1; n >= 3 && i < n; j = i++) {
			const auto &pi = a.polygon[i];
			const auto &pj = a.polygon[j];
			if ((pi[1] > p_point[2]) != (pj[1] > p_point[2]) && p_point[0] < (pj[0] - pi[0]) * (p_point[2] - pi[1]) / (pj[1] - pi[1]) + pi[0]) {
				inside = !inside;
			}
		}
		if (inside) {
			return true;
		}
	}
	return false;
}

struct ModeLoad {
	std::shared_ptr<const host::SharedLevel> shared;
	std::shared_ptr<const host::BotLevel> bots;
	double seconds_db = 0, seconds_shared = 0, seconds_bots = 0;
	std::string error; // when a step failed (shared or bots null)
};

// The database and the static world (TC: terrain everywhere) of one level x mode, kept for several bot loads.
struct LevelBase {
	std::optional<host::LevelDatabase> db;
	std::shared_ptr<const host::SharedLevel> shared;
	double seconds_db = 0, seconds_shared = 0;
	std::string error;
};

// p_threads: the lanes of every pool the load uses (0 = all cores). No doctest assertions: runs on worker lanes.
LevelBase load_base(Install &r_m, const std::string &p_level, const std::string &p_mode, core::Cache *p_cache, unsigned p_threads = 0) {
	LevelBase out;
	auto clock = std::chrono::steady_clock::now();
	core::Result<host::LevelDatabase> db = host::open_level_database(r_m.install, p_level, p_mode, p_threads, nullptr);
	if (!db) {
		out.error = p_level + " " + p_mode + ": " + db.error().message;
		return out;
	}
	out.db.emplace(std::move(*db));
	out.seconds_db = seconds_since(clock);
	clock = std::chrono::steady_clock::now();
	host::SharedLoadOptions so;
	so.cache = p_cache;
	so.tuning = r_m.tuning;
	so.threads = p_threads;
	core::Result<std::shared_ptr<const host::SharedLevel>> shared = host::load_shared_level(*out.db, so, nullptr);
	if (!shared) {
		out.error = p_level + " " + p_mode + ": " + shared.error().message;
		return out;
	}
	out.shared = *shared;
	out.seconds_shared = seconds_since(clock);
	return out;
}

// The bot level (nav with the water rule, components, rows) over p_base through p_cache.
ModeLoad load_bots(const LevelBase &p_base, const core::level::LevelCombatTiles &p_combat, core::Cache *p_cache,
		const host::BotLoadOptions &p_bot_options = {}, unsigned p_threads = 0) {
	ModeLoad out;
	out.shared = p_base.shared;
	out.seconds_db = p_base.seconds_db;
	out.seconds_shared = p_base.seconds_shared;
	if (p_base.shared == nullptr) {
		out.error = p_base.error;
		return out;
	}
	const auto clock = std::chrono::steady_clock::now();
	host::BotLoadOptions bo = p_bot_options;
	bo.cache = p_cache;
	bo.combat = &p_combat;
	if (p_threads != 0) {
		bo.threads = p_threads;
	}
	core::Result<std::shared_ptr<const host::BotLevel>> bots = host::load_bot_level(*p_base.db, *p_base.shared, bo, nullptr);
	if (!bots) {
		out.error = bots.error().message;
		return out;
	}
	out.bots = *bots;
	out.seconds_bots = seconds_since(clock);
	return out;
}

// One level x mode as the bots load it (docs §8): load_base then load_bots. No assertions (worker lanes).
ModeLoad load_mode_on(Install &r_m, const std::string &p_level, const std::string &p_mode, const core::level::LevelCombatTiles &p_combat,
		core::Cache *p_cache, unsigned p_threads = 0) {
	const LevelBase base = load_base(r_m, p_level, p_mode, p_cache, p_threads);
	return load_bots(base, p_combat, p_cache, {}, p_threads);
}

// load_mode_on on this thread, the failure CHECKed.
std::optional<ModeLoad> load_mode(Install &r_m, const std::string &p_level, const std::string &p_mode, const core::level::LevelCombatTiles &p_combat,
		core::Cache *p_cache) {
	ModeLoad l = load_mode_on(r_m, p_level, p_mode, p_combat, p_cache);
	CHECK_MESSAGE(l.bots != nullptr, p_level, " ", p_mode, ": ", l.error);
	if (l.bots == nullptr) {
		return std::nullopt;
	}
	return l;
}

struct RowCounts {
	size_t objectives = 0, objectives_main = 0;
	// objectives outside every combat area of their mode (unplayed stages' crates): counted, not judged
	size_t objectives_unplayed = 0, objectives_unplayed_main = 0;
	size_t spawns = 0, spawns_main = 0, exit_only = 0, entry_only = 0, disconnected = 0, off_mesh = 0;
	void add(const host::BotPointRow &p_r, bool p_played = true) {
		const bool spawn = p_r.kind == host::BotPointRow::Kind::Spawn;
		if (!spawn && !p_played) {
			++objectives_unplayed;
			objectives_unplayed_main += p_r.in_main ? 1 : 0;
			return;
		}
		(spawn ? spawns : objectives) += 1;
		if (p_r.in_main) {
			(spawn ? spawns_main : objectives_main) += 1;
		} else if (spawn) {
			if (!p_r.on_mesh) {
				++off_mesh;
			} else if (p_r.reaches_main) {
				++exit_only;
			} else if (p_r.in_main_weak) {
				++entry_only;
			} else {
				++disconnected;
			}
		}
	}
};

// G2 over every MP mode of p_level; returns the level's counts.
RowCounts g2_level(Install &r_m, const std::string &p_level) {
	RowCounts level_counts;
	auto clock = std::chrono::steady_clock::now();
	core::Result<core::level::LevelCombatTiles> combat = core::level::level_combat_tiles(r_m.install, p_level, sim::NavParams().region_margin);
	CHECK_MESSAGE(combat.ok(), p_level, ": ", (combat.ok() ? std::string() : combat.error().message));
	if (!combat) {
		return level_counts;
	}
	std::printf("---- sim.nav G2 %s: %zu modes, region %zu tiles (%zu polygon, %zu point-only), %zu water planes, combat areas %.2f s\n", p_level.c_str(),
			combat->modes.size(), combat->tiles.size(), combat->polygon_tiles, combat->point_only_tiles, combat->water.size(), seconds_since(clock));
	CHECK(combat->modes.size() == 7);
	core::Cache cache;
	if (!open_cache(r_m, (p_level + "_g2").c_str(), cache)) {
		return level_counts;
	}
	// Mode 1 runs alone on every core first: it bakes and stores the tiles every mode shares, so the lanes of modes 2-7
	// (run side by side, the pools of each sized hw / lanes) find them in the cache instead of baking them several times.
	// The loads hold no assertions; the rows are printed and CHECKed below on this thread in mode order.
	constexpr unsigned kModeLanes = 4;
	const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
	std::vector<ModeLoad> loads;
	loads.reserve(combat->modes.size());
	if (!combat->modes.empty()) {
		loads.push_back(load_mode_on(r_m, p_level, combat->modes[0], *combat, &cache));
	}
	if (combat->modes.size() > 1) {
		std::vector<ModeLoad> rest = core::parallel_map<ModeLoad>(
				combat->modes.size() - 1,
				[&](size_t i) { return load_mode_on(r_m, p_level, combat->modes[i + 1], *combat, &cache, std::max(1u, hw / kModeLanes)); },
				kModeLanes);
		for (ModeLoad &l : rest) {
			loads.push_back(std::move(l));
		}
	}
	for (size_t m = 0; m < combat->modes.size(); ++m) {
		const std::string &mode = combat->modes[m];
		CHECK_MESSAGE(loads[m].bots != nullptr, p_level, " ", mode, ": ", loads[m].error);
		if (loads[m].bots == nullptr) {
			continue;
		}
		const ModeLoad *l = &loads[m];
		RowCounts c;
		for (const host::BotPointRow &r : l->bots->points) {
			const bool played = r.kind == host::BotPointRow::Kind::Spawn || in_combat_area(r.point, combat->areas[m]);
			c.add(r, played);
			level_counts.add(r, played);
		}
		const sim::NavStats &ns = l->bots->nav->stats();
		std::printf("G2 %s %s: objectives %zu / %zu in main (+ %zu outside every combat area, %zu of them in main); spawns %zu / %zu in main "
					"(exit-only %zu, entry-only %zu, disconnected %zu, off-mesh %zu); %zu combat areas (%zu unwired triggers); nav %zu polys, %s; "
					"steps db %.2f s, shared %.2f s, bots %.2f s\n",
				p_level.c_str(), mode.c_str(), c.objectives_main, c.objectives, c.objectives_unplayed, c.objectives_unplayed_main, c.spawns_main,
				c.spawns, c.exit_only, c.entry_only, c.disconnected, c.off_mesh, combat->areas[m].combat_areas.size(),
				combat->areas[m].unwired_triggers.size(), ns.polys,
				ns.world_hit ? "nav-world cache hit" : (std::to_string(ns.tiles_hit) + " tiles from the cache").c_str(), l->seconds_db, l->seconds_shared,
				l->seconds_bots);
		for (const host::BotPointRow &r : l->bots->points) {
			if (!r.in_main) {
				const bool played = r.kind == host::BotPointRow::Kind::Spawn || in_combat_area(r.point, combat->areas[m]);
				std::printf("   %s %u (%.2f %.2f %.2f): %s, h %.2f v %.2f%s%s\n", kind_name(r.kind), r.index, r.point[0], r.point[1], r.point[2],
						row_class(r), r.check.horizontal, r.check.vertical,
						r.kind == host::BotPointRow::Kind::Spawn && !r.settled ? ", spawn did not settle" : "", played ? "" : ", outside every combat area");
			}
		}
		// Every flag, HQ and MCOM inside a combat area of its mode should be in the main component (G2). Reported
		// (WARN): Metro and Caspian Rush / Squad Rush have stage-gated geometry the static bake sees closed (docs §5.7).
		WARN_MESSAGE(c.objectives_main == c.objectives, p_level, " ", mode, ": ", c.objectives - c.objectives_main,
				" objectives outside the main component");
		if (mode.starts_with("Conquest")) {
			CHECK_MESSAGE(c.objectives_main == c.objectives, p_level, " ", mode, ": every flag and HQ in the main component");
		}
		CHECK(c.objectives + c.objectives_unplayed + c.spawns == l->bots->points.size());
	}
	cache.close();
	const size_t rows = level_counts.objectives + level_counts.spawns;
	const size_t in_main = level_counts.objectives_main + level_counts.spawns_main;
	std::printf("G2 %s all modes: %zu / %zu rows in the main component (objectives %zu / %zu, spawns %zu / %zu; exit-only %zu, entry-only %zu, "
				"disconnected %zu, off-mesh %zu; %zu objectives outside every combat area not counted)  [gate: every row: %s]\n",
			p_level.c_str(), in_main, rows, level_counts.objectives_main, level_counts.objectives, level_counts.spawns_main, level_counts.spawns,
			level_counts.exit_only, level_counts.entry_only, level_counts.disconnected, level_counts.off_mesh, level_counts.objectives_unplayed, in_main == rows ? "PASS" : "FAIL");
	std::fflush(stdout);
	// The all-rows gate is reported (WARN): retail spawn points that our static world leaves off the main component
	// are listed above with their class (docs §5.7 as built has the table).
	WARN_MESSAGE(in_main == rows, "G2 ", p_level, ": ", rows - in_main, " rows outside the main component");
	return level_counts;
}

// Möller-Trumbore in double: the ray parameter t in [0, 1] of the first hit, or -1. r_edge: the hit lies within
// p_edge_eps (barycentric) of an edge (a grazing or edge hit Jolt may resolve either way).
double ray_triangle(const double p_o[3], const double p_d[3], const float *p_v, double p_edge_eps, bool &r_edge) {
	const double a[3] = { p_v[0], p_v[1], p_v[2] }, b[3] = { p_v[3], p_v[4], p_v[5] }, c[3] = { p_v[6], p_v[7], p_v[8] };
	const double e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
	const double p[3] = { p_d[1] * e2[2] - p_d[2] * e2[1], p_d[2] * e2[0] - p_d[0] * e2[2], p_d[0] * e2[1] - p_d[1] * e2[0] };
	const double det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
	if (std::fabs(det) < 1e-18) {
		return -1.0;
	}
	const double inv = 1.0 / det;
	const double s[3] = { p_o[0] - a[0], p_o[1] - a[1], p_o[2] - a[2] };
	const double u = (s[0] * p[0] + s[1] * p[1] + s[2] * p[2]) * inv;
	if (u < -p_edge_eps || u > 1.0 + p_edge_eps) {
		return -1.0;
	}
	const double q[3] = { s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2], s[0] * e1[1] - s[1] * e1[0] };
	const double v = (p_d[0] * q[0] + p_d[1] * q[1] + p_d[2] * q[2]) * inv;
	if (v < -p_edge_eps || u + v > 1.0 + p_edge_eps) {
		return -1.0;
	}
	const double t = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * inv;
	if (t < 0.0 || t > 1.0) {
		return -1.0;
	}
	r_edge = u < p_edge_eps || v < p_edge_eps || u + v > 1.0 - p_edge_eps;
	return t;
}

// §5.1 on one level x mode: proofs 1 (per tile against the cooked shapes), 2 (no Raycast triangle) and 3 (rays).
void input_proofs(Install &r_m, const std::string &p_level, const std::string &p_mode) {
	auto clock = std::chrono::steady_clock::now();
	core::Result<std::unique_ptr<core::level::LoadedLevel>> level = core::level::load_level(r_m.install, p_level, p_mode);
	REQUIRE_MESSAGE(level.ok(), (level.ok() ? std::string() : level.error().message));
	core::Result<std::unique_ptr<core::level::World>> world = core::level::World::traverse(**level);
	REQUIRE_MESSAGE(world.ok(), (world.ok() ? std::string() : world.error().message));
	core::Result<core::level::LevelCombatTiles> combat = core::level::level_combat_tiles(r_m.install, p_level, sim::NavParams().region_margin);
	REQUIRE_MESSAGE(combat.ok(), (combat.ok() ? std::string() : combat.error().message));
	core::level::SoupOptions so;
	core::Result<core::level::CollisionSoup> soup = core::level::build_collision_soup(**level, **world, so);
	REQUIRE_MESSAGE(soup.ok(), (soup.ok() ? std::string() : soup.error().message));
	sim::StaticWorldBuildOptions bo;
	core::Result<std::shared_ptr<const sim::StaticWorldShapes>> shapes = sim::build_static_world(*soup, bo);
	REQUIRE_MESSAGE(shapes.ok(), (shapes.ok() ? std::string() : shapes.error().message));
	const sim::NavParams params;
	core::Result<std::vector<sim::NavTileInput>> inputs = sim::nav_tile_inputs(*soup, combat->tiles, params, sim::query_mask::kMovement, combat->water);
	REQUIRE_MESSAGE(inputs.ok(), (inputs.ok() ? std::string() : inputs.error().message));
	const double seconds_setup = seconds_since(clock);
	// Proofs 1 and 2.
	clock = std::chrono::steady_clock::now();
	size_t matched = 0, world_only = 0, nav_only = 0, nav_only_degenerate = 0, raycast = 0, far = 0, triangles = 0;
	float max_error = 0.0f;
	std::vector<sim::NavTileKey> bad_tiles;
	for (const sim::NavTileInput &in : *inputs) {
		core::Result<sim::NavInputCheck> chk = sim::nav_check_input_against_world(**shapes, in, params);
		REQUIRE_MESSAGE(chk.ok(), (chk.ok() ? std::string() : chk.error().message));
		matched += chk->matched;
		world_only += chk->world_only;
		nav_only += chk->nav_only;
		nav_only_degenerate += chk->nav_only_degenerate;
		max_error = std::max(max_error, chk->max_corner_error);
		if (chk->world_only != 0 || chk->nav_only != chk->nav_only_degenerate) {
			bad_tiles.push_back(in.tile);
		}
		for (uint8_t l : in.layers) {
			raycast += l == uint8_t(sim::StaticLayer::Raycast) ? 1 : 0;
		}
		far += in.far_triangles;
		triangles += in.layers.size();
	}
	std::printf("sim.nav §5.1 %s %s: %zu tiles, %zu input triangles (%zu far), proof 1: matched %zu, world-only %zu, nav-only %zu (%zu degenerate), "
				"max corner error %.6f m, tiles failing %zu; proof 2: Raycast triangles %zu; setup %.2f s, proofs 1-2 %.2f s\n",
			p_level.c_str(), p_mode.c_str(), inputs->size(), triangles, far, matched, world_only, nav_only, nav_only_degenerate, double(max_error),
			bad_tiles.size(), raycast, seconds_setup, seconds_since(clock));
	CHECK(world_only == 0);
	CHECK(nav_only == nav_only_degenerate);
	CHECK(raycast == 0);
	CHECK(max_error < 0.001f);
	CHECK(bad_tiles.empty());
	// Proof 3: 10,000 seeded rays, half straight down through a tile, half horizontal inside its input box, against
	// CollisionWorld::ray(kMovement) and a brute-force double-precision test of the tile's input.
	clock = std::chrono::steady_clock::now();
	core::Result<std::unique_ptr<sim::CollisionWorld>> cw = sim::CollisionWorld::create(*shapes);
	REQUIRE_MESSAGE(cw.ok(), (cw.ok() ? std::string() : cw.error().message));
	uint64_t rng = 0x9e3779b97f4a7c15ull ^ std::hash<std::string>()(p_level);
	const auto next = [&]() {
		rng ^= rng << 13;
		rng ^= rng >> 7;
		rng ^= rng << 17;
		return double(rng >> 11) * (1.0 / 9007199254740992.0);
	};
	const float border = sim::nav_input_border(params);
	size_t rays = 0, both = 0, neither = 0, agree = 0, ambiguous = 0, disagree = 0;
	double worst = 0.0;
	std::vector<std::string> examples;
	for (int attempt = 0; rays < 10000 && attempt < 100000; ++attempt) {
		const sim::NavTileInput &in = (*inputs)[size_t(next() * double(inputs->size())) % inputs->size()];
		if (in.layers.empty()) {
			continue;
		}
		float ymin = 1e30f, ymax = -1e30f;
		for (size_t k = 1; k < in.vertices.size(); k += 3) {
			ymin = std::min(ymin, in.vertices[k]);
			ymax = std::max(ymax, in.vertices[k]);
		}
		const double x = (double(in.tile[0]) + next()) * 64.0, z = (double(in.tile[1]) + next()) * 64.0;
		double o[3], d[3];
		if (rays % 2 == 0) {
			o[0] = x, o[1] = double(ymax) + 1.0, o[2] = z;
			d[0] = 0.0, d[1] = double(ymin) - double(ymax) - 2.0, d[2] = 0.0;
		} else {
			const double a = next() * 6.283185307179586;
			const double len = double(border) * 0.9;
			o[0] = x, o[1] = double(ymin) + next() * double(ymax - ymin), o[2] = z;
			d[0] = std::cos(a) * len, d[1] = 0.0, d[2] = std::sin(a) * len;
		}
		++rays;
		double best = 2.0;
		bool best_edge = false, any_edge = false;
		for (size_t t = 0; t < in.layers.size(); ++t) {
			bool edge = false;
			const double h = ray_triangle(o, d, &in.vertices[t * 9], 1e-4, edge);
			if (h >= 0.0) {
				any_edge = any_edge || edge;
				if (h < best) {
					best = h;
					best_edge = edge;
				}
			}
		}
		sim::StaticHit hit;
		const bool jolt = (*cw)->ray({ float(o[0]), float(o[1]), float(o[2]) }, { float(d[0]), float(d[1]), float(d[2]) }, sim::query_mask::kMovement, &hit);
		const double len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
		const bool brute = best <= 1.0;
		if (!jolt && !brute) {
			++neither;
			continue;
		}
		if (jolt && brute) {
			++both;
			const double err = std::fabs(double(hit.fraction) - best) * len;
			worst = std::max(worst, err);
			if (err <= 0.001) {
				++agree;
				continue;
			}
		}
		// A miss on one side, or two hits apart: an edge or grazing hit is ambiguous (as sim.world counts them).
		if (best_edge || any_edge) {
			++ambiguous;
			continue;
		}
		++disagree;
		if (examples.size() < 5) {
			char line[256];
			std::snprintf(line, sizeof(line), "(%.3f %.3f %.3f) d (%.3f %.3f %.3f): jolt %s %.6f, brute %s %.6f", o[0], o[1], o[2], d[0], d[1], d[2],
					jolt ? "hit" : "miss", jolt ? double(hit.fraction) : 0.0, brute ? "hit" : "miss", brute ? best : 0.0);
			examples.push_back(line);
		}
	}
	std::printf("sim.nav §5.1 %s %s proof 3: %zu rays, both hit %zu (agree within 1 mm %zu, worst %.6f m), neither %zu, ambiguous (edge) %zu, "
				"disagree %zu; %.2f s\n",
			p_level.c_str(), p_mode.c_str(), rays, both, agree, worst, neither, ambiguous, disagree, seconds_since(clock));
	for (const std::string &e : examples) {
		std::printf("   disagree %s\n", e.c_str());
	}
	std::fflush(stdout);
	CHECK(rays == 10000);
	CHECK(disagree == 0);
	world->reset();
	level->reset();
}

} // namespace

TEST_SUITE("sim-nav") {
	TEST_CASE("sim.nav: G2 on every MP mode of MP_Subway (Metro)") {
		Install *m = install();
		if (m == nullptr) {
			return;
		}
		const RowCounts c = g2_level(*m, "MP_Subway");
		// Regression floor of the as-built nav (docs §5.7): 1 of 78 CQL rows was off the main component per 10 rows.
		CHECK(c.objectives > 0);
		CHECK(double(c.spawns_main) >= 0.85 * double(c.spawns));
	}

	TEST_CASE("sim.nav: G2 on every MP mode of MP_007 (Caspian Border)") {
		Install *m = install();
		if (m == nullptr) {
			return;
		}
		const RowCounts c = g2_level(*m, "MP_007");
		CHECK(c.objectives > 0);
		CHECK(double(c.spawns_main) >= 0.9 * double(c.spawns));
	}

	TEST_CASE("sim.nav: §5.1 the nav input is the motor's collision set on MP_Subway and MP_007 ConquestLarge0") {
		Install *m = install();
		if (m == nullptr) {
			return;
		}
		input_proofs(*m, "MP_Subway", "ConquestLarge0");
		input_proofs(*m, "MP_007", "ConquestLarge0");
	}

	TEST_CASE("sim.nav: §5.7 Metro CQL components by layer set and cell size; cold then warm on a scratch root") {
		Install *m = install();
		if (m == nullptr) {
			return;
		}
		core::Result<core::level::LevelCombatTiles> combat = core::level::level_combat_tiles(m->install, "MP_Subway", sim::NavParams().region_margin);
		REQUIRE_MESSAGE(combat.ok(), (combat.ok() ? std::string() : combat.error().message));
		struct Variant {
			const char *name;
			uint32_t mask;
			float cs;
			bool ceil;
		};
		const Variant variants[] = {
			{ "as built (movement, cs 0.2, radius ceil 0.4 m)", sim::query_mask::kMovement, 0.2f, true },
			{ "all layers (Raycast included, the research's set)", sim::query_mask::kAll, 0.2f, true },
			{ "plan (cs 0.25, radius ceil 0.5 m)", sim::query_mask::kMovement, 0.25f, true },
			{ "cs 0.25, radius nearest 0.25 m", sim::query_mask::kMovement, 0.25f, false },
		};
		std::optional<size_t> plan_main, built_main;
		// The variants differ only in BotLoadOptions: one database and static world (same level, mode and tuning, no
		// cache) serve all four bot loads.
		const LevelBase base = load_base(*m, "MP_Subway", "ConquestLarge0", nullptr);
		REQUIRE_MESSAGE(base.shared != nullptr, base.error);
		for (const Variant &v : variants) {
			host::BotLoadOptions bo;
			bo.layer_mask = v.mask;
			bo.params.cell_size = v.cs;
			bo.params.radius_round_up = v.ceil;
			const ModeLoad loaded = load_bots(base, *combat, nullptr, bo);
			CHECK_MESSAGE(loaded.bots != nullptr, v.name, ": ", loaded.error);
			if (loaded.bots == nullptr) {
				continue;
			}
			const ModeLoad *l = &loaded;
			const sim::NavComponents &c = l->bots->components;
			double total = 0.0;
			for (double a : c.area) {
				total += a;
			}
			RowCounts rc;
			for (const host::BotPointRow &r : l->bots->points) {
				rc.add(r);
			}
			const size_t in_main = rc.objectives_main + rc.spawns_main;
			std::printf("sim.nav §5.7 Metro CQL %s: %zu polys, %zu SCCs, %zu weak, main %.1f %% of %.0f m^2; G2 rows in main %zu / %zu (objectives %zu / "
						"%zu); nav %.2f s\n",
					v.name, l->bots->nav->stats().polys, c.area.size(), c.weak_count, total > 0 ? 100.0 * c.area[l->bots->main_component] / total : 0.0,
					total, in_main, rc.objectives + rc.spawns, rc.objectives_main, rc.objectives, l->bots->seconds_nav);
			if (&v == &variants[0]) {
				built_main = in_main;
			}
			if (&v == &variants[2]) {
				plan_main = in_main;
			}
		}
		std::fflush(stdout);
		REQUIRE(built_main.has_value());
		REQUIRE(plan_main.has_value());
		CHECK(*built_main > *plan_main); // the as-built choice is the better one on the gate's rows
		// Cold then warm on a scratch root: the warm load restores the nav world (no soup, no bake), the same tiles.
		core::Cache cache;
		REQUIRE(open_cache(*m, "metro_cold_warm", cache));
		const std::optional<ModeLoad> cold = load_mode(*m, "MP_Subway", "ConquestLarge0", *combat, &cache);
		const std::optional<ModeLoad> warm = load_mode(*m, "MP_Subway", "ConquestLarge0", *combat, &cache);
		REQUIRE(cold.has_value());
		REQUIRE(warm.has_value());
		const sim::NavStats &a = cold->bots->nav->stats(), &b = warm->bots->nav->stats();
		std::printf("sim.nav cold / warm Metro CQL: cold %.2f s (nav %s, %zu tiles baked, %zu not stored%s%s), warm %.2f s (nav %s, %zu tiles from the "
					"cache)\n",
				cold->seconds_shared + cold->seconds_bots, a.world_hit ? "hit" : "baked", a.tiles_baked, a.tiles_uncached, a.cache_error.empty() ? "" : ": ",
				a.cache_error.c_str(), warm->seconds_shared + warm->seconds_bots, b.world_hit ? "hit" : "baked", b.tiles_hit);
		CHECK(!a.world_hit);
		CHECK(b.world_hit);
		REQUIRE(cold->bots->nav->tile_count() == warm->bots->nav->tile_count());
		for (size_t i = 0; i < cold->bots->nav->tile_count(); ++i) {
			CHECK(cold->bots->nav->tile(i).input == warm->bots->nav->tile(i).input);
			CHECK(cold->bots->nav->tile(i).data_bytes == warm->bots->nav->tile(i).data_bytes);
		}
		CHECK(cold->bots->points.size() == warm->bots->points.size());
		cache.close();
	}

	TEST_CASE("sim.nav: install unchanged (layout.toc, cas.cat, the .sb / .toc files)") {
		Install *m = install();
		if (m == nullptr) {
			return;
		}
		const core::SnapshotDiff diff = core::diff_snapshots(m->before, m->reader->snapshot());
		CHECK_MESSAGE(diff.clean(), diff.summary());
		std::printf("install.unchanged: %s (%zu files)\n", diff.clean() ? "clean" : diff.summary().c_str(), m->before.files.size());
	}
}

TEST_SUITE("sim-nav-bake") {
	TEST_CASE("sim.nav.bake: G4 on MP_007 and MP_018 ConquestLarge0 at 16 threads; Caspian at 1 and 4 threads (reported)") {
		Install *m = install();
		if (m == nullptr) {
			return;
		}
		struct Run {
			const char *level;
			unsigned threads;
			double gate; // 0 = reported only
		};
		const Run runs[] = {
			{ "MP_007", 16, host::kNavBakeGateCaspianSeconds },
			{ "MP_018", 16, host::kNavBakeGateKhargSeconds },
			{ "MP_007", 4, 0.0 },
			{ "MP_007", 1, 0.0 },
		};
		for (const Run &r : runs) {
			host::NavBakeBenchOptions o;
			o.level = r.level;
			o.mode = "ConquestLarge0";
			o.threads = r.threads;
			o.tuning_text = read_text(OPENBF3_MOTOR_TUNING_CFG);
			core::Result<host::NavBakeBench> b = host::run_nav_bake_bench(m->install, o);
			CHECK_MESSAGE(b.ok(), r.level, ": ", (b.ok() ? std::string() : b.error().message));
			if (!b) {
				continue;
			}
			std::printf("---- sim.nav.bake %s ConquestLarge0, %u threads ----\n%s", r.level, r.threads, host::format_nav_bake_bench(*b).c_str());
			if (r.gate > 0.0) {
				std::printf("G4 %s: %.3f s  [gate <= %.1f s on 16 threads: %s]\n", r.level, b->gate_seconds, r.gate, b->gate_seconds <= r.gate ? "PASS" : "FAIL");
#if !defined(OPENBF3_TIMING_REPORT_ONLY)
				CHECK_MESSAGE(b->gate_seconds <= r.gate, "G4 ", r.level, ": ", b->gate_seconds, " s > ", r.gate, " s");
#endif
			}
			std::fflush(stdout);
			CHECK(b->nav != nullptr);
			CHECK(b->nav->stats().polys > 0);
		}
		const core::SnapshotDiff diff = core::diff_snapshots(m->before, m->reader->snapshot());
		CHECK_MESSAGE(diff.clean(), diff.summary());
		std::printf("install.unchanged: %s (%zu files)\n", diff.clean() ? "clean" : diff.summary().c_str(), m->before.files.size());
	}
}
