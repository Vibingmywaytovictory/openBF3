#include "nav_commands.h"

#include "prepare_commands.h"

#include "bf3/core/cache/disk_cache.h"
#include "bf3/core/install_snapshot.h"
#include "bf3/core/level/collision_soup.h"
#include "bf3/core/level/level.h"
#include "bf3/core/level/mode_areas.h"
#include "bf3/core/level/world.h"
#include "bf3/host/bot_fill.h"
#include "bf3/host/level_load.h"
#include "bf3/host/scale_bench.h"
#include "bf3/sim/nav.h"
#include "bf3/sim/nav_query.h"
#include "bf3/sim/soldier_motor.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <unordered_map>

using namespace bf3;

namespace bf3dump {

namespace {

double seconds_since(std::chrono::steady_clock::time_point p_start) {
	return std::chrono::duration<double>(std::chrono::steady_clock::now() - p_start).count();
}

std::filesystem::path utf8_path(std::string_view p_text) {
	return std::filesystem::path(std::u8string(p_text.begin(), p_text.end()));
}

bool parse_uint(std::string_view p_text, unsigned &r_value) {
	const auto res = std::from_chars(p_text.data(), p_text.data() + p_text.size(), r_value);
	return res.ec == std::errc() && res.ptr == p_text.data() + p_text.size();
}

bool parse_float(const std::string &p_text, float &r_value) {
	char *end = nullptr;
	r_value = std::strtof(p_text.c_str(), &end);
	return end != nullptr && *end == 0 && std::isfinite(r_value);
}

struct Args {
	std::vector<std::string> positional;
	unsigned threads = 0;
	uint32_t layers = sim::query_mask::kMovement;
	bool radius_round_up = true;
	float cell_size = 0.0f;
	int partition = -1;
	bool links = true;
	bool tiles = false;
	bool points = false;
	bool seams = false;
	unsigned pairs = 1000;
	unsigned seed = 1;
	std::filesystem::path cache;
	std::filesystem::path dump;
	bool bad = false;
};

Args parse(const std::vector<std::string> &p_args, size_t p_from) {
	Args a;
	for (size_t i = p_from; i < p_args.size(); ++i) {
		const std::string &s = p_args[i];
		if (s.rfind("--threads=", 0) == 0) {
			a.bad = a.bad || !parse_uint(std::string_view(s).substr(10), a.threads) || a.threads == 0 || a.threads > 256;
		} else if (s == "--layers=movement") {
			a.layers = sim::query_mask::kMovement;
		} else if (s == "--layers=all") {
			a.layers = sim::query_mask::kAll;
		} else if (s == "--radius=ceil") {
			a.radius_round_up = true;
		} else if (s == "--radius=nearest") {
			a.radius_round_up = false;
		} else if (s == "--partition=watershed" || s == "--partition=monotone" || s == "--partition=layers" || s == "--partition=auto") {
			a.partition = s == "--partition=watershed" ? 0 : s == "--partition=monotone" ? 1 : s == "--partition=layers" ? 2 : 3;
		} else if (s.rfind("--cs=", 0) == 0) {
			a.bad = a.bad || !parse_float(s.substr(5), a.cell_size) || a.cell_size < 0.1f || a.cell_size > 1.0f;
		} else if (s == "--no-links") {
			a.links = false;
		} else if (s == "--no-cache") {
			a.cache.clear();
		} else if (s == "--tiles") {
			a.tiles = true;
		} else if (s == "--points") {
			a.points = true;
		} else if (s == "--seams") {
			a.seams = true;
		} else if (s.rfind("--pairs=", 0) == 0) {
			a.bad = a.bad || !parse_uint(std::string_view(s).substr(8), a.pairs) || a.pairs == 0 || a.pairs > 1000000;
		} else if (s.rfind("--seed=", 0) == 0) {
			a.bad = a.bad || !parse_uint(std::string_view(s).substr(7), a.seed);
		} else if (s.rfind("--cache=", 0) == 0) {
			a.cache = utf8_path(std::string_view(s).substr(8));
		} else if (s.rfind("--dump=", 0) == 0) {
			a.dump = utf8_path(std::string_view(s).substr(7));
		} else if (s.rfind("--", 0) == 0 && !(s.size() > 2 && (std::isdigit(uint8_t(s[1])) || s[1] == '.'))) {
			std::fprintf(stderr, "nav: unknown option %s\n", s.c_str());
			a.bad = true;
		} else {
			a.positional.push_back(s);
		}
	}
	return a;
}

int finish(const core::InstallReader &p_reader, const core::InstallSnapshot &p_before, int p_rc) {
	const core::SnapshotDiff diff = core::diff_snapshots(p_before, p_reader.snapshot());
	std::printf("install.unchanged %s\n",
			p_before.valid && diff.clean() ? ("yes (" + std::to_string(p_before.files.size()) + " files: sizes and mtimes equal)").c_str() : ("NO: " + diff.summary()).c_str());
	return p_before.valid && diff.clean() ? p_rc : 1;
}

struct Loaded {
	std::optional<host::LevelDatabase> db;
	core::level::LevelCombatTiles combat;
	std::shared_ptr<const host::SharedLevel> shared;
	std::shared_ptr<const host::BotLevel> bots;
	double seconds_db = 0, seconds_areas = 0, seconds_shared = 0, seconds_bots = 0;
};

core::Result<Loaded> load(const core::InstallReader &p_reader, const std::shared_ptr<core::Install> &p_install, const std::string &p_level,
		const std::string &p_mode, const Args &p_args, core::Cache &r_cache) {
	Loaded out;
	core::Cache *cache = nullptr;
	if (!p_args.cache.empty()) {
		core::CacheOptions co;
		std::error_code ec;
		co.root = std::filesystem::absolute(p_args.cache, ec);
		co.guard = p_reader.write_guard();
		std::string detail;
		if (ec || r_cache.open(std::move(co), &detail) != core::CacheStatus::Ok) {
			return core::make_error("cache: " + detail);
		}
		cache = &r_cache;
	}
	host::LoadProgress progress;
	auto clock = std::chrono::steady_clock::now();
	core::Result<host::LevelDatabase> db = host::open_level_database(p_install, p_level, p_mode, p_args.threads, &progress);
	if (!db) {
		return db.error();
	}
	out.db.emplace(std::move(*db));
	out.seconds_db = seconds_since(clock);
	clock = std::chrono::steady_clock::now();
	const sim::NavParams defaults;
	core::Result<core::level::LevelCombatTiles> combat = core::level::level_combat_tiles(p_install, p_level, defaults.region_margin);
	if (!combat) {
		return combat.error();
	}
	out.combat = std::move(*combat);
	out.seconds_areas = seconds_since(clock);
	clock = std::chrono::steady_clock::now();
	host::SharedLoadOptions so;
	so.cache = cache;
	so.threads = p_args.threads;
	core::Result<std::shared_ptr<const host::SharedLevel>> shared = host::load_shared_level(*out.db, so, &progress);
	if (!shared) {
		return shared.error();
	}
	out.shared = *shared;
	out.seconds_shared = seconds_since(clock);
	clock = std::chrono::steady_clock::now();
	host::BotLoadOptions bo;
	bo.cache = cache;
	bo.threads = p_args.threads;
	bo.combat = &out.combat;
	bo.layer_mask = p_args.layers;
	bo.params.radius_round_up = p_args.radius_round_up;
	if (p_args.cell_size > 0.0f) {
		bo.params.cell_size = p_args.cell_size;
	}
	if (p_args.partition >= 0) {
		bo.params.partition = sim::NavPartition(p_args.partition);
	}
	core::Result<std::shared_ptr<const host::BotLevel>> bots = host::load_bot_level(*out.db, *out.shared, bo, &progress);
	if (!bots) {
		return bots.error();
	}
	out.bots = *bots;
	out.seconds_bots = seconds_since(clock);
	return out;
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

void print_components(const host::BotLevel &p_bots) {
	const sim::NavComponents &c = p_bots.components;
	double total = 0.0;
	for (double a : c.area) {
		total += a;
	}
	std::vector<uint32_t> order;
	for (uint32_t i = 0; i < c.area.size(); ++i) {
		if (c.area[i] > 0.0) {
			order.push_back(i);
		}
	}
	std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return c.area[a] > c.area[b]; });

