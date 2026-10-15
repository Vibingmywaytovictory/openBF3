// bf3dump looks <LEVEL> <MODE> (L1 look; README "L1 look"): the drawn-material census, M1 against L1.
#include "look_commands.h"

#include "bf3/core/level/census.h"
#include "bf3/core/level/level.h"
#include "bf3/core/level/world.h"
#include "bf3/render/level_scene.h"
#include "bf3/render/mesh_cache.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
#include <set>

using namespace bf3::core;
using namespace bf3::core::level;
namespace render = bf3::render;

namespace bf3dump {

namespace {

double seconds_since(std::chrono::steady_clock::time_point p_start) {
	return std::chrono::duration<double>(std::chrono::steady_clock::now() - p_start).count();
}

double percent(uint64_t p_part, uint64_t p_whole) {
	return p_whole == 0 ? 100.0 : 100.0 * double(p_part) / double(p_whole);
}

const char *uv_name(uint8_t p_uv) {
	switch (p_uv) {
		case render::kLookUv0:
			return "uv0";
		case render::kLookUv1:
			return "uv1";
		case render::kLookUvOverlay:
			return "uv2";
		case render::kLookUvProjected:
			return "proj";
	}
	return "?";
}

struct ShaderRow {
	std::set<uint32_t> materials_l1;
	uint64_t slots = 0; // drawn instance slots (instances x drawn LOD0 slots)
	uint64_t m1_albedo = 0, l1_albedo = 0, l1_glass = 0;
	uint64_t normal_available = 0, m1_normal = 0, l1_normal = 0;
	uint64_t l1_undrawn = 0;
	bool in_db = false;
	size_t graph_textures = 0;
	std::map<std::string, uint64_t> parameters; // parameter name -> drawn instance slots naming it
	std::map<std::string, uint64_t> tiers; // "graph uv1 x4" -> instance slots
};

} // namespace

int cmd_looks(const std::shared_ptr<Install> &p_install, const std::vector<std::string> &p_args) {
	if (p_args.size() < 2) {
		std::fprintf(stderr, "usage: looks <LEVEL> <MODE> [--materials] [--failures]\n");
		return 2;
	}
	const std::string level_name = p_args[0], mode = p_args[1];
	const bool list_materials = std::find(p_args.begin(), p_args.end(), "--materials") != p_args.end();
	std::vector<std::string> shader_dumps; // --shader NAME (repeatable): the graph's constants, parameters and values
	for (size_t i = 2; i + 1 < p_args.size(); ++i) {
		if (p_args[i] == "--shader") {
			shader_dumps.push_back(p_args[i + 1]);
		}
	}
	const auto start = std::chrono::steady_clock::now();
	Result<std::unique_ptr<LoadedLevel>> loaded = load_level(p_install, level_name, mode);
	if (!loaded) {
		std::fprintf(stderr, "looks: %s\n", loaded.error().message.c_str());
		return 1;
	}
	const LoadedLevel &level = **loaded;
	Result<std::unique_ptr<World>> world = World::traverse(level);
	if (!world) {
		std::fprintf(stderr, "looks: traverse: %s\n", world.error().message.c_str());
		return 1;
	}
	Result<MeshCensus> census = mesh_census(level, **world);
	if (!census) {
		std::fprintf(stderr, "looks: census: %s\n", census.error().message.c_str());
		return 1;
	}
	const double seconds_load = seconds_since(start);
	render::ScenePlanOptions m1_options;
	m1_options.look = render::SceneLook::M1;
	Result<render::LevelScenePlan> m1 = render::plan_level_scene(level, **world, *census, m1_options);
	Result<render::LevelScenePlan> l1 = render::plan_level_scene(level, **world, *census);
	if (!m1 || !l1) {
		std::fprintf(stderr, "looks: plan: %s\n", (!m1 ? m1.error() : l1.error()).message.c_str());
		return 1;
	}
	if (m1->meshes.size() != l1->meshes.size()) {
		std::fprintf(stderr, "looks: M1 and L1 plans differ in meshes (%zu, %zu)\n", m1->meshes.size(), l1->meshes.size());
		return 1;
	}
	// The slots each request's LOD 0 draws (subsets that are not z-only), from the res header alone.
	std::vector<std::optional<std::set<uint32_t>>> drawn_slots(l1->requests.size());
	size_t heads_failed = 0;
	for (size_t r = 0; r < l1->requests.size(); ++r) {
		Result<render::MeshHead> head = render::read_mesh_head(level.mount(), l1->requests[r].res);
		if (!head || head->mesh.lods.empty()) {
			++heads_failed;
			continue;
		}
		std::set<uint32_t> slots;
		for (const res::MeshSubset &s : head->mesh.lods[0].subsets) {
			if (!s.is_z_only() && s.primitive_count > 0) {
				slots.insert(s.material_index);
			}
		}
		drawn_slots[r] = std::move(slots);
	}

	std::map<std::string, ShaderRow> rows;
	uint64_t l1_glass_untextured = 0, color_available = 0, l1_albedo_named = 0;
	uint64_t total = 0, m1_albedo = 0, l1_albedo = 0, l1_glass = 0, normal_available = 0, m1_normal = 0, l1_normal = 0, l1_undrawn = 0;
	uint64_t instances = 0, instances_all_albedo_m1 = 0, instances_all_albedo_l1 = 0;
	std::map<std::string, uint64_t> tier_totals;
	for (size_t m = 0; m < l1->meshes.size(); ++m) {
		const render::SceneMeshPlan &lm = l1->meshes[m];
		const render::SceneMeshPlan &mm = m1->meshes[m];
		const uint64_t weight = lm.instances;
		std::set<uint32_t> slots;
		if (lm.request < drawn_slots.size() && drawn_slots[lm.request]) {
			slots = *drawn_slots[lm.request];
		} else {
			for (uint32_t s = 0; s < lm.surface_materials.size(); ++s) {
				slots.insert(s);
			}
		}
		bool all_m1 = true, all_l1 = true, any = false;
		for (uint32_t s : slots) {
			if (s >= lm.surface_materials.size() || s >= mm.surface_materials.size() || mm.surface_materials[s] == render::kSceneUndrawnMaterial) {
				continue; // a slot past the asset's list, or one M1 never drew (undrawn shader words)
			}
			any = true;
			const render::SceneMaterialPlan &mmat = m1->materials[mm.surface_materials[s]];
			ShaderRow &row = rows[mmat.slot.variation_shader.empty() ? mmat.slot.shader : mmat.slot.variation_shader];
			row.slots += weight;
			total += weight;
			const bool m1_has = mmat.albedo >= 0;
			row.m1_albedo += m1_has ? weight : 0;
			m1_albedo += m1_has ? weight : 0;
			all_m1 = all_m1 && m1_has;
			for (const auto &[name, texture] : mmat.slot.textures) {
				row.parameters[name] += weight;
			}
			if (lm.surface_materials[s] == render::kSceneUndrawnMaterial) {
				row.l1_undrawn += weight;
				l1_undrawn += weight;
				continue;
			}
			const uint32_t lid = lm.surface_materials[s];
			const render::SceneMaterialPlan &lmat = l1->materials[lid];
			row.materials_l1.insert(lid);
			const bool l1_has = lmat.albedo >= 0;
			row.l1_albedo += l1_has ? weight : 0;
			l1_albedo += l1_has ? weight : 0;
			row.l1_glass += lmat.glass ? weight : 0;
			l1_glass_untextured += lmat.glass && !l1_has ? weight : 0;
			const bool color_named = lmat.look && lmat.look->color_candidates > 0;
			color_available += color_named ? weight : 0;
			l1_albedo_named += color_named && l1_has ? weight : 0;
			l1_glass += lmat.glass ? weight : 0;
			all_l1 = all_l1 && l1_has;
			if (lmat.look) {
				row.in_db = row.in_db || lmat.look->shader_in_database;
				const bool available = lmat.look->normal_candidates > 0;
				row.normal_available += available ? weight : 0;
				normal_available += available ? weight : 0;
				if (available) {
					row.m1_normal += mmat.normal >= 0 ? weight : 0;
					m1_normal += mmat.normal >= 0 ? weight : 0;
					row.l1_normal += lmat.normal >= 0 ? weight : 0;
					l1_normal += lmat.normal >= 0 ? weight : 0;
				}
				char tier[96];
				if (lmat.look->color) {
					std::snprintf(tier, sizeof(tier), "%s %s x%g", render::look_tier_name(lmat.look->color->tier), uv_name(lmat.look->color->channel),
							double(lmat.look->color->scale));
				} else {
					std::snprintf(tier, sizeof(tier), "%s", lmat.glass ? "glass" : "none");
				}
				row.tiers[tier] += weight;
				tier_totals[tier] += weight;
			}
		}
		if (any) {
			instances += weight;
			instances_all_albedo_m1 += all_m1 ? weight : 0;
			instances_all_albedo_l1 += all_l1 ? weight : 0;
		}
	}
	const render::LookStats &ls = l1->look_stats;
	std::printf("looks %s %s: load + traverse + census %.3f s; plan M1 %.3f s, L1 %.3f s (shader database %.3f s, resolve %.3f s)\n", level_name.c_str(),
			mode.c_str(), seconds_load, m1->seconds, l1->seconds, ls.seconds_database, ls.seconds_resolve);
	std::printf("shader databases %zu, surface shaders %zu, parse work %llu units; texture headers read %zu (%zu failed); mesh heads failed %zu\n",
			ls.shader_databases, ls.shaders, (unsigned long long)ls.shader_work, ls.texture_headers, ls.texture_headers_failed, heads_failed);
	std::printf("resolver work %llu units; graph colour/normal candidates: max %zu per graph (cap %zu), %zu dropped\n", (unsigned long long)ls.resolve_work,
			ls.graph_candidates_max, render::kLookMaxGraphCandidates, ls.graph_candidates_dropped);
	std::printf("plan M1: %zu materials, %zu textures; plan L1: %zu materials, %zu textures\n", m1->materials.size(), m1->textures.size(),
			l1->materials.size(), l1->textures.size());
	for (const auto &[item, error] : l1->failures) {
		std::printf("failure %s: %s\n", item.c_str(), error.c_str());
	}
	std::printf("# shader\tin_db\tl1_materials\tinstance_slots\tm1_albedo%%\tl1_albedo%%\tglass\tnormal_in_bf3\tm1_normal\tl1_normal\tl1_undrawn\tl1_colour_from\t"
				"parameters\n");
	std::vector<std::pair<uint64_t, std::string>> order;
	for (const auto &[name, row] : rows) {
		order.emplace_back(row.slots, name);
	}
	std::sort(order.rbegin(), order.rend());
	for (const auto &[slots, name] : order) {
		const ShaderRow &row = rows[name];
		std::string params, tiers;
		for (const auto &[p, n] : row.parameters) {
			params += (params.empty() ? "" : ",") + p;
		}
		for (const auto &[t, n] : row.tiers) {
			tiers += (tiers.empty() ? "" : "; ") + t + " " + std::to_string(n);
		}
		std::printf("%s\t%d\t%zu\t%llu\t%.1f\t%.1f\t%llu\t%llu\t%llu\t%llu\t%llu\t%s\t%s\n", name.empty() ? "(none)" : name.c_str(), row.in_db ? 1 : 0,
				row.materials_l1.size(), (unsigned long long)row.slots, percent(row.m1_albedo, row.slots), percent(row.l1_albedo, row.slots),
				(unsigned long long)row.l1_glass, (unsigned long long)row.normal_available, (unsigned long long)row.m1_normal, (unsigned long long)row.l1_normal,
				(unsigned long long)row.l1_undrawn, tiers.c_str(), params.empty() ? "-" : params.c_str());
	}
	std::printf("colour source (drawn instance slots):");
	for (const auto &[t, n] : tier_totals) {
		std::printf(" [%s] %llu", t.c_str(), (unsigned long long)n);
	}
	std::printf("\n");
	if (list_materials) {
		std::printf("# material\tshader\tvariation_shader\tblend\tsides\tcolour\tnormal\n");
		for (size_t i = 0; i < l1->materials.size(); ++i) {
			const render::SceneMaterialPlan &mat = l1->materials[i];
			const auto tex = [](const std::optional<render::LookTexture> &p_t) {
				if (!p_t) {
					return std::string("-");
				}
				char buf[64];
				std::snprintf(buf, sizeof(buf), " (%s %s x%g)", render::look_tier_name(p_t->tier), uv_name(p_t->channel), double(p_t->scale));
				return p_t->name + buf;
			};
			std::printf("%zu\t%s\t%s\t%s\t%s\t%s\t%s\n", i, mat.slot.shader.c_str(), mat.slot.variation_shader.empty() ? "-" : mat.slot.variation_shader.c_str(),
					mat.glass ? "glass" : mat.blend ? "blend" : mat.alpha_test ? "alpha-test" : "opaque", mat.double_sided ? "2" : "1",
					mat.look ? tex(mat.look->color).c_str() : "-", mat.look ? tex(mat.look->normal).c_str() : "-");
		}
	}
	{
		const render::LookEnvironment &env = l1->look_environment;
		const auto v3 = [](const render::LookVec3 &p_v) {
			char buf[96];
			std::snprintf(buf, sizeof(buf), "(%.3f %.3f %.3f)", double(p_v[0]), double(p_v[1]), double(p_v[2]));
			return std::string(buf);
		};
		const auto frame_line = [&](const char *p_label, const render::LookFrame &p_f) {
			std::printf("  %s: sun %s x %.2f dir %s; ambient up %s horizon %s down %s; sky x%.2f horizon %s; fog %s %.0f..%.0f m density %.3f "
						"curve %.2f colour %s; middle grey %.3f exposure %.2f (%.2f..%.2f); bloom %.3f; lut %d\n",
					p_label, v3(p_f.sun_color).c_str(), double(p_f.sun_energy), v3(p_f.sun_direction).c_str(), v3(p_f.ambient_up).c_str(),
					v3(p_f.ambient_horizon).c_str(), v3(p_f.ambient_down).c_str(), double(p_f.sky_brightness), v3(p_f.horizon).c_str(), p_f.fog ? "on" : "off",
					double(p_f.fog_begin), double(p_f.fog_end), double(p_f.fog_density), double(p_f.fog_curve), v3(p_f.fog_color).c_str(), double(p_f.middle_gray), double(p_f.exposure),
					double(p_f.min_exposure), double(p_f.max_exposure), double(p_f.bloom), p_f.lut);
		};
		std::printf("environment: from data %d, %zu areas, %zu LUTs; base %s\n", env.from_data ? 1 : 0, env.areas.size(), env.luts.size(),
				env.base.source.c_str());
		std::printf("  sky panorama %s alpha %s gradient %s (plan textures %d %d %d)\n", env.base.panorama.c_str(), env.base.panorama_alpha.c_str(),
				env.base.gradient.c_str(), l1->sky_panorama, l1->sky_panorama_alpha, l1->sky_gradient);
		frame_line("base", render::look_frame_of(env.base));
		for (const render::LookArea &a : env.areas) {
			std::printf("  area %s priority %lld %s fade %.1f m: %s\n", a.name.c_str(), (long long)a.priority, a.box ? "box" : "polygon",
					double(a.fade), a.state.source.c_str());
			if (a.box) {
				std::printf("    box centre %s half %s\n", v3(a.box_center).c_str(), v3(a.box_half).c_str());
			} else {
				std::printf("    polygon %zu points, floor %.2f height %.2f\n", a.outline.size(), double(a.floor), double(a.height));
			}
		}
		for (size_t i = 0; i < env.luts.size(); ++i) {
			std::string ramp;
			for (size_t g : { size_t(0), size_t(4), size_t(8), size_t(16), size_t(24), size_t(31) }) {
				const size_t n = render::kGradeLutSide, at = ((g * n + g) * n + g) * 4;
				char buf[64];
				std::snprintf(buf, sizeof(buf), " %zu:(%u %u %u)", g, env.luts[i][at], env.luts[i][at + 1], env.luts[i][at + 2]);
				ramp += buf;
			}
			std::printf("  lut %zu grey ramp (texel: rgb):%s\n", i, ramp.c_str());
		}
		for (const std::string &n : env.notes) {
			std::printf("  note: %s\n", n.c_str());
		}
		// The look at the L1 viewpoints (README "L1 look").
		const std::pair<const char *, render::LookVec3> points[] = { { "spawn (street)", { -196.14f, 61.47f, -568.25f } },
			{ "station", { -167.76f, 66.2f, -475.19f } }, { "park", { -12.0f, 80.0f, 51.0f } }, { "overview", { -150.0f, 260.0f, -800.0f } } };
		for (const auto &[label, at] : points) {
			std::string weights;
			for (const render::LookArea &a : env.areas) {
				char buf[128];
				std::snprintf(buf, sizeof(buf), " %s=%.2f", a.name.substr(a.name.rfind('/') + 1).c_str(), double(render::look_area_weight(a, at)));
				weights += buf;
			}
			std::printf("  at %s %s:%s\n", label, v3(at).c_str(), weights.c_str());
			frame_line(label, render::look_frame(env, at));
		}
	}
	if (!shader_dumps.empty()) {
		std::unique_ptr<render::LookResolver> resolver = render::LookResolver::open(level.mount());
		for (const std::string &name : shader_dumps) {
			const render::SurfaceShader *s = resolver->shaders().by_name(name);
			if (s == nullptr) {
				std::printf("shader %s: not in the database\n", name.c_str());
				continue;
			}
			std::printf("shader %s: surface type %u flags %u, %zu solutions\n", name.c_str(), unsigned(s->surface_type), unsigned(s->flags), s->solutions.size());
			for (const render::ShaderTexture &t : s->textures) {
				std::printf("  texture %s coord %u usage %u factor %g\n", t.name.c_str(), t.coord_type, t.vertex_usage, double(t.factor));
			}
			for (const render::ShaderTextureParameter &t : s->texture_parameters) {
				std::printf("  parameter %s coord %u usage %u factor %g\n", t.name.c_str(), t.coord_type, t.vertex_usage, double(t.factor));
			}
			for (const std::string &t : s->sampled_textures) {
				std::printf("  sampled %s\n", t.c_str());
			}
			for (const render::ShaderValue &v : s->values) {
				std::printf("  value %s (%g, %g, %g, %g)\n", v.name.c_str(), double(v.value[0]), double(v.value[1]), double(v.value[2]), double(v.value[3]));
			}
		}
	}
	const uint64_t drawn = total - l1_undrawn;
	std::printf("drawn instance slots %llu (M1 drawn %llu; L1 leaves %llu undrawn: mask-only transparent overlays)\n", (unsigned long long)drawn,
			(unsigned long long)total, (unsigned long long)l1_undrawn);
	std::printf("m1.albedo %.2f %% (%llu of %llu); m1.normal %.2f %% (%llu of %llu where BF3 gives a normal map)\n", percent(m1_albedo, total),
			(unsigned long long)m1_albedo, (unsigned long long)total, percent(m1_normal, normal_available), (unsigned long long)m1_normal,
			(unsigned long long)normal_available);
	std::printf("l1.albedo %.2f %% (%llu of %llu drawn; %llu glass slots, %llu of them untextured tints); l1.normal %.2f %% (%llu of %llu where BF3 gives "
				"a normal map)\n",
			percent(l1_albedo, drawn), (unsigned long long)l1_albedo, (unsigned long long)drawn, (unsigned long long)l1_glass,
			(unsigned long long)l1_glass_untextured, percent(l1_normal, normal_available), (unsigned long long)l1_normal, (unsigned long long)normal_available);
	std::printf("l1.albedo where BF3 names a colour texture %.2f %% (%llu of %llu); drawn slots whose graph names no colour texture %llu (constant "
				"colours compiled into the shader)\n",
			percent(l1_albedo_named, color_available), (unsigned long long)l1_albedo_named, (unsigned long long)color_available,
			(unsigned long long)(drawn - color_available));
	std::printf("instances with every drawn slot coloured: M1 %.2f %%, L1 %.2f %% (of %llu)\n", percent(instances_all_albedo_m1, instances),
			percent(instances_all_albedo_l1, instances), (unsigned long long)instances);
	const bool pass = percent(l1_albedo, drawn) >= 95.0;
	std::printf("looks gate l1.albedo >= 95 %%: %s\n", pass ? "PASS" : "FAIL");
	return pass ? 0 : 1;
}

} // namespace bf3dump
