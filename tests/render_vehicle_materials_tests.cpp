#include <doctest/doctest.h>

#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/install_snapshot.h"
#include "bf3/core/util/text.h"
#include "bf3/host/level_load.h"
#include "bf3/host/level_vehicles.h"
#include "bf3/render/dxbc.h"
#include "bf3/render/look_materials.h"
#include "bf3/render/vehicle_meshes.h"
#include "test_support.h"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

using namespace bf3;

namespace {

constexpr std::string_view kTexturePrefix = "texture_";

const render::ShaderSolution *gbuffer_solution(const render::SurfaceShader &p_shader, render::dxbc::Program &r_pixel) {
	for (const render::ShaderSolution &s : p_shader.solutions) {
		if (!s.pixel_program || !s.vertex_program) {
			continue;
		}
		core::Result<render::dxbc::Program> p = render::dxbc::parse(*s.pixel_program);
		if (!p) {
			continue;
		}
		const bool albedo = std::any_of(p->outputs.begin(), p->outputs.end(),
				[](const render::dxbc::SignatureElement &p_e) { return p_e.name == "SV_Target" && p_e.reg == 1; });
		const bool samples = std::any_of(p->code.begin(), p->code.end(),
				[](const render::dxbc::Instruction &p_i) { return p_i.is(render::dxbc::Op::Sample); });
		if (albedo && samples) {
			r_pixel = std::move(*p);
			return &s;
		}
	}
	return nullptr;
}

struct SlotInputs {
	std::vector<std::string> textures;
	size_t constants = 0;
};

SlotInputs solution_inputs(const render::SurfaceShader &p_shader, const render::ShaderSolution &p_solution,
		const render::dxbc::Program &p_pixel, const render::MaterialSlot &p_slot) {
	SlotInputs out;
	for (const render::dxbc::Binding &b : p_pixel.bindings) {
		if (b.type != uint32_t(render::dxbc::BindingType::Texture)) {
			continue;
		}
		std::string name;
		bool engine = false;
		if (p_solution.pixel_constants) {
			for (const auto &[slot, texture] : p_solution.pixel_constants->textures) {
				if ((slot & render::kTextureSlotRegister) == b.bind_point) {
					name = core::to_lower_ascii(texture);
					engine = true;
				}
			}
		}
		if (!engine && b.name.rfind(kTexturePrefix, 0) == 0) {
			const std::string parameter = b.name.substr(kTexturePrefix.size());
			if (p_shader.parameter(parameter) == nullptr) {
				continue;
			}
			name = core::to_lower_ascii(p_slot.texture(parameter));
			if (name.empty()) {
				name = "(unset " + parameter + ")";
			}
		}
		if (!engine && name.empty()) {
			continue;
		}
		out.textures.push_back(name);
	}
	for (const render::dxbc::ConstantBuffer &cb : p_pixel.constant_buffers) {
		if (cb.name == "externalConstants") {
			out.constants += cb.variables.size();
		}
	}
	return out;
}

}

