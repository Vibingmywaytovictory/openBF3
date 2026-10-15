// bf3dump collision <LEVEL> <MODE>: the collision soup (bf3_core level/collision_soup.h) in the
// canonical form research/probes/tools/level-oracle `collision` prints through IW4L's crates, so
// the two diff byte-identical when the soups agree: sorted `T` placement lines (state, triangle
// counts, FNV-1a hash), `K` tiles (count, hash sum), then the totals and the surface and body
// histograms. Timings go to stderr.
#include "collision_commands.h"

#include "bf3/core/level/collision_soup.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using namespace bf3::core;
using namespace bf3::core::level;

namespace bf3dump {

namespace {

std::string hex64(uint64_t p_value) {
	char text[24];
	std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(p_value));
	return text;
}

std::string counts(const SoupPlacement &p_p) {
	return "tris " + std::to_string(p_p.triangles) + " destroyed " + std::to_string(p_p.destroyed) + " degenerate " + std::to_string(p_p.degenerate) +
			" nonfinite " + std::to_string(p_p.nonfinite) + " hash " + hex64(p_p.hash);
}

} // namespace

std::string collision_listing(const LevelCollision &p_collision) {
	const EbxResolver &db = p_collision.level->db();
	const World &world = *p_collision.world;
	const CollisionSoup &soup = p_collision.soup;
	std::vector<std::string> lines;
	lines.reserve(soup.placements.size());
	for (const SoupPlacement &p : soup.placements) {
		if (p.kind == CollisionPlacement::Kind::Group) {
			lines.push_back("T group " + std::to_string(p.index) + " res " + p.res + " " + counts(p));
			continue;
		}
		const EbxInstance &entity = world.objects[p.index].entity;
		const std::string label = std::string(db.name_of(entity.partition().guid()).value_or("?")) + "#" + std::to_string(entity.index());
		switch (p.state) {
			case SoupPlacement::State::NoRes:
				lines.push_back("T object " + label + " res - nores");
				break;
			case SoupPlacement::State::Hidden:
				lines.push_back("T object " + label + " res " + p.res + " hidden");
				break;
			case SoupPlacement::State::Missing:
				lines.push_back("T object " + label + " res " + p.res + " missing");
				break;
			case SoupPlacement::State::Built:
				lines.push_back("T object " + label + " res " + p.res + " built " + counts(p));
				break;
		}
	}
	for (const SoupTerrain &t : soup.terrains) {
		const EbxInstance &entity = world.objects[t.object].entity;
		const std::string label = std::string(db.name_of(entity.partition().guid()).value_or("?")) + "#" + std::to_string(entity.index());
		switch (t.state) {
			case SoupTerrain::State::NoRes:
				lines.push_back("T terrain " + label + " res - nores");
				break;
			case SoupTerrain::State::Missing:
				lines.push_back("T terrain " + label + " res " + t.asset + " missing");
				break;
			case SoupTerrain::State::Built:
				lines.push_back("T terrain " + label + " res " + t.asset + " built depth " + std::to_string(t.depth) + " side " + std::to_string(t.side) +
						" holes " + std::to_string(t.holes) + " cells " + std::to_string(t.cells) + " open " + std::to_string(t.open) + " tris " +
						std::to_string(t.triangles) + " destroyed 0 degenerate " + std::to_string(t.degenerate) + " nonfinite " + std::to_string(t.nonfinite) +
						" hash " + hex64(t.hash));
				break;
		}
	}
	std::sort(lines.begin(), lines.end());
	std::string out;
	for (const std::string &line : lines) {
		out += line;
		out += '\n';
	}
	for (const SoupTile &tile : soup.tiles) {
		out += "K " + std::to_string(tile.x) + " " + std::to_string(tile.z) + " tris " + std::to_string(tile.triangles.size()) + " sum " + hex64(tile.sum) + "\n";
	}
	out += "soup placements " + std::to_string(soup.placements.size()) + " built " + std::to_string(soup.built) + " hidden " +
			std::to_string(soup.hidden) + " missing " + std::to_string(soup.missing) + " triangles " + std::to_string(soup.triangles) + " terrain " +
			std::to_string(soup.terrain_triangles) + " destroyed " +
			std::to_string(soup.destroyed) + " degenerate " + std::to_string(soup.degenerate) + " nonfinite " + std::to_string(soup.nonfinite) + " tiles " +
			std::to_string(soup.tiles.size()) + " sum " + hex64(soup.sum) + "\n";
	for (size_t k = 0; k < kSurfaceCount; ++k) {
		if (soup.surfaces[k] > 0) {
			out += "surface " + std::to_string(k) + " " + std::to_string(soup.surfaces[k]) + "\n";
		}
	}
	for (size_t k = 0; k < kCollisionBodyCount; ++k) {
		out += "body " + std::to_string(k) + " " + std::to_string(soup.bodies[k]) + "\n";
	}
	return out;
}

int cmd_collision(const std::shared_ptr<Install> &p_install, const std::string &p_level, const std::string &p_mode) {
	const auto start = std::chrono::steady_clock::now();
	Result<LevelCollision> collision = load_level_collision(p_install, p_level, p_mode);
	if (!collision) {
		std::fprintf(stderr, "collision %s %s: %s\n", p_level.c_str(), p_mode.c_str(), collision.error().message.c_str());
		return 1;
	}
	const CollisionSoup &soup = collision->soup;
	const double total = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	std::fprintf(stderr,
			"collision %s %s: %zu placements (%zu built, %zu hidden, %zu missing, %zu without res), %zu res decoded, %zu triangles in %zu tiles; "
			"material pairs %zu/%zu resolved\n",
			p_level.c_str(), p_mode.c_str(), soup.placements.size(), soup.built, soup.hidden, soup.missing, soup.no_res, soup.res_decoded, soup.triangles,
			soup.tiles.size(), soup.resolved_pairs, soup.material_pairs);
	std::fprintf(stderr, "time: load %.3f s, traverse %.3f s, soup %.3f s (placements %.3f, decode %.3f, triangles %.3f, terrain %.3f, tiles %.3f); total %.3f s\n",
			collision->seconds_load, collision->seconds_traverse, collision->seconds_soup, soup.seconds_placements, soup.seconds_decode, soup.seconds_triangles,
			soup.seconds_terrain, soup.seconds_tiles, total);
	for (const SoupPlacement &p : soup.placements) {
		if (p.state == SoupPlacement::State::Missing) {
			std::fprintf(stderr, "missing: %s: %s\n", p.res.c_str(), p.error.c_str());
		}
	}
	for (const SoupTerrain &t : soup.terrains) {
		if (t.state == SoupTerrain::State::Missing) {
			std::fprintf(stderr, "terrain missing: %s\n", t.error.c_str());
		}
	}
	std::fprintf(stderr, "work budget: %zu of %zu units spent\n", collision->level->budget->spent(), collision->level->budget->limit());
	const std::string text = collision_listing(*collision);
	std::fwrite(text.data(), 1, text.size(), stdout);
	return 0;
}

} // namespace bf3dump
