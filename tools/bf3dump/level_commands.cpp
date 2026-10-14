// bf3dump level commands. The listing is the canonical form of research/probes/tools/level-oracle
// (src/main.rs): same lines, same sorting, floats as their f32 bit patterns, so the two diff
// byte-identical when the C++ traversal matches IW4L's level_bf3.
//
// Listing: level / bundle / settings / subworld-bundle / absent / partitions / world lines, then
// sorted sections: skipped and absent sub-worlds, `S` sub-worlds, `G` static model groups, `P`
// placements (every placed object: kind, entity, reference, sub-world, variation, group slot, mesh
// asset with entity type, physics data and hidden composite parts, start-hidden flag, transform as
// right/up/forward/trans rows + scale), `V` mesh variants and `F` undecodable MeshSets of the mesh
// census, the census totals, `C` collision placements and their totals.
#include "level_commands.h"

#include "bf3/core/level/census.h"
#include "bf3/core/level/level.h"
#include "bf3/core/level/world.h"
#include "bf3/core/superbundle_mount.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <vector>

using namespace bf3::core;
using namespace bf3::core::level;

namespace bf3dump {

namespace {

double seconds_since(std::chrono::steady_clock::time_point p_start) {
	return std::chrono::duration<double>(std::chrono::steady_clock::now() - p_start).count();
}

std::string hex(float p_value) {
	uint32_t bits = 0;
	std::memcpy(&bits, &p_value, 4);
	char text[16];
	std::snprintf(text, sizeof(text), "%08x", bits);
	return text;
}

std::string hex32(uint32_t p_value) {
	char text[16];
	std::snprintf(text, sizeof(text), "%08x", p_value);
	return text;
}

std::string transform_hex(const LevelTransform &p_t) {
	std::string out;
	for (const level::Vec3 &row : p_t.rows()) {
		for (float v : row) {
			out += hex(v);
			out += ' ';
		}
	}
	out += hex(p_t.scale);
	return out;
}

std::string id(const EbxResolver &p_db, const EbxInstance &p_instance) {
	return std::string(p_db.name_of(p_instance.partition().guid()).value_or("?")) + "#" + std::to_string(p_instance.index());
}

std::string opt_id(const EbxResolver &p_db, const std::optional<EbxInstance> &p_instance) {
	return p_instance ? id(p_db, *p_instance) : "-";
}

template <class T>
std::string opt_num(const std::optional<T> &p_value) {
	return p_value ? std::to_string(*p_value) : "-";
}

std::string join_u16(const std::vector<uint16_t> &p_values) {
	std::string out;
	for (size_t i = 0; i < p_values.size(); ++i) {
		out += (i ? "," : "") + std::to_string(p_values[i]);
	}
	return out;
}

void flush(std::string &r_out, std::vector<std::string> &p_lines) {
	std::sort(p_lines.begin(), p_lines.end());
	for (const std::string &line : p_lines) {
		r_out += line;
		r_out += '\n';
	}
	p_lines.clear();
}

std::string hidden_label(const EbxResolver &p_db, const MeshObject &p_mesh, const EbxInstance &p_entity) {
	if (!p_mesh.asset.is_a("CompositeMeshAsset")) {
		return "-";
	}
	const std::optional<std::vector<uint16_t>> parts = hidden_parts(p_db, p_entity);
	return parts ? "[" + join_u16(*parts) + "]" : "noparts";
}

std::string placement_line(const EbxResolver &p_db, const World &p_world, const PlacedObject &p_object) {
	std::string line = "P " + std::string(p_object.label()) + " " + id(p_db, p_object.entity) + " " + std::string(p_object.entity.type_name()) + " ref " +
			opt_id(p_db, p_object.reference) + " sw " + std::to_string(p_object.subworld) + " var " + hex32(p_object.variation);
	if (p_object.group) {
		const GroupSlot &slot = *p_object.group;
		line += " grp " + std::to_string(slot.group) + "/" + std::to_string(slot.member) + "/" + std::to_string(slot.instance) + "/" +
				opt_num(slot.havok_subpart) + "/" + opt_num(slot.physics_part);
	} else {
		line += " grp -";
	}
	if (p_object.kind == ObjectKind::Mesh && p_object.mesh) {
		const MeshObject &mesh = *p_object.mesh;
		line += " mesh " + id(p_db, mesh.asset) + " " + std::string(mesh_entity_type_name(mesh.entity_type)) + " phys " + opt_id(p_db, mesh.physics) +
				" hid " + hidden_label(p_db, mesh, p_object.entity);
	} else {
		line += " mesh -";
	}
	line += std::string(" start_hidden ") + (starts_hidden(p_world, p_object) ? "1" : "0") + " t " + transform_hex(p_object.transform);
	return line;
}

std::string listing(const LoadedLevel &p_level, const World &p_world, const MeshCensus &p_census, const std::vector<CollisionPlacement> &p_collision) {
	const EbxResolver &db = p_level.db();
	std::string out;
	std::vector<std::string> lines;
	out += "level " + p_level.info.id + " mode " + p_level.mode + "\n";
	out += "bundle " + bundle_key(p_level.bundles.level) + "\n";
	for (const std::string &name : p_level.bundles.settings) {
		out += "settings " + bundle_key(name) + "\n";
	}
	for (const std::string &name : p_level.bundles.subworlds) {
		out += "subworld-bundle " + bundle_key(name) + "\n";
	}
	for (const std::string &name : p_level.bundles.absent) {
		out += "absent " + name + "\n";
	}
	const PartitionStats &ps = p_level.stats;
	out += "partitions " + std::to_string(ps.partitions) + " duplicates " + std::to_string(ps.duplicates) + " failed " + std::to_string(ps.failed.size()) +
			" imports " + std::to_string(ps.imports) + " unresolved " + std::to_string(ps.unresolved_imports) + "\n";
	const WorldStats &ws = p_world.stats;
	out += "world objects " + std::to_string(p_world.objects.size()) + " subworlds " + std::to_string(p_world.subworlds.size()) + " scopes " +
			std::to_string(p_world.scopes.size()) + " groups " + std::to_string(p_world.groups.size()) + " excluded " + std::to_string(ws.excluded) +
			" excluded_overrides " + std::to_string(ws.excluded_overrides) + " transform_overrides " + std::to_string(ws.transform_overrides) +
			" unresolved_objects " + std::to_string(ws.unresolved_objects) + " missing_blueprints " + std::to_string(ws.missing_blueprints) +
			" depth_limited " + std::to_string(ws.depth_limited) + "\n";
	for (const std::string &name : ws.skipped_subworlds) {
		lines.push_back("skipped " + name);
	}
	for (const std::string &name : ws.absent_subworlds) {
		lines.push_back("absent-subworld " + name);
	}
	flush(out, lines);

	for (size_t at = 0; at < p_world.subworlds.size(); ++at) {
		const SubWorld &sw = p_world.subworlds[at];
		lines.push_back("S " + std::to_string(at) + " " + sw.name + " parent " + opt_num(sw.parent) + " mvdb " + std::to_string(sw.variations.size()));
	}
	flush(out, lines);

	for (size_t at = 0; at < p_world.groups.size(); ++at) {
		const StaticModelGroup &g = p_world.groups[at];
		lines.push_back("G " + std::to_string(at) + " sw " + std::to_string(g.subworld) + " res " + g.physics_asset.value_or("-") + " members " +
				std::to_string(g.members) + " instances " + std::to_string(g.instances) + " explicit " + std::to_string(g.explicit_count) + " havok " +
				std::to_string(g.havok_placed) + " subparts " + std::to_string(g.subparts) + " mismatches " + std::to_string(g.part_mismatches) + " t " +
				transform_hex(g.transform));
	}
	flush(out, lines);

	lines.reserve(p_world.objects.size());
	for (const PlacedObject &object : p_world.objects) {
		lines.push_back(placement_line(db, p_world, object));
	}
	flush(out, lines);

	for (const auto &[name, error] : p_census.failures) {
		lines.push_back("F " + name);
	}
	for (const SceneMesh &mesh : p_census.meshes) {
		if (mesh.failed) {
			continue;
		}
		std::string bake = "-";
		if (mesh.bake) {
			bake.clear();
			for (size_t i = 0; i < 9; ++i) {
				bake += (i ? "," : "") + std::to_string(mesh.bake_key[i]);
			}
		}
		std::string lods;
		for (bool l : mesh.lods) {
			lods += l ? '1' : '0';
		}
		lines.push_back("V " + id(db, mesh.asset) + " entry " + opt_id(db, mesh.entry) + " hid [" + join_u16(mesh.hidden) + "] bake " + bake + " n " +
				std::to_string(mesh.placements) + " radius " + hex(mesh.radius) + " kind " + (mesh.world_kind ? "world" : "model") + " lods " + lods +
				" kept " + (mesh.kept ? "1" : "0"));
	}
	flush(out, lines);
	out += "census placements " + std::to_string(p_census.placements) + " variants " + std::to_string(p_census.meshes.size()) + " meshes " +
			std::to_string(p_census.scene_meshes) + " model " + std::to_string(p_census.model_meshes) + " world " + std::to_string(p_census.world_meshes) +
			" instances " + std::to_string(p_census.scene_instances) + " failures " + std::to_string(p_census.failures.size()) + "\n";

	size_t groups = 0, objects = 0;
	for (const CollisionPlacement &c : p_collision) {
		if (c.kind == CollisionPlacement::Kind::Group) {
			++groups;
			const StaticModelGroup &g = p_world.groups[c.index];
			lines.push_back("C group " + std::to_string(c.index) + " res " + c.res + " subparts " + std::to_string(c.subparts) + " t " + transform_hex(g.transform));
			continue;
		}
		const PlacedObject &object = p_world.objects[c.index];
		std::string bodies;
		for (size_t i = 0; i < c.bodies.size(); ++i) {
			bodies += (i ? "," : "") + std::string(c.bodies[i]);
		}
		if (c.res.empty()) {
			lines.push_back("C object " + id(db, object.entity) + " res - bodies " + bodies);
			continue;
		}
		++objects;
		std::string frame;
		for (const res::Vec3 &row : c.frame.axis) {
			for (float v : row) {
				frame += hex(v) + " ";
			}
		}
		for (float v : c.frame.origin) {
			frame += hex(v) + " ";
		}
		frame.pop_back();
		lines.push_back("C object " + id(db, object.entity) + " res " + c.res + " present " + (c.present ? "1" : "0") + " scale " + hex(c.asset_scale) +
				" bodies " + bodies + " frame " + frame);
	}
	flush(out, lines);
	out += "collision groups " + std::to_string(groups) + " objects " + std::to_string(objects) + "\n";
	return out;
}

} // namespace

int cmd_level(const std::shared_ptr<Install> &p_install, const std::string &p_level, const std::string &p_mode, bool p_legacy_mvdb) {
	auto start = std::chrono::steady_clock::now();
	Result<std::unique_ptr<LoadedLevel>> loaded = load_level(p_install, p_level, p_mode);
	if (!loaded) {
		std::fprintf(stderr, "level %s %s: %s\n", p_level.c_str(), p_mode.c_str(), loaded.error().message.c_str());
		return 1;
	}
	const LoadedLevel &level = **loaded;
	std::fprintf(stderr, "loaded %zu partitions in %.3f s (bundles %.3f s, parse %.3f s)\n", level.stats.partitions, seconds_since(start),
			level.stats.bundle_seconds, level.stats.parse_seconds);
	start = std::chrono::steady_clock::now();
	Result<std::unique_ptr<World>> world = World::traverse(level);
	if (!world) {
		std::fprintf(stderr, "traverse %s %s: %s\n", p_level.c_str(), p_mode.c_str(), world.error().message.c_str());
		return 1;
	}
	std::fprintf(stderr, "traversed %zu objects in %.3f s\n", (*world)->objects.size(), seconds_since(start));
	start = std::chrono::steady_clock::now();
	CensusOptions options;
	options.legacy_variations = p_legacy_mvdb;
	const MeshCensus census = mesh_census(level, **world, options);
	const std::vector<CollisionPlacement> collision = collision_placements(level, **world);
	std::fprintf(stderr, "census: %zu placements -> %zu meshes (%zu model, %zu world), %zu instances, %zu failures in %.3f s\n", census.placements,
			census.scene_meshes, census.model_meshes, census.world_meshes, census.scene_instances, census.failures.size(), seconds_since(start));
	const std::string text = listing(level, **world, census, collision);
	std::fwrite(text.data(), 1, text.size(), stdout);
	return 0;
}

int cmd_level_modes(const std::shared_ptr<Install> &p_install, const std::string &p_level) {
	Result<LevelInfo> info = level_info(*p_install, p_level);
	if (!info) {
		std::fprintf(stderr, "level %s: %s\n", p_level.c_str(), info.error().message.c_str());
		return 1;
	}
	for (const std::string &mode : info->modes) {
		std::printf("%s\n", mode.c_str());
	}
	return 0;
}

} // namespace bf3dump