TEST_SUITE("render-vehicle-materials") {
	TEST_CASE("vehicle materials: every MP_007 Conquest vehicle slot with a G-buffer program draws its colour through "
			  "that program or its one colour texture, and every texture it binds is loaded") {
		core::InstallReader reader;
		if (!reader.locate({})) {
			test::skip_install_case(reader.describe().c_str());
			return;
		}
		const core::InstallSnapshot before = reader.snapshot();
		core::Result<std::shared_ptr<core::Install>> install = core::Install::open(reader);
		REQUIRE(install.ok());
		core::Result<host::LevelDatabase> db =
				host::open_level_database(*install, "MP_007", "ConquestLarge0", 0, nullptr);
		REQUIRE_MESSAGE(db.ok(), (db.ok() ? "" : db.error().message));
		core::Result<std::shared_ptr<const host::LevelVehicles>> level =
				host::load_level_vehicles(*db, sim::VehicleTuning{});
		REQUIRE_MESSAGE(level.ok(), (level.ok() ? "" : level.error().message));
		std::vector<render::VehicleMeshSpec> specs;
		for (const host::LevelVehicleKind &k : (*level)->kinds) {
			specs.push_back(render::VehicleMeshSpec{ k.blueprint, k.mesh, k.component_meshes });
		}
		for (const host::LevelVehicleKind &k : (*level)->kinds) {
			const std::optional<core::gameplay::VehicleWreckDef> &w = k.damage_visuals.wreck;
			if (w && !w->mesh.empty() &&
					std::none_of(specs.begin(), specs.end(),
							[&](const render::VehicleMeshSpec &p_s) { return p_s.blueprint == w->blueprint; })) {
				specs.push_back(render::VehicleMeshSpec{ w->blueprint, w->mesh, {} });
			}
		}
		core::Result<std::unique_ptr<render::VehicleMeshSet>> set =
				render::load_vehicle_meshes(db->level->db(), db->level->mount(), specs);
		REQUIRE_MESSAGE(set.ok(), (set.ok() ? "" : set.error().message));
		std::unique_ptr<render::LookResolver> resolver = render::LookResolver::open(db->level->mount());
		REQUIRE(resolver != nullptr);
		const auto loaded = [&](const std::string &p_name) { return (*set)->texture(p_name) != nullptr; };
		size_t slots = 0, gbuffer = 0, drawn_by_program = 0, unloaded = 0, inputs = 0, bound = 0, constants = 0;
		size_t constants_bound = 0, alpha_tested = 0, alpha_cut = 0;
		std::set<std::string> zero_parameters;
		for (const render::VehicleMeshKind &k : (*set)->kinds) {
			std::vector<std::set<uint32_t>> used(k.materials.size());
			for (size_t p = 0; p < k.parts.size(); ++p) {
				for (const render::PreparedSurface &s : k.parts[p].prepared.surfaces) {
					used[k.part_mesh[p]].insert(s.material_index);
				}
			}
			size_t kind_inputs = 0, kind_bound = 0;
			for (size_t m = 0; m < k.materials.size(); ++m) {
				for (uint32_t s : used[m]) {
					if (s >= k.materials[m].slots.size() || s >= k.looks[m].size()) {
						continue;
					}
					const render::MaterialSlot &slot = k.materials[m].slots[s];
					const render::LookMaterial &look = k.looks[m][s];
					if (!look.drawn) {
						continue;
					}
					++slots;
					const std::string shader_name =
							core::to_lower_ascii(slot.variation_shader.empty() ? slot.shader : slot.variation_shader);
					const render::SurfaceShader *shader = resolver->shaders().by_name(shader_name);
					render::dxbc::Program pixel;
					const render::ShaderSolution *solution =
							shader != nullptr ? gbuffer_solution(*shader, pixel) : nullptr;
					if (solution == nullptr || look.blend == render::LookBlend::Blend) {
						continue;
					}
					++gbuffer;
					const SlotInputs in = solution_inputs(*shader, *solution, pixel, slot);
					std::set<std::string> drawn;
					if (look.graph) {
						drawn.insert(look.graph_textures.begin(), look.graph_textures.end());
						constants_bound += look.graph_constants.size();
					}
					if (look.color) {
						drawn.insert(look.color->name);
					}
					if (look.normal) {
						drawn.insert(look.normal->name);
					}
					const bool through_program = look.graph != nullptr || look.graph_plain;
					drawn_by_program += through_program ? 1u : 0u;
					std::string missing, zero;
					size_t slot_bound = 0;
					for (const std::string &t : in.textures) {
						const bool unset = t.rfind("(unset ", 0) == 0;
						const bool here = unset ? look.graph != nullptr : drawn.count(t) != 0 && loaded(t);
						slot_bound += here ? 1u : 0u;
						if (!here) {
							missing += " " + t;
						} else if (unset) {
							zero += " " + t.substr(7, t.size() - 8);
							zero_parameters.insert(t.substr(7, t.size() - 8));
						}
					}
					if (!zero.empty()) {
						const std::array<float, 4> *given = slot.vector("FLIRData");
						const render::ShaderValue *fallback = shader->value("FLIRData");
						const float weight = given != nullptr ? (*given)[3]
								: fallback != nullptr		  ? fallback->value[3]
															  : -1.0f;
						CHECK_MESSAGE(weight == 0.0f, k.blueprint, " slot ", s,
								": the zero-bound HeatMap is weighted by FLIRData.w ", weight);
					}
					if (look.graph && look.blend == render::LookBlend::AlphaTest) {
						++alpha_tested;
						const bool cut = look.graph->translated.code.find("discard") != std::string::npos;
						alpha_cut += cut ? 1u : 0u;
						CHECK_MESSAGE(cut, k.blueprint, " slot ", s, " ", shader_name,
								" is alpha-tested and its program cuts nothing");
					}
					for (const std::string &t : drawn) {
						unloaded += !t.empty() && !loaded(t) ? 1u : 0u;
					}
					kind_inputs += in.textures.size();
					kind_bound += slot_bound;
					constants += in.constants;
					std::printf("vehicle materials: %s mesh %zu slot %u %s: %s, textures %zu of %zu bound%s%s%s%s\n",
							k.blueprint.c_str(), m, s, shader_name.c_str(),
							look.graph				   ? "its program"
									: look.graph_plain ? "its one colour texture"
													   : "NOT its program",
							slot_bound, in.textures.size(), zero.empty() ? "" : ", zero for the unset", zero.c_str(),
							missing.empty() ? "" : ", not bound:", missing.c_str());
					CHECK_MESSAGE(through_program, k.blueprint, " slot ", s, " ", shader_name);
				}
			}
			inputs += kind_inputs;
			bound += kind_bound;
			std::printf("vehicle materials: %s: %zu of %zu shader texture inputs bound\n", k.blueprint.c_str(),
					kind_bound, kind_inputs);
		}
		std::printf("vehicle materials: %zu drawn slots, %zu with a G-buffer program, %zu drawn through it or its one "
					"colour texture; %zu of %zu shader texture inputs bound; %zu program constants given of the "
					"programs' %zu external constants; %zu bound textures not loaded\n",
				slots, gbuffer, drawn_by_program, bound, inputs, constants_bound, constants, unloaded);
		std::string zeros;
		for (const std::string &z : zero_parameters) {
			zeros += " " + z;
		}
		std::printf(
				"vehicle materials: %zu of %zu alpha-tested program slots cut their texels; unset parameters bound to "
				"zero:%s\n",
				alpha_cut, alpha_tested, zeros.c_str());
		CHECK(gbuffer > 0);
		CHECK(drawn_by_program == gbuffer);
		CHECK(unloaded == 0);
		CHECK(zero_parameters == std::set<std::string>{ "HeatMap" });
		const core::SnapshotDiff diff = core::diff_snapshots(before, reader.snapshot());
		CHECK(diff.clean());
	}
}
