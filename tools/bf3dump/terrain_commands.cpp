#include "terrain_commands.h"

#include "bf3/core/install.h"
#include "bf3/core/res/terrain.h"
#include "bf3/core/res_types.h"
#include "bf3/core/superbundle_mount.h"
#include "bf3/core/util/elapsed.h"
#include "bf3/render/look_materials.h"
#include "bf3/render/terrain_look.h"
#include "bf3/render/terrain_scatter.h"

#include <cstdio>
#include <map>

namespace bf3dump {

using namespace bf3::core;

int cmd_terrain(const std::shared_ptr<Install> &p_install, const std::vector<std::string> &p_args) {
	if (p_args.empty()) {
		std::fprintf(stderr, "usage: bf3dump terrain [--scatter] <LEVEL>...\n");
		return 2;
	}
	int failures = 0;
	bool list_scatter = false;
	for (const std::string &arg : p_args) {
		list_scatter = list_scatter || arg == "--scatter";
	}
	for (const std::string &level : p_args) {
		if (level.starts_with("--")) {
			continue;
		}
		const auto start = std::chrono::steady_clock::now();
		const std::string sb = "Win32/Levels/" + level + "/" + level;
		Result<LevelMount> mount = p_install->mount_superbundle(sb);
		if (!mount || !mount->load_all_bundles().ok()) {
			std::printf("%s\tno level\n", level.c_str());
			++failures;
			continue;
		}
		std::unique_ptr<bf3::render::LookResolver> resolver = bf3::render::LookResolver::open(*mount);
		for (const std::string &name : mount->res_names()) {
			const std::optional<ResInfo> info = mount->res_info(name);
			if (!info || info->res_type != res::kResTypeTerrain) {
				continue;
			}
			auto terrain = mount->read_res(name);
			std::string tree_name;
			if (!terrain || !res::terrain_streaming_tree_name(terrain->second, tree_name).ok()) {
				std::printf("%s\t%s\tno streaming tree name\n", level.c_str(), name.c_str());
				++failures;
				continue;
			}
			auto bytes = mount->read_res(tree_name);
			res::TerrainStreamingTree tree;
			const res::ResError e = bytes ? res::TerrainStreamingTree::parse(bytes->second, tree)
										  : res::ResError::malformed("missing", 0);
			if (!e.ok()) {
				std::printf("%s\t%s\tparse: %s\n", level.c_str(), name.c_str(), e.message().c_str());
				++failures;
				continue;
			}
			std::map<std::array<uint8_t, 16>, std::vector<uint8_t>> chunks;
			const res::TerrainChunkFetch fetch = [&](const res::ChunkId &p_id) -> std::optional<std::vector<uint8_t>> {
				auto found = chunks.find(p_id.bytes);
				if (found == chunks.end()) {
					Result<std::vector<uint8_t>> c = mount->read_chunk(Guid::from_bytes(p_id.bytes.data()));
					if (!c) {
						return std::nullopt;
					}
					found = chunks.emplace(p_id.bytes, std::move(*c)).first;
				}
				return found->second;
			};
			std::printf("%s\t%s\tmask %s colour %s", level.c_str(), name.c_str(),
					tree.mask ? (std::to_string(tree.mask->nodes.size()) + " nodes, " +
										std::to_string(tree.mask->layer_count()) + " layers, side " +
										std::to_string(tree.mask->samples_per_side))
										.c_str()
							  : "none",
					tree.colour ? (std::to_string(tree.colour->nodes.size()) + " nodes, side " +
										  std::to_string(tree.colour->texels_per_side))
										  .c_str()
								: "none");
			std::vector<std::pair<std::string, std::string>> plan_failures;
			uint32_t textures = 0;
			const std::vector<bf3::render::SceneTerrainCombo> combos = bf3::render::plan_terrain_combos(
					*mount, resolver->shaders(), name, [&](const std::string &) { return int32_t(textures++); },
					plan_failures);
			std::printf(", %zu combinations, %zu plan failures", combos.size(), plan_failures.size());
			for (const auto &[item, error] : plan_failures) {
				std::printf(" [%s: %s]", item.c_str(), error.c_str());
			}
			if (!tree.mask || combos.empty() || !tree.heightfield) {
				std::printf(", flat\n");
				continue;
			}
			res::HeightGrid heights;
			const res::ResError he =
					tree.height_grid(std::min<uint8_t>(tree.heightfield->max_level(), 4), fetch, heights);
			if (!he.ok()) {
				std::printf(", heights: %s\n", he.message().c_str());
				++failures;
				continue;
			}
			bf3::render::TerrainLookInput input;
			input.tree = &tree;
			input.heights = &heights;
			const float half = 1024.0f;
			input.region = { -half, -half, half, half };
			input.combos = combos;
			input.fetch = fetch;
			const Result<bf3::render::TerrainSurfaceLanes> surface =
					bf3::render::terrain_surface_lanes(resolver->shaders(), name);
			if (!surface) {
				std::printf(", surface lanes: %s\n", surface.error().message.c_str());
				++failures;
				continue;
			}
			std::printf(", surface smoothness %s specular %s",
					bf3::render::terrain_lane_code(surface->smoothness, {}).c_str(),
					bf3::render::terrain_lane_code(surface->specular, {}).c_str());
			input.surface = *surface;
			Result<bf3::render::TerrainLook> look = bf3::render::build_terrain_look(input);
			if (!look) {
				std::printf(", look: %s\n", look.error().message.c_str());
				++failures;
				continue;
			}
			const bf3::render::TerrainLookStats &s = look->stats;
			std::printf(", patch %.0f m: %u patches (%u exact, %u superset, %u opaque, %u misses), %zu materials, %.1f "
						"MiB images, masks at %.2f m (data %.2f m), colour at %.2f m (data %.2f m), %.2f s\n",
					double(look->patch), s.patches, s.exact, s.superset, s.opaque, s.misses, look->materials.size(),
					double(s.image_bytes) / 1048576.0, double(s.mask_texel), double(s.data_mask_texel),
					double(s.colour_texel), double(s.data_colour_texel), seconds_since(start));
			for (const std::vector<uint8_t> &set : s.missed) {
				std::string text;
				for (uint8_t l : set) {
					text += std::to_string(l) + " ";
				}
				std::printf("\tmissed layers %s\n", text.c_str());
			}
			failures += s.misses > 0 ? 1 : 0;
			std::vector<std::pair<std::string, std::string>> scatter_failures;
			uint32_t scatter_meshes = 0;
			const bf3::render::SceneScatterPlan scatter_plan = bf3::render::plan_terrain_scatter(
					*mount, resolver->shaders(), name, [&](const std::string &) { return int32_t(scatter_meshes++); },
					scatter_failures);
			if (scatter_plan.types.empty() && scatter_failures.empty()) {
				continue;
			}
			bf3::render::TerrainScatter::Input scatter_input;
			scatter_input.plan = &scatter_plan;
			scatter_input.tree = &tree;
			scatter_input.heights = &heights;
			scatter_input.fetch = fetch;
			scatter_input.image = [&](const std::string &p_name) {
				return bf3::render::load_scatter_image(*mount, p_name);
			};
			const auto scatter_start = std::chrono::steady_clock::now();
			Result<std::shared_ptr<const bf3::render::TerrainScatter>> scatter =
					bf3::render::TerrainScatter::build(scatter_input, scatter_failures);
			if (!scatter) {
				std::printf("\tscatter: %s\n", scatter.error().message.c_str());
				++failures;
				continue;
			}
			std::string levels;
			for (uint8_t l : (*scatter)->levels()) {
				levels += std::to_string(l) + " ";
			}
			std::printf("\tscatter %zu types on %u meshes, %zu of %zu mask programs, cell cap %u, tree levels %s, %.1f "
						"MiB, "
						"%zu failures, %.2f s\n",
					scatter_plan.types.size(), scatter_meshes, (*scatter)->programs(), scatter_plan.combos.size(),
					scatter_plan.cell_instances, levels.c_str(), double((*scatter)->bytes()) / 1048576.0,
					scatter_failures.size(), seconds_since(scatter_start));
			for (const auto &[item, error] : scatter_failures) {
				std::printf("\t  [%s: %s]\n", item.c_str(), error.c_str());
			}
			failures += scatter_failures.empty() ? 0 : 1;
			if (list_scatter) {
				for (const bf3::render::ScatterType &type : (*scatter)->types()) {
					const res::TerrainScatterType &data = scatter_plan.types[type.source].data;
					std::string record;
					for (uint8_t b : data.record) {
						char hex[4];
						std::snprintf(hex, sizeof(hex), "%02x", unsigned(b));
						record += hex;
					}
					std::printf("\t  layer %u level %u cell %.0f m density %g candidates %u scale %g,%g to %g,%g %s "
								"record %s\n",
							unsigned(type.layer), unsigned(type.tree_level), double(type.cell), double(type.density),
							type.candidates, double(type.scale_min[0]), double(type.scale_min[1]),
							double(type.scale_max[0]), double(type.scale_max[1]), data.mesh.c_str(), record.c_str());
				}
			}
		}
	}
	return failures == 0 ? 0 : 1;
}

}