	const sim::NavDebugMesh dm = sim::nav_debug_mesh(*p_bots.nav, &c);
	std::unordered_map<uint32_t, std::pair<sim::Vec3, sim::Vec3>> bounds;
	for (size_t t = 0; t < dm.triangle_component.size(); ++t) {
		auto it = bounds.find(dm.triangle_component[t]);
		for (int k = 0; k < 3; ++k) {
			const sim::Vec3 &v = dm.triangles[t * 3 + size_t(k)];
			if (it == bounds.end()) {
				it = bounds.emplace(dm.triangle_component[t], std::make_pair(v, v)).first;
			}
			for (int a = 0; a < 3; ++a) {
				it->second.first[a] = std::min(it->second.first[a], v[a]);
				it->second.second[a] = std::max(it->second.second[a], v[a]);
			}
		}
	}
	std::printf("components: %zu strongly connected with area (%zu in all incl. drop-link polys), %zu weak; walkable area %.0f m^2; main %u holds %.1f %%\n",
			order.size(), c.polys.size(), c.weak_count, total, p_bots.main_component, total > 0 ? 100.0 * c.area[p_bots.main_component] / total : 0.0);
	for (size_t i = 0; i < order.size() && i < 12; ++i) {
		const uint32_t id = order[i];
		const auto b = bounds.count(id) ? bounds[id] : std::make_pair(sim::Vec3{}, sim::Vec3{});
		std::printf("C %u\tpolys %u\tarea %.0f m^2 (%.1f %%)\tbounds (%.0f %.0f %.0f)..(%.0f %.0f %.0f)\n", id, c.polys[id], c.area[id], 100.0 * c.area[id] / total,
				b.first[0], b.first[1], b.first[2], b.second[0], b.second[1], b.second[2]);
	}
}

void print_seams(const host::BotLevel &p_bots) {
	const sim::NavDebugMesh dm = sim::nav_debug_mesh(*p_bots.nav, &p_bots.components);
	const sim::NavComponents &c = p_bots.components;
	const uint32_t main = p_bots.main_component;
	const auto cell = [](float v) { return int64_t(std::floor(v)); };
	const auto key = [](int64_t x, int64_t z) { return uint64_t(x & 0xffffffff) << 32 | uint64_t(z & 0xffffffff); };
	std::unordered_map<uint64_t, std::vector<sim::Vec3>> main_cells;
	for (size_t t = 0; t < dm.triangle_component.size(); ++t) {
		if (dm.triangle_component[t] == main) {
			for (int k = 0; k < 3; ++k) {
				const sim::Vec3 &v = dm.triangles[t * 3 + size_t(k)];
				main_cells[key(cell(v[0]), cell(v[2]))].push_back(v);
			}
		}
	}
	struct Seam {
		uint32_t other = 0;
		sim::Vec3 at{};
		size_t hits = 0;
	};
	std::map<std::tuple<uint32_t, int64_t, int64_t, int64_t>, Seam> seams;
	for (size_t t = 0; t < dm.triangle_component.size(); ++t) {
		const uint32_t comp = dm.triangle_component[t];
		if (comp == main || comp >= c.area.size() || c.area[comp] < 25.0) {
			continue;
		}
		for (int k = 0; k < 3; ++k) {
			const sim::Vec3 &v = dm.triangles[t * 3 + size_t(k)];
			bool near = false;
			for (int64_t dx = -1; dx <= 1 && !near; ++dx) {
				for (int64_t dz = -1; dz <= 1 && !near; ++dz) {
					const auto it = main_cells.find(key(cell(v[0]) + dx, cell(v[2]) + dz));
					if (it == main_cells.end()) {
						continue;
					}
					for (const sim::Vec3 &m : it->second) {
						const float h = std::hypot(m[0] - v[0], m[2] - v[2]);
						if (h <= 1.0f && std::fabs(m[1] - v[1]) <= 0.5f) {
							near = true;
							break;
						}
					}
				}
			}
			if (near) {
				Seam &s = seams[{ comp, int64_t(std::floor(v[0] / 4.0f)), int64_t(std::floor(v[1] / 4.0f)), int64_t(std::floor(v[2] / 4.0f)) }];
				s.other = comp;
				s.at = v;
				++s.hits;
			}
		}
	}
	std::printf("seams between main component %u and the others (corners within 1 m / 0.5 m, 4 m clusters): %zu\n", main, seams.size());
	size_t shown = 0;
	for (const auto &[k, s] : seams) {
		if (shown++ >= 200) {
			break;
		}
		std::printf("S %u\tarea %.0f m^2\tat (%.2f %.2f %.2f)\tcorners %zu\n", s.other, c.area[s.other], s.at[0], s.at[1], s.at[2], s.hits);
	}
}

void print_points(const host::BotLevel &p_bots) {
	size_t on = 0, main = 0, one_way = 0, disconnected = 0, exit_only = 0;
	for (const host::BotPointRow &r : p_bots.points) {
		const char *cls = r.in_main ? "main" : !r.on_mesh ? "off-mesh" : r.reaches_main ? "exit-only" : r.in_main_weak ? "entry-only" : "disconnected";
		exit_only += (!r.in_main && r.reaches_main) ? 1 : 0;
		on += r.on_mesh ? 1 : 0;
		main += r.in_main ? 1 : 0;
		one_way += (r.on_mesh && !r.in_main && r.in_main_weak) ? 1 : 0;
		disconnected += (r.on_mesh && !r.in_main_weak) ? 1 : 0;
		std::printf("P %s\t%u\t(%.2f %.2f %.2f)\th %.2f\tv %.2f\tscc %d\tweak %d\t%s%s\n", kind_name(r.kind), r.index, r.point[0], r.point[1], r.point[2],
				r.check.horizontal, r.check.vertical, r.check.component == UINT32_MAX ? -1 : int(r.check.component),
				r.check.weak_component == UINT32_MAX ? -1 : int(r.check.weak_component), cls, r.kind == host::BotPointRow::Kind::Spawn && !r.settled ? " (unsettled)" : "");
	}
	std::printf("G2 rows: %zu, on the mesh %zu, in the main component %zu (%.1f %%), one-way only %zu (exit-only %zu: they reach the main component), disconnected %zu, off the mesh %zu\n",
			p_bots.points.size(), on, main, p_bots.points.empty() ? 0.0 : 100.0 * double(main) / double(p_bots.points.size()), one_way, exit_only, disconnected,
			p_bots.points.size() - on);
	const host::BotSpawns &s = p_bots.spawns;
	std::printf("bot spawns: level %u, in main %u, excluded %u, teams merged %s, synthetic %u\n", s.level_points, s.in_main, s.excluded,
			s.teams_merged ? "yes" : "no", s.synthetic);
}

int cmd_nav_components(const core::InstallReader &p_reader, const std::shared_ptr<core::Install> &p_install, const Args &p_args) {
	const core::InstallSnapshot before = p_reader.snapshot();
	core::Cache cache;
	const auto start = std::chrono::steady_clock::now();
	core::Result<Loaded> l = load(p_reader, p_install, p_args.positional[0], p_args.positional[1], p_args, cache);
	if (!l) {
		std::fprintf(stderr, "nav components: %s\n", l.error().message.c_str());
		return 1;
	}
	const sim::NavStats &ns = l->bots->nav->stats();
	std::printf("nav %s %s: layers %s, radius %s; region %zu tiles (%zu polygon, %zu point-only, %zu of %zu points outside the areas); %zu tiles with polys, %zu "
				"polys, %zu drop links; db %.2f s, areas %.2f s, shared %.2f s, bots %.2f s (nav %.2f s, %s)\n",
			p_args.positional[0].c_str(), p_args.positional[1].c_str(), p_args.layers == sim::query_mask::kMovement ? "movement" : "all",
			p_args.radius_round_up ? "ceil" : "nearest", l->combat.tiles.size(), l->combat.polygon_tiles, l->combat.point_only_tiles,
			l->combat.points_outside_areas, l->combat.points, ns.tiles, ns.polys, ns.drop_links, l->seconds_db, l->seconds_areas, l->seconds_shared,
			l->seconds_bots, l->bots->seconds_nav, ns.world_hit ? "cache hit" : "baked");
	for (const core::level::WaterPlane &w : l->combat.water) {
		float x0 = 1e30f, x1 = -1e30f, z0 = 1e30f, z1 = -1e30f;
		for (const auto &p : w.polygon) {
			x0 = std::min(x0, p[0]);
			x1 = std::max(x1, p[0]);
			z0 = std::min(z0, p[1]);
			z1 = std::max(z1, p[1]);
		}
		std::printf("water: plane y %.2f, %zu points, x %.0f..%.0f z %.0f..%.0f\n", w.y, w.polygon.size(), x0, x1, z0, z1);
	}
	for (size_t m = 0; m < l->combat.modes.size(); ++m) {
		std::printf("mode %s: %zu combat areas (%zu unwired triggers), %zu objectives\n", l->combat.modes[m].c_str(), l->combat.areas[m].combat_areas.size(),
				l->combat.areas[m].unwired_triggers.size(), l->combat.areas[m].objectives.size());
	}
	print_components(*l->bots);
	if (p_args.points) {
		print_points(*l->bots);
	}
	if (p_args.seams) {
		print_seams(*l->bots);
	}
	if (!p_args.dump.empty()) {

		std::error_code ec;
		const std::filesystem::path target = std::filesystem::absolute(p_args.dump, ec);
		if (ec || p_reader.write_guard().check_write_target(target) != core::PathGuard::Verdict::Allowed) {
			std::fprintf(stderr, "nav components: refusing to write %s\n", p_args.dump.string().c_str());
			return 1;
		}
		const sim::NavDebugMesh dm = sim::nav_debug_mesh(*l->bots->nav, &l->bots->components);
		std::FILE *f = _wfopen(target.c_str(), L"wb");
		if (f == nullptr) {
			std::fprintf(stderr, "nav components: cannot write %s\n", p_args.dump.string().c_str());
			return 1;
		}
		for (size_t t = 0; t < dm.triangle_component.size(); ++t) {
			const sim::Vec3 *v = &dm.triangles[t * 3];
			std::fprintf(f, "T %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f %u %u\n", v[0][0], v[0][1], v[0][2], v[1][0], v[1][1], v[1][2], v[2][0], v[2][1],
					v[2][2], dm.triangle_component[t], unsigned(dm.triangle_area[t]));
		}
		for (size_t k = 0; k + 1 < dm.links.size(); k += 2) {
			std::fprintf(f, "L %.2f %.2f %.2f %.2f %.2f %.2f\n", dm.links[k][0], dm.links[k][1], dm.links[k][2], dm.links[k + 1][0], dm.links[k + 1][1],
					dm.links[k + 1][2]);
		}
		std::fclose(f);
		std::printf("debug mesh: %zu triangles, %zu links -> %s\n", dm.triangle_component.size(), dm.links.size() / 2, p_args.dump.string().c_str());
	}
	cache.close();
	std::printf("total %.2f s\n", seconds_since(start));
	return finish(p_reader, before, 0);
}

int cmd_nav_path(const core::InstallReader &p_reader, const std::shared_ptr<core::Install> &p_install, const Args &p_args) {
	const core::InstallSnapshot before = p_reader.snapshot();
	core::Cache cache;
	core::Result<Loaded> l = load(p_reader, p_install, p_args.positional[0], p_args.positional[1], p_args, cache);
	if (!l) {
		std::fprintf(stderr, "nav path: %s\n", l.error().message.c_str());
		return 1;
	}
	sim::NavQueryOptions qo;
	qo.max_nodes = 65535;
	core::Result<std::unique_ptr<sim::NavQuery>> q = sim::NavQuery::create(l->bots->nav, qo);
	if (!q) {
		std::fprintf(stderr, "nav path: %s\n", q.error().message.c_str());
		return 1;
	}
	uint64_t rng = p_args.seed;
	std::vector<double> ms, lengths, polys;
	size_t done = 0, partial = 0, failed = 0;
	std::vector<sim::NavPolyRef> path;
	for (unsigned i = 0; i < p_args.pairs; ++i) {
		sim::NavPolyRef a = 0, b = 0;
		sim::Vec3 pa{}, pb{};
		if (!(*q)->random_point(rng, l->bots->components, l->bots->main_component, &a, &pa) ||
				!(*q)->random_point(rng, l->bots->components, l->bots->main_component, &b, &pb)) {
			break;
		}
		const auto t0 = std::chrono::steady_clock::now();
		sim::NavPathStatus st = (*q)->begin_path(a, pa, b, pb);
		while (st == sim::NavPathStatus::InProgress) {
			st = (*q)->continue_path(1u << 20, nullptr);
		}
		st = (*q)->finish_path(path);
		ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
		done += st == sim::NavPathStatus::Done ? 1 : 0;
		partial += st == sim::NavPathStatus::Partial ? 1 : 0;
		failed += st == sim::NavPathStatus::Failed ? 1 : 0;
		polys.push_back(double(path.size()));
		sim::NavCorridor corridor;
		corridor.reset(path, pa, pb);
		lengths.push_back(std::hypot(pb[0] - pa[0], pb[2] - pa[2]));
	}
	const host::Percentiles p = host::percentiles_of(ms);
	const host::Percentiles pl = host::percentiles_of(polys);
	const host::Percentiles d = host::percentiles_of(lengths);
	std::printf("nav path %s %s: %zu pairs in the main component, done %zu partial %zu failed %zu; ms mean %.3f p50 %.3f p95 %.3f max %.3f; path polys mean "
				"%.0f max %.0f; straight distance mean %.0f m max %.0f m\n",
			p_args.positional[0].c_str(), p_args.positional[1].c_str(), ms.size(), done, partial, failed, p.mean, p.p50, p.p95, p.max, pl.mean, pl.max, d.mean,
			d.max);
	cache.close();
	return finish(p_reader, before, 0);
}

int cmd_nav_probe(const core::InstallReader &p_reader, const std::shared_ptr<core::Install> &p_install, const Args &p_args) {
	if (p_args.positional.size() != 6) {
		std::fprintf(stderr, "usage: bf3dump nav probe <LEVEL> <MODE> <x> <y> <z> <radius>\n");
		return 2;
	}
	float x = 0, y = 0, z = 0, r = 0;
	if (!parse_float(p_args.positional[2], x) || !parse_float(p_args.positional[3], y) || !parse_float(p_args.positional[4], z) ||
			!parse_float(p_args.positional[5], r) || r <= 0.0f || r > 64.0f) {
		std::fprintf(stderr, "nav probe: bad point or radius (0 < radius <= 64)\n");
		return 2;
	}
	const core::InstallSnapshot before = p_reader.snapshot();
	core::Result<std::unique_ptr<core::level::LoadedLevel>> level = core::level::load_level(p_install, p_args.positional[0], p_args.positional[1]);
	if (!level) {
		std::fprintf(stderr, "nav probe: %s\n", level.error().message.c_str());
		return 1;
	}
	core::Result<std::unique_ptr<core::level::World>> world = core::level::World::traverse(**level);
	if (!world) {
		std::fprintf(stderr, "nav probe: %s\n", world.error().message.c_str());
		return 1;
	}

	core::level::SoupOptions so;
	core::Result<core::level::CollisionSoup> soup = core::level::build_collision_soup(**level, **world, so);
	if (!soup) {
		std::fprintf(stderr, "nav probe: %s\n", soup.error().message.c_str());
		return 1;
	}

	const auto near = [&](const core::level::SoupTriangle &t) {
		const auto d2 = [&](float a, float b, float c) { return (a - x) * (a - x) + (b - y) * (b - y) + (c - z) * (c - z); };
		float best = 1e30f;
		for (int i = 0; i < 3; ++i) {
			const auto &p = t.v[size_t(i)];
			const auto &q = t.v[size_t((i + 1) % 3)];
			for (int s = 0; s <= 8; ++s) {
				const float f = float(s) / 8.0f;
				best = std::min(best, d2(p[0] + (q[0] - p[0]) * f, p[1] + (q[1] - p[1]) * f, p[2] + (q[2] - p[2]) * f));
			}
		}
		const float cx = (t.v[0][0] + t.v[1][0] + t.v[2][0]) / 3.0f, cy = (t.v[0][1] + t.v[1][1] + t.v[2][1]) / 3.0f, cz = (t.v[0][2] + t.v[1][2] + t.v[2][2]) / 3.0f;
		best = std::min(best, d2(cx, cy, cz));
		return best <= r * r;
	};
	struct Hit {
		size_t triangles = 0;
		std::map<uint8_t, size_t> bodies;
		std::map<uint8_t, size_t> surfaces;
		uint8_t flags = 0;
		float ymin = 1e30f, ymax = -1e30f;
	};
	std::map<uint32_t, Hit> hits;

	std::FILE *dump = nullptr;
	if (!p_args.dump.empty()) {
		std::error_code ec;
		const std::filesystem::path target = std::filesystem::absolute(p_args.dump, ec);
		if (ec || p_reader.write_guard().check_write_target(target) != core::PathGuard::Verdict::Allowed ||
				(dump = _wfopen(target.c_str(), L"wb")) == nullptr) {
			std::fprintf(stderr, "nav probe: cannot write %s\n", p_args.dump.string().c_str());
			return 1;
		}
	}
	for (const core::level::SoupTile &tile : soup->tiles) {
		if (tile.max[0] < x - r || tile.min[0] > x + r || tile.max[2] < z - r || tile.min[2] > z + r) {
			continue;
		}
		for (const core::level::SoupTriangle &t : tile.triangles) {
			if (!near(t)) {
				continue;
			}
			if (dump != nullptr) {
				std::fprintf(dump, "S %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %u %u %u %u\n", t.v[0][0], t.v[0][1], t.v[0][2], t.v[1][0], t.v[1][1],
						t.v[1][2], t.v[2][0], t.v[2][1], t.v[2][2], t.placement, unsigned(t.body), unsigned(t.surface), unsigned(t.flags));
			}
			Hit &h = hits[t.placement];
			++h.triangles;
			++h.bodies[t.body];
			++h.surfaces[t.surface];
			h.flags |= t.flags;
			for (const auto &v : t.v) {
				h.ymin = std::min(h.ymin, v[1]);
				h.ymax = std::max(h.ymax, v[1]);
			}
		}
	}
	std::printf("nav probe %s %s (%.2f %.2f %.2f) r %.2f: %zu placements\n", p_args.positional[0].c_str(), p_args.positional[1].c_str(), x, y, z, r, hits.size());
	for (const auto &[pi, h] : hits) {
		std::string what;
		if (pi < soup->placements.size()) {
			const core::level::SoupPlacement &p = soup->placements[pi];
			if (p.kind == core::level::CollisionPlacement::Kind::Object && p.index < (*world)->objects.size()) {
				const core::level::PlacedObject &o = (*world)->objects[p.index];
				what = "object " + std::to_string(p.index) + " " + std::string(o.entity.type_name()) + " res " + p.res;
				if (const std::optional<std::string_view> label = (*world)->scopes.db().name_of(o.entity.partition().guid())) {
					what += " in " + std::string(*label);
				}
			} else {
				what = "group " + std::to_string(p.index) + " res " + p.res;
			}
		} else {
			what = "terrain " + std::to_string(pi - soup->placements.size());
		}
		std::string bodies;
		for (const auto &[b, n] : h.bodies) {
			bodies += std::string(core::level::collision_body_name(b)) + "=" + std::to_string(n) + " ";
		}
		std::string surfaces;
		for (const auto &[sf, n] : h.surfaces) {
			surfaces += std::to_string(sf) + "=" + std::to_string(n) + " ";
		}
		std::printf("O %s\ttriangles %zu\tbodies %s\tsurfaces %s\tflags %u\ty %.2f..%.2f\n", what.c_str(), h.triangles, bodies.c_str(), surfaces.c_str(),
				unsigned(h.flags), h.ymin, h.ymax);
	}
	if (dump != nullptr) {
		std::fclose(dump);
	}
	world->reset();
	return finish(p_reader, before, 0);
}

}

