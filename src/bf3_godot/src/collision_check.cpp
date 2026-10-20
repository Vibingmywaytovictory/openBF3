#include "collision_check.h"

#include "collision_overlay.h"
#include "godot_util.h"

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace bf3::godot_side {

using namespace godot;
namespace lvl = bf3::core::level;

namespace {

std::string fmt(const char *p_fmt, ...) {
	char buf[768];
	va_list args;
	va_start(args, p_fmt);
	vsnprintf(buf, sizeof(buf), p_fmt, args);
	va_end(args);
	return buf;
}

std::string hex64(uint64_t p_value) {
	return fmt("%016llx", static_cast<unsigned long long>(p_value));
}

struct Row {
	std::string check;
	int result;
	std::string detail;
};

struct OracleRow {
	const char *level;
	const char *mode;
	size_t placements;
	size_t triangles;
	size_t terrain;
	size_t destroyed;
	size_t degenerate;
	size_t tiles;
	uint64_t sum;
};
constexpr OracleRow kOracle[] = {
	{ "MP_Subway", "TeamDeathMatch0", 2122, 1136699, 196608, 72733, 34, 181, 0x082eac1e5df0ae1cull },
	{ "XP3_Alborz", "ConquestLarge0", 519, 36215517, 33554432, 56015, 0, 16384, 0x78096d54cf5022f0ull },
};

struct Merged {
	bool ok = true;
	std::string error;
	size_t triangles = 0, terrain = 0, degenerate = 0, tiles = 0;
	uint64_t sum = 0;
	std::vector<uint64_t> terrain_hashes;
};
Merged merged_totals(const lvl::CollisionSoup &p_soup) {
	Merged out;
	out.triangles = p_soup.triangles;
	out.degenerate = p_soup.degenerate;
	out.sum = p_soup.sum;
	std::set<std::pair<int32_t, int32_t>> tiles;
	for (const lvl::SoupTile &t : p_soup.tiles) {
		tiles.insert({ t.x, t.z });
	}
	for (size_t i = 0; i < p_soup.terrains.size(); ++i) {
		const lvl::SoupTerrain &t = p_soup.terrains[i];
		if (t.state != lvl::SoupTerrain::State::Built) {
			continue;
		}
		lvl::WorkBudget budget(size_t(1) << 28);
		bf3::core::Result<lvl::SoupTerrainHash> h = lvl::hash_soup_terrain(t, uint32_t(p_soup.placements.size() + i), budget);
		if (!h) {
			out.ok = false;
			out.error = h.error().message;
			return out;
		}
		out.terrain += h->triangles;
		out.triangles += h->triangles;
		out.degenerate += h->degenerate;
		out.terrain_hashes.push_back(h->hash);
		for (const lvl::SoupTerrainTileSum &ts : h->tiles) {
			tiles.insert({ ts.tile[0], ts.tile[1] });
			out.sum += ts.sum;
		}
	}
	out.tiles = tiles.size();
	return out;
}

bool same_vertex(const Vector3 &p_a, const bf3::core::res::Vec3 &p_b) {
	return std::memcmp(&p_a.x, &p_b[0], 4) == 0 && std::memcmp(&p_a.y, &p_b[1], 4) == 0 && std::memcmp(&p_a.z, &p_b[2], 4) == 0;
}

}

int run_collision_check(Bf3CollisionOverlay &p_overlay) {
	std::vector<Row> rows;
	bool failed = false;
	auto add = [&](const std::string &p_check, int p_result, const std::string &p_detail) {
		rows.push_back({ p_check, p_result, p_detail });
		failed = failed || p_result == 1;
	};
	const lvl::LevelCollision &c = p_overlay.collision();
	const lvl::CollisionSoup &s = c.soup;

	size_t terrain_missing = 0;
	for (const lvl::SoupTerrain &t : s.terrains) {

		terrain_missing += t.state == lvl::SoupTerrain::State::Built && t.grid != nullptr ? 0 : 1;
	}
	const Merged merged = merged_totals(s);
	add("load", (s.missing == 0 && s.nonfinite == 0 && s.built > 0 && terrain_missing == 0) ? 0 : 1,
			fmt("%s %s: %zu placements (%zu built, %zu hidden, %zu missing, %zu without res), %zu res decoded, %zu terrains (%zu not built), %zu triangles "
				"(+ %zu terrain, every terrain with its record) in %zu Havok tiles, %zu non-finite",
					p_overlay.level_id().c_str(), p_overlay.mode_id().c_str(), s.placements.size(), s.built, s.hidden, s.missing, s.no_res, s.res_decoded,
					s.terrains.size(), terrain_missing, s.triangles, s.terrain_triangles, s.tiles.size(), s.nonfinite));
	add("timing", 2,
			fmt("load %.3f s (mount %.3f, traverse %.3f, soup %.3f: placements %.3f, decode %.3f, triangles %.3f, terrain %.3f, tiles %.3f), prepare %.3f s, "
				"upload %.3f s",
					p_overlay.load_seconds(), c.seconds_load, c.seconds_traverse, c.seconds_soup, s.seconds_placements, s.seconds_decode, s.seconds_triangles,
					s.seconds_terrain, s.seconds_tiles, p_overlay.prepare_seconds(), p_overlay.upload_seconds()));

	{
		const auto t0 = std::chrono::steady_clock::now();
		bf3::core::Result<lvl::LevelCollision> again = lvl::load_level_collision(p_overlay.opened_install(), p_overlay.level_id(), p_overlay.mode_id());
		const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		if (!again) {
			add("godot_free.recompute", 1, again.error().message);
		} else {
			const lvl::CollisionSoup &g = again->soup;
			size_t same_hashes = 0;
			if (g.placements.size() == s.placements.size()) {
				for (size_t i = 0; i < s.placements.size(); ++i) {
					same_hashes += (g.placements[i].hash == s.placements[i].hash && g.placements[i].triangles == s.placements[i].triangles &&
										   g.placements[i].res == s.placements[i].res)
							? 1
							: 0;
				}
			}

			const Merged gm = merged_totals(g);
			bool same_terrain = g.terrains.size() == s.terrains.size() && gm.ok && merged.ok && gm.terrain_hashes == merged.terrain_hashes &&
					gm.terrain == merged.terrain && gm.sum == merged.sum;
			for (size_t i = 0; same_terrain && i < s.terrains.size(); ++i) {
				same_terrain = g.terrains[i].cells == s.terrains[i].cells && g.terrains[i].open == s.terrains[i].open;
			}
			const bool same = g.placements.size() == s.placements.size() && same_hashes == s.placements.size() && same_terrain &&
					g.terrain_triangles == s.terrain_triangles && g.triangles == s.triangles &&
					g.destroyed == s.destroyed && g.degenerate == s.degenerate && g.tiles.size() == s.tiles.size() && g.sum == s.sum && g.bodies == s.bodies &&
					g.surfaces == s.surfaces;
			add("godot_free.recompute", same ? 0 : 1,
					fmt("second load_level_collision in %.2f s: %zu placements (%zu with the same hash), %zu triangles, %zu tiles, sum %s (overlay: %s)", seconds,
							g.placements.size(), same_hashes, g.triangles, g.tiles.size(), hex64(g.sum).c_str(), hex64(s.sum).c_str()));
		}
	}

	const OracleRow *oracle = nullptr;
	for (const OracleRow &o : kOracle) {
		if (p_overlay.level_id() == o.level && p_overlay.mode_id() == o.mode) {
			oracle = &o;
		}
	}
	if (oracle == nullptr) {
		add("oracle", 2, "no level-oracle numbers recorded for this level x mode");
	} else {
		const bool same = merged.ok && s.placements.size() == oracle->placements && s.built == oracle->placements && merged.triangles == oracle->triangles &&
				merged.terrain == oracle->terrain && s.terrain_triangles == oracle->terrain && s.destroyed == oracle->destroyed &&
				merged.degenerate == oracle->degenerate && merged.tiles == oracle->tiles && merged.sum == oracle->sum;
		add("oracle", same ? 0 : 1,
				fmt("level-oracle collision: %zu placements, %zu triangles (%zu terrain), %zu destroyed, %zu degenerate, %zu tiles, sum %s; overlay %zu, %zu (%zu), "
					"%zu, %zu, %zu, %s",
						oracle->placements, oracle->triangles, oracle->terrain, oracle->destroyed, oracle->degenerate, oracle->tiles, hex64(oracle->sum).c_str(),
						s.placements.size(), merged.triangles, merged.terrain, s.destroyed, merged.degenerate, merged.tiles, hex64(merged.sum).c_str()));
	}

	std::map<std::pair<std::pair<int32_t, int32_t>, uint8_t>, std::vector<const lvl::SoupTriangle *>> expected;
	std::map<std::pair<int32_t, int32_t>, size_t> tile_meshes;
	for (const lvl::SoupTile &tile : s.tiles) {
		tile_meshes[{ tile.x, tile.z }] = 0;
		for (const lvl::SoupTriangle &t : tile.triangles) {
			expected[{ { tile.x, tile.z }, bf3::render::collision_layer(t) }].push_back(&t);
		}
	}
	const bf3::render::TerrainWindowStats &window = p_overlay.window();
	std::map<std::pair<int32_t, int32_t>, std::vector<lvl::SoupTriangle>> terrain_store;
	size_t window_expected = 0, window_errors = 0, window_triangles = 0;
	{

		const std::vector<std::array<int32_t, 2>> candidates = bf3::render::terrain_window_candidates(p_overlay.window_options());
		for (const std::array<int32_t, 2> &tile : candidates) {
			std::vector<lvl::SoupTriangle> tris;
			bool ok = true;
			for (size_t i = 0; i < s.terrains.size() && ok; ++i) {
				if (s.terrains[i].state != lvl::SoupTerrain::State::Built) {
					continue;
				}
				lvl::WorkBudget budget(bf3::render::kTerrainWindowTileBudget);
				ok = lvl::append_soup_terrain_tile(s.terrains[i], uint32_t(s.placements.size() + i), tile, tris, budget).ok();
			}
			window_errors += ok ? 0 : 1;
			if (ok && !tris.empty()) {
				terrain_store[{ tile[0], tile[1] }] = std::move(tris);
			}
		}

		std::set<std::pair<int32_t, int32_t>> held;
		for (const Bf3CollisionOverlay::TileMesh &m : p_overlay.meshes()) {
			if (m.layer == bf3::render::kTerrainLayer) {
				held.insert({ m.tile_x, m.tile_z });
			}
		}
		for (auto &[tile, tris] : terrain_store) {
			if (window.capped && held.count(tile) == 0) {
				continue;
			}
			++window_expected;
			window_triangles += tris.size();
			tile_meshes[tile] = 0;
			for (const lvl::SoupTriangle &t : tris) {
				expected[{ tile, bf3::render::kTerrainLayer }].push_back(&t);
			}
		}
	}
	add("terrain.window", (window_errors == 0 && window.tiles_failed == 0 && window.tiles == window_expected && window.triangles == window_triangles) ? 0 : 1,
			fmt("%zu tiles (%zu triangles) within %.0f m of the camera's (%.1f, %.1f)%s: expected %zu tiles (%zu triangles) from append_soup_terrain_tile, "
				"%zu errors",
					window.tiles, window.triangles, double(window.radius), double(window.centre[0]), double(window.centre[1]), window.capped ? ", capped" : "",
					window_expected, window_triangles, window_errors + window.tiles_failed));
	const std::vector<Bf3CollisionOverlay::TileMesh> &meshes = p_overlay.meshes();
	add("meshes.count", (meshes.size() == expected.size() && p_overlay.prepared().size() == expected.size()) ? 0 : 1,
			fmt("%zu RenderingServer meshes for %zu (tile, layer) slots of the soup (%zu prepared)", meshes.size(), expected.size(), p_overlay.prepared().size()));

	RenderingServer *rs = RenderingServer::get_singleton();
	size_t valid = 0, one_surface = 0, right_count = 0, matched = 0, with_material = 0, instances = 0, vertices = 0;
	for (const Bf3CollisionOverlay::TileMesh &m : meshes) {
		if (!m.mesh.is_valid()) {
			continue;
		}
		++valid;
		instances += m.instance.is_valid() ? 1 : 0;
		if (rs->mesh_get_surface_count(m.mesh) != 1) {
			continue;
		}
		++one_surface;
		with_material += rs->mesh_surface_get_material(m.mesh, 0) == p_overlay.material() ? 1 : 0;
		const Array arrays = rs->mesh_surface_get_arrays(m.mesh, 0);
		const PackedVector3Array positions = arrays[RenderingServer::ARRAY_VERTEX];
		vertices += size_t(positions.size());
		const auto it = expected.find({ { m.tile_x, m.tile_z }, m.layer });
		if (it == expected.end() || size_t(positions.size()) != 3 * it->second.size() || m.triangles != it->second.size()) {
			continue;
		}
		++right_count;
		tile_meshes[{ m.tile_x, m.tile_z }]++;

		const lvl::SoupTriangle &first = *it->second.front();
		const lvl::SoupTriangle &last = *it->second.back();
		const int64_t n = positions.size();
		const bool ok = same_vertex(positions[0], first.v[0]) && same_vertex(positions[1], first.v[2]) && same_vertex(positions[2], first.v[1]) &&
				same_vertex(positions[n - 3], last.v[0]) && same_vertex(positions[n - 2], last.v[2]) && same_vertex(positions[n - 1], last.v[1]);
		matched += ok ? 1 : 0;
	}
	add("meshes.surfaces", (valid == meshes.size() && one_surface == meshes.size() && right_count == meshes.size()) ? 0 : 1,
			fmt("%zu valid, %zu with one surface, %zu with 3 x the slot's triangles as vertices", valid, one_surface, right_count));
	add("meshes.vertices", vertices == 3 * (s.triangles + window.triangles) ? 0 : 1,
			fmt("%zu vertices over all meshes = 3 x (%zu Havok + %zu window terrain triangles)", vertices, s.triangles, window.triangles));
	add("meshes.winding", matched == meshes.size() ? 0 : 1,
			fmt("%zu of %zu meshes hold the soup's first and last triangle of their slot bit for bit, as (v0, v2, v1)", matched, meshes.size()));
	add("meshes.material", with_material == meshes.size() ? 0 : 1, fmt("%zu of %zu surfaces use the shared overlay material", with_material, meshes.size()));
	size_t covered = 0;
	for (const auto &[tile, count] : tile_meshes) {
		covered += count > 0 ? 1 : 0;
	}
	add("tiles", (covered == tile_meshes.size() && instances == meshes.size()) ? 0 : 1,
			fmt("%zu of %zu tiles (Havok and terrain window) have their meshes; %zu instances", covered, tile_meshes.size(), instances));

	if (c.spawns.empty()) {
		add("camera.start", 2, "no AlternateSpawn in this level x mode: the camera starts above the centre");
	} else {
		const Vector3 cam = p_overlay.camera()->get_global_transform().origin;
		const lvl::LevelTransform &sp = c.spawns.front();
		const float d = cam.distance_to(Vector3(sp.trans[0], sp.trans[1] + 1.7f, sp.trans[2]));
		add("camera.start", d < 1e-3f ? 0 : 1,
				fmt("camera at (%.2f, %.2f, %.2f), spawn 1 of %zu at (%.2f, %.2f, %.2f) + 1.7 m", cam.x, cam.y, cam.z, c.spawns.size(), sp.trans[0], sp.trans[1],
						sp.trans[2]));
	}
	add("bounds", 2,
			fmt("x %.1f..%.1f, y %.1f..%.1f, z %.1f..%.1f m; bodies collision %zu, raycast %zu, character %zu", s.min[0], s.max[0], s.min[1], s.max[1], s.min[2],
					s.max[2], s.bodies[0], s.bodies[1], s.bodies[2]));

	const bf3::core::InstallSnapshot &before = p_overlay.install_snapshot_at_start();
	if (before.valid) {
		const bf3::core::SnapshotDiff d = bf3::core::diff_snapshots(before, p_overlay.install().snapshot());
		add("install.unchanged", d.clean() ? 0 : 1, d.clean() ? fmt("%zu files: sizes and mtimes equal", before.files.size()) : d.summary());
	} else {
		add("install.unchanged", 1, "no snapshot at start");
	}

	size_t checks = 0, passed = 0;
	UtilityFunctions::print("");
	UtilityFunctions::print("COLLISION CHECK");
	for (const Row &r : rows) {
		static const char *names[] = { "PASS", "FAIL", "INFO" };
		UtilityFunctions::print(to_godot(fmt("  %-5s %-22s %s", names[r.result], r.check.c_str(), r.detail.c_str())));
		if (r.result != 2) {
			++checks;
			passed += r.result == 0 ? 1 : 0;
		}
	}
	UtilityFunctions::print(to_godot(fmt("COLLISION CHECK: %s (%zu/%zu)", failed ? "FAIL" : "PASS", passed, checks)));
	return failed ? 1 : 0;
}

}