const char *nav_usage() {
	return "  nav bake|stats <LEVEL> <MODE> [--threads=N] [--layers=movement|all] [--radius=ceil|nearest] [--cs=M] [--no-links] [--tiles]\n"
		   "  nav components <LEVEL> <MODE> [--points] [--seams] [--layers=movement|all] [--radius=ceil|nearest] [--cs=M] [--threads=N] "
		   "[--cache=<dir>] [--dump=<file>]\n"
		   "  nav path <LEVEL> <MODE> [--pairs=N] [--seed=N] [--threads=N] [--cache=<dir>]\n"
		   "  nav probe <LEVEL> <MODE> <x> <y> <z> <radius> [--dump=<file>]\n"
		   "  bots <LEVEL> <MODE> [--walkers=N] [--observers=N] [--seconds=S] [--warmup=S] [--paced=S] [--seed=N] [--threads=N] [--tuning=<cfg>] "
		   "[--cache=<dir>]\n";
}

int cmd_nav(const core::InstallReader &p_reader, const std::shared_ptr<core::Install> &p_install, const std::vector<std::string> &p_args) {
	if (p_args.empty()) {
		std::fprintf(stderr, "%s", nav_usage());
		return 2;
	}
	const std::string &sub = p_args[0];
	const Args a = parse(p_args, 1);
	if (a.bad || (sub != "probe" && a.positional.size() != 2)) {
		std::fprintf(stderr, "%s", nav_usage());
		return 2;
	}
	if (sub == "bake" || sub == "stats") {
		const core::InstallSnapshot before = p_reader.snapshot();
		host::NavBakeBenchOptions o;
		o.level = a.positional[0];
		o.mode = a.positional[1];
		o.threads = a.threads == 0 ? 16 : a.threads;
		o.layer_mask = a.layers;
		o.radius_round_up = a.radius_round_up;
		o.cell_size = a.cell_size;
		o.partition = a.partition;
		o.link_checks = a.links;
		core::Result<host::NavBakeBench> b = host::run_nav_bake_bench(p_install, o);
		if (!b) {
			std::fprintf(stderr, "nav %s: %s\n", sub.c_str(), b.error().message.c_str());
			return 1;
		}
		std::printf("%s", host::format_nav_bake_bench(*b).c_str());
		if (a.tiles || sub == "stats") {
			for (size_t i = 0; i < b->nav->tile_count(); ++i) {
				const sim::NavTileInfo &t = b->nav->tile(i);
				std::printf("T %d %d\tinput %u\tpolys %u\tverts %u\tdetail %u\tlinks %u\tbytes %u\t%.3f s\n", t.tile[0], t.tile[1], t.input_triangles, t.polys,
						t.vertices, t.detail_triangles, t.drop_links, t.data_bytes, t.seconds);
			}
		}
		return finish(p_reader, before, 0);
	}
	if (sub == "components") {
		return cmd_nav_components(p_reader, p_install, a);
	}
	if (sub == "path") {
		return cmd_nav_path(p_reader, p_install, a);
	}
	if (sub == "probe") {
		return cmd_nav_probe(p_reader, p_install, a);
	}
	std::fprintf(stderr, "%s", nav_usage());
	return 2;
}

int cmd_bots(const core::InstallReader &p_reader, const std::shared_ptr<core::Install> &p_install, const std::vector<std::string> &p_args) {
	host::ScaleBenchOptions o;
	std::vector<std::string> positional;
	std::filesystem::path cache_root;
	for (const std::string &s : p_args) {
		unsigned v = 0;
		float f = 0.0f;
		if (s.rfind("--walkers=", 0) == 0 && parse_uint(std::string_view(s).substr(10), v)) {
			o.walkers = v;
		} else if (s.rfind("--observers=", 0) == 0 && parse_uint(std::string_view(s).substr(12), v)) {
			o.observers = v;
		} else if (s.rfind("--seconds=", 0) == 0 && parse_float(s.substr(10), f)) {
			o.seconds = f;
		} else if (s.rfind("--warmup=", 0) == 0 && parse_float(s.substr(9), f)) {
			o.warmup_seconds = f;
		} else if (s.rfind("--paced=", 0) == 0 && parse_float(s.substr(8), f)) {
			o.paced_seconds = f;
		} else if (s.rfind("--seed=", 0) == 0 && parse_uint(std::string_view(s).substr(7), v)) {
			o.seed = v;
		} else if (s.rfind("--threads=", 0) == 0 && parse_uint(std::string_view(s).substr(10), v)) {
			o.load_threads = v;
		} else if (s.rfind("--tuning=", 0) == 0) {
			std::ifstream in(utf8_path(std::string_view(s).substr(9)), std::ios::binary);
			std::ostringstream text;
			text << in.rdbuf();
			o.tuning_text = text.str();
			if (o.tuning_text.empty()) {
				std::fprintf(stderr, "bots: cannot read %s\n", s.c_str() + 9);
				return 2;
			}
		} else if (s == "--radius=ceil" || s == "--radius=nearest") {
			o.radius_round_up = s == "--radius=ceil";
		} else if (s.rfind("--cs=", 0) == 0 && parse_float(s.substr(5), f) && f >= 0.1f && f <= 1.0f) {
			o.cell_size = f;
		} else if (s.rfind("--cache=", 0) == 0) {
			cache_root = utf8_path(std::string_view(s).substr(8));
		} else if (s.rfind("--", 0) == 0) {
			std::fprintf(stderr, "bots: bad option %s\n", s.c_str());
			return 2;
		} else {
			positional.push_back(s);
		}
	}
	if (positional.size() != 2) {
		std::fprintf(stderr, "%s", nav_usage());
		return 2;
	}
	o.level = positional[0];
	o.mode = positional[1];
	if (o.tuning_text.empty()) {

		std::ifstream in("bf3-opensource/config/motor_tuning.cfg", std::ios::binary);
		std::ostringstream text;
		text << in.rdbuf();
		o.tuning_text = text.str();
		std::printf("tuning: %s\n", o.tuning_text.empty() ? "defaults (pass --tuning=<motor_tuning.cfg>)" : "bf3-opensource/config/motor_tuning.cfg");
	}
	const core::InstallSnapshot before = p_reader.snapshot();
	core::Cache cache;
	if (!cache_root.empty()) {
		core::CacheOptions co;
		std::error_code ec;
		co.root = std::filesystem::absolute(cache_root, ec);
		co.guard = p_reader.write_guard();
		std::string detail;
		if (ec || cache.open(std::move(co), &detail) != core::CacheStatus::Ok) {
			std::fprintf(stderr, "bots: cache: %s\n", detail.c_str());
			return 1;
		}
		o.cache = &cache;
	}
	core::Result<host::ScaleReport> r = host::run_scale_bench(p_install, o);
	cache.close();
	if (!r) {
		std::fprintf(stderr, "bots: %s\n", r.error().message.c_str());
		return 1;
	}
	std::printf("%s", host::format_scale_report(*r).c_str());
	return finish(p_reader, before, 0);
}

}
