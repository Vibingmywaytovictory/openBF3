#include "game_fx.h"

#include "fx_decals.h"
#include "fx_meshes.h"
#include "fx_renderer.h"
#include "godot_util.h"
#include "viewmodel_muzzle.h"

#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/material_grid.h"
#include "bf3/core/gameplay/weapon_blob.h"
#include "bf3/core/gameplay/weapon_catalog.h"
#include "bf3/core/install.h"
#include "bf3/core/level/placed_effects.h"
#include "bf3/core/util/elapsed.h"
#include "bf3/core/util/format.h"
#include "bf3/fx/atlas_pixels.h"
#include "bf3/fx/effect_compiler.h"
#include "bf3/fx/emitter_atlas.h"
#include "bf3/fx/impact_table.h"
#include "bf3/host/gunplay.h"
#include "bf3/host/level_vehicles.h"
#include "bf3/render/effect_meshes.h"
#include "bf3/sim/logic/conquest_data.h"

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <optional>
#include <set>
#include <tuple>

namespace bf3::godot_side {

using namespace godot;

namespace {

constexpr size_t kMaxStartEffectDefs = 256;
constexpr size_t kMaxNotesErrors = 8;

using bf3::core::fmt;
using bf3::core::seconds_since;

bool stopping(const GameLoadInputs &p_in) {
	return p_in.stop != nullptr && p_in.stop->load(std::memory_order_relaxed);
}

}

struct Bf3GameFx::Impl {
	std::shared_ptr<const core::Install> install;
	std::unique_ptr<core::gameplay::WeaponSources> sources;
	std::optional<fx::EmitterAtlas> atlas;
	fx::AtlasPixels pixels;
	std::optional<fx::ImpactTable> impacts;
	std::unique_ptr<fx::EffectLibrary> library;
	std::vector<std::string> preloaded;
	std::shared_ptr<const fx::FireEffectTable> fire;
	std::vector<std::tuple<uint16_t, uint32_t, uint16_t>> accessory_effects;
	std::unique_ptr<fx::FxWorld> world;
	std::vector<host::PresentEvent> events;
	std::unique_ptr<fx::FxTriggers> triggers;
	fx::PlacedEffects placed;
	FxMeshes meshes;
	std::vector<std::optional<sim::logic::PlacedLogicKey>> placed_keys;
	const core::gameplay::LogicGraph *logic_graph = nullptr;
	uint64_t logic_generation = 0;
	uint64_t logic_cursor = 0;
	fx::FxTriggerConfig config;
	FxDecalAssets decal_assets;
	FxDecalPool decals;
	Bf3FxRenderer *renderer = nullptr;
	uint64_t renderer_id = 0;
	std::vector<std::string> catalogue;
	std::unique_ptr<core::EbxLibrary> wide;
	bool wide_tried = false;
	std::map<std::string, std::shared_ptr<const fx::EffectDef>, std::less<>> local_defs;
	std::vector<fx::MuzzleSample> muzzles;
	std::vector<fx::EffectSoundCue> effect_cues;
	struct ImpactLit {
		fx::EffectHandle handle;
		uint8_t slot = 0;
		const fx::EffectDef *def = nullptr;
		bool light_ended = false;
	};
	std::vector<ImpactLit> impact_lit;
	std::map<std::pair<const fx::EffectDef *, uint8_t>, fx::EffectHandle> impact_latest;
	std::vector<fx::FxExclusionVolume> exclusion;
	std::vector<std::string> notes;
	GameFxStats stats;
	bool loaded = false;
	bool built = false;
	uint64_t start_seed = 0x5EEDF003ull;

	Bf3FxRenderer *live_renderer() const {
		return renderer != nullptr ? Object::cast_to<Bf3FxRenderer>(ObjectDB::get_instance(renderer_id)) : nullptr;
	}

	void note_errors(const std::string &p_what, const std::vector<std::string> &p_errors) {
		for (size_t i = 0; i < p_errors.size() && i < kMaxNotesErrors; ++i) {
			notes.push_back(p_what + ": " + p_errors[i]);
		}
		if (p_errors.size() > kMaxNotesErrors) {
			notes.push_back(p_what + fmt(": %zu more", p_errors.size() - kMaxNotesErrors));
		}
	}

	core::Result<std::shared_ptr<const fx::EffectDef>> resolve(std::string_view p_name) {
		if (const auto it = local_defs.find(p_name); it != local_defs.end()) {
			return it->second;
		}
		std::string full;
		const core::EbxResolver *db = sources->level.get();
		if (std::binary_search(catalogue.begin(), catalogue.end(), p_name)) {
			full = std::string(p_name);
		} else if (core::Result<std::string> found = fx::find_partition(catalogue, p_name)) {
			full = std::move(*found);
		} else {
			if (p_name.find('/') == std::string_view::npos) {
				return found.error();
			}
			if (!wide_tried) {
				wide_tried = true;
				const auto t0 = std::chrono::steady_clock::now();
				core::Result<std::unique_ptr<core::EbxLibrary>> w = core::EbxLibrary::open(install, core::expand_superbundles(*install, "@all-mp"));
				if (w) {
					wide = std::move(*w);
					notes.push_back(fmt("start_effect: @all-mp opened in %.2f s for %.*s", seconds_since(t0),
							int(p_name.size()), p_name.data()));
				} else {
					notes.push_back("start_effect: @all-mp: " + w.error().message);
				}
			}
			if (!wide || wide->partition_by_name(p_name) == nullptr) {
				return core::Error{ "start_effect: " + std::string(p_name) + " is neither in the level nor in @all-mp" };
			}
			full = std::string(p_name);
			db = wide.get();
		}
		const core::EbxPartition *part = db->partition_by_name(full);
		if (part == nullptr) {
			return core::Error{ "start_effect: " + full + " does not parse" };
		}

		const std::string_view type = part->primary().type_name() == "EffectBlueprint" || part->instances_of("EmitterDocument").empty()
				? part->primary().type_name()
				: std::string_view("EmitterDocument");
		std::shared_ptr<const fx::EffectDef> def;
		if (type == "EffectBlueprint" && db == sources->level.get()) {
			core::Result<std::shared_ptr<const fx::EffectDef>> d = library->get(full);
			if (!d) {
				return d.error();
			}
			def = *d;
		} else if (type == "EffectBlueprint" || type == "EmitterDocument") {
			fx::CompileContext ctx;
			ctx.db = db;
			core::Result<fx::EffectDef> d = type == "EffectBlueprint" ? fx::compile_effect(full, ctx) : fx::compile_emitter_effect(full, ctx);
			if (!d) {
				return d.error();
			}
			def = std::make_shared<const fx::EffectDef>(std::move(*d));
		} else {
			return core::Error{ "start_effect: " + full + " is a " + std::string(type) + ", not an EffectBlueprint or EmitterDocument" };
		}
		if (local_defs.size() < kMaxStartEffectDefs) {
			local_defs.emplace(std::string(p_name), def);
		}
		return def;
	}
};

Bf3GameFx::Bf3GameFx() :
		impl_(std::make_unique<Impl>()) {}

Bf3GameFx::~Bf3GameFx() {
	shutdown();
}

core::Status Bf3GameFx::load(const GameLoadInputs &p_in) {
	Impl &m = *impl_;
	const auto t0 = std::chrono::steady_clock::now();
	if (m.loaded) {
		return core::Error{ "Bf3GameFx::load: loaded already" };
	}

	m.triggers.reset();
	m.placed.forget();
	m.placed_keys.clear();
	m.logic_graph = nullptr;
	m.world.reset();
	m.local_defs.clear();
	m.library.reset();
	m.preloaded.clear();
	m.fire.reset();
	m.impacts.reset();
	m.pixels = {};
	m.atlas.reset();
	m.decal_assets = {};
	m.wide.reset();
	m.sources.reset();
	m.catalogue.clear();
	m.wide_tried = false;
	m.muzzles.clear();
	m.effect_cues.clear();
	m.notes.clear();
	m.stats = {};
	if (p_in.db == nullptr || !p_in.db->install || !p_in.shared) {
		return core::Error{ "Bf3GameFx::load: no level database or shared level" };
	}
	m.install = p_in.db->install;
	const std::string &level_id = p_in.shared->level;
	const std::optional<core::Level> info = m.install->level(level_id);
	if (!info) {
		return core::Error{ "Bf3GameFx::load: unknown level " + level_id };
	}

	core::Result<core::gameplay::WeaponSources> src = core::gameplay::open_weapon_sources(m.install, info->superbundle);
	if (!src) {
		return core::Error{ "Bf3GameFx::load: " + level_id + ": " + src.error().message };
	}
	m.sources = std::make_unique<core::gameplay::WeaponSources>(std::move(*src));
	core::EbxLibrary &lib = *m.sources->level;
	const core::EbxLibrary::LoadReport report = lib.load_all();
	m.notes.push_back(fmt("%s: %zu partitions parsed in %.2f s", level_id.c_str(), lib.catalog_size(), report.parse_seconds + report.insert_seconds));
	if (stopping(p_in)) {
		return core::Error{ "Bf3GameFx::load: stopped" };
	}
	for (const core::EbxLibrary::Entry &e : lib.entries()) {
		if (e.partition == nullptr) {
			continue;
		}
		const std::string_view type = e.partition->primary().type_name();
		if (type == "EffectBlueprint" || type == "EmitterDocument") {
			m.catalogue.emplace_back(e.name);
		}
	}
	std::sort(m.catalogue.begin(), m.catalogue.end());

	{
		const auto ta = std::chrono::steady_clock::now();
		core::gameplay::WorkBudget budget{ fx::kDefaultCompileWorkLimit };
		core::Result<fx::EmitterAtlas> atlas = fx::EmitterAtlas::load(lib, lib.mount(0), fx::EmitterAtlas::emitter_system_of(level_id), budget);
		if (!atlas) {
			return core::Error{ "Bf3GameFx::load: atlas: " + atlas.error().message };
		}
		m.atlas.emplace(std::move(*atlas));
		core::gameplay::WorkBudget pixel_budget{ fx::kDefaultCompileWorkLimit };
		core::Result<fx::AtlasPixels> pixels = fx::prepare_atlas_pixels(*m.atlas, lib.mount(0), p_in.cache, pixel_budget);
		if (!pixels) {
			return core::Error{ "Bf3GameFx::load: atlas pixels: " + pixels.error().message };
		}
		m.pixels = std::move(*pixels);
		m.notes.push_back(fmt("atlas: %zu entries, pixels %s in %.2f s (%.1f + %.1f MiB)", m.atlas->entries().size(),
				m.pixels.from_cache ? "from the cache" : "converted", seconds_since(ta),
				double(m.pixels.base.bytes.size()) / 1048576.0, double(m.pixels.normal.bytes.size()) / 1048576.0));
	}
	if (stopping(p_in)) {
		return core::Error{ "Bf3GameFx::load: stopped" };
	}

	{
		const std::optional<std::string> grid = core::gameplay::MaterialGrid::grid_of_level(lib, m.sources->level_partition);
		if (!grid) {
			return core::Error{ "Bf3GameFx::load: no material grid for " + m.sources->level_partition };
		}
		core::gameplay::WorkBudget budget{ fx::kDefaultCompileWorkLimit };
		core::Result<fx::ImpactTable> impacts = fx::ImpactTable::load(lib, *grid, budget);
		if (!impacts) {
			return core::Error{ "Bf3GameFx::load: impact table: " + impacts.error().message };
		}
		m.impacts.emplace(std::move(*impacts));
	}

	fx::EffectLibrary::Sources s;
	s.fx.db = &lib;
	s.fx.digest = core::gameplay::library_partition_digest(lib);
	s.cache = nullptr;
	s.atlas = &*m.atlas;
	s.impacts = &*m.impacts;
	m.library = std::make_unique<fx::EffectLibrary>(s);

	if (p_in.shared->gunplay) {
		std::vector<core::gameplay::WeaponDef> fired = p_in.shared->gunplay->defs;
		if (p_in.shared->vehicles != nullptr) {
			fired.insert(fired.end(), p_in.shared->vehicles->weapons.begin(), p_in.shared->vehicles->weapons.end());
		}
		core::Result<fx::FireEffectTable> fire = fx::load_fire_effects(lib, fired);
		if (!fire) {
			return core::Error{ "Bf3GameFx::load: fire effects: " + fire.error().message };
		}
		m.accessory_effects.clear();
		const host::GunplayLevel &g = *p_in.shared->gunplay;
		std::vector<uint16_t> done;
		for (const auto &team : g.loadouts.kits) {
			for (const host::LoadoutKit &kit : team) {
				for (const host::LoadoutItem &item : kit.slots[size_t(core::gameplay::WeaponSlot::Primary)]) {
					if (std::find(done.begin(), done.end(), item.def) != done.end()) {
						continue;
					}
					done.push_back(item.def);
					const size_t first = fire->variants.size();
					if (const core::Status st = fx::add_unlock_fire_effects(*fire, lib, item.def, item.unlock); !st) {
						m.notes.push_back("accessory fire effects " + st.error().message);
						continue;
					}
					for (size_t v = first; v < fire->variants.size(); ++v) {
						for (size_t a = 0; a < host::kLoadoutAccessories; ++a) {
							for (size_t i = 0; i < item.accessory_unlocks[a].size() && i < item.accessories[a].size();
									++i) {
								if (item.accessory_unlocks[a][i] == fire->variants[v].unlock) {
									m.accessory_effects.emplace_back(
											item.def, item.accessories[a][i], fire->variants[v].index);
								}
							}
						}
					}
				}
			}
		}
		m.notes.push_back(fmt("accessory fire effects: %zu variants, %zu (def, accessory) pairs", fire->variants.size(),
				m.accessory_effects.size()));
		m.fire = std::make_shared<const fx::FireEffectTable>(std::move(*fire));
	} else {
		m.fire = std::make_shared<const fx::FireEffectTable>();
		m.notes.push_back("no gunplay level: no fire effects (impacts and start_effect only)");
	}

	{
		const auto tp = std::chrono::steady_clock::now();
		std::vector<std::string> names = m.fire->blueprints();
		for (std::string &n : fx::impact_effect_names(*m.impacts)) {
			names.push_back(std::move(n));
		}
		const size_t before_vehicles = names.size();
		if (p_in.shared->vehicles) {
			for (const host::LevelVehicleKind &k : p_in.shared->vehicles->kinds) {
				if (!k.explosion_effect.empty()) {
					names.push_back(k.explosion_effect);
				}
				for (const core::gameplay::VehicleDamageEffectDef &d : k.damage_visuals.damage) {
					names.push_back(d.effect);
				}
				if (k.damage_visuals.wreck) {
					for (const core::gameplay::VehicleWreckEffectDef &w : k.damage_visuals.wreck->effects) {
						names.push_back(w.effect);
					}
				}
			}
		}
		const size_t vehicle_names = names.size() - before_vehicles;
		std::vector<core::level::PlacedEffect> placed;
		if (p_in.db->world != nullptr) {
			core::Result<std::vector<core::level::PlacedEffect>> read =
					core::level::read_placed_effects(*p_in.db->world);
			if (read) {
				placed = std::move(*read);
			} else {
				m.notes.push_back("placed effects: " + read.error().message);
			}
		}
		for (const core::level::PlacedEffect &e : placed) {
			names.push_back(e.blueprint);
		}
		m.exclusion.clear();
		if (p_in.db->world != nullptr) {
			core::Result<std::vector<core::level::EmitterExclusionVolume>> volumes =
					core::level::read_emitter_exclusion_volumes(*p_in.db->world);
			if (!volumes) {
				m.notes.push_back("emitter exclusion volumes: " + volumes.error().message);
			} else {
				for (const core::level::EmitterExclusionVolume &v : *volumes) {
					fx::FxExclusionVolume f;
					f.transform.right = v.transform.right;
					f.transform.up = v.transform.up;
					f.transform.forward = v.transform.forward;
					f.transform.trans = v.transform.trans;
					f.half_extents = v.half_extents;
					m.exclusion.push_back(f);
				}
				m.notes.push_back(fmt("emitter exclusion volumes: %zu", m.exclusion.size()));
			}
		}
		std::sort(names.begin(), names.end());
		names.erase(std::unique(names.begin(), names.end()), names.end());
		const size_t failed = m.library->preload(names, p_in.threads);
		m.preloaded = names;
		m.stats.preloaded = uint32_t(names.size());
		m.stats.preload_failures = uint32_t(failed);
		m.notes.push_back(fmt("preload: %zu blueprints (%zu fire, %zu impact, %zu vehicle), %zu failed, %.3f s",
				names.size(), m.fire->blueprints().size(), fx::impact_effect_names(*m.impacts).size(), vehicle_names,
				failed, seconds_since(tp)));
		{
			const auto tm = std::chrono::steady_clock::now();
			std::vector<std::string> mesh_names;
			std::vector<std::shared_ptr<const fx::EffectDef>> defs;
			for (const std::string &n : names) {
				if (core::Result<std::shared_ptr<const fx::EffectDef>> def = m.library->get(n)) {
					fx::collect_mesh_names(**def, mesh_names);
					defs.push_back(*def);
				}
			}
			m.meshes.load(render::load_effect_meshes(lib, lib.mount(0), mesh_names, p_in.cache));
			m.stats.effect_meshes = uint32_t(mesh_names.size()) - m.meshes.stats().failed;
			m.stats.effect_meshes_failed = m.meshes.stats().failed;
			m.notes.push_back(fmt("effect meshes: %u of %zu loaded, %.2f s", m.stats.effect_meshes, mesh_names.size(),
					seconds_since(tm)));
			m.note_errors("effect mesh", m.meshes.notes());
			std::array<uint32_t, fx::kEmittableTypeCount> kinds{};
			uint32_t mesh_loaded = 0, lights = 0;
			for (const std::shared_ptr<const fx::EffectDef> &def : defs) {
				for (const fx::EmitterDef &e : def->emitters) {
					++kinds[size_t(e.tmpl.emittable) % kinds.size()];
					mesh_loaded += e.tmpl.emittable == fx::EmittableType::ParticleMesh && m.meshes.loaded(e.tmpl.mesh)
							? 1u
							: 0u;
					lights += (e.tmpl.flags & fx::kEmitterActAsPointLight) != 0 ? 1u : 0u;
				}
			}
			std::string census =
					fmt("effects census: %zu of %zu blueprints compiled, emitters", defs.size(), names.size());
			for (size_t k = 0; k < kinds.size(); ++k) {
				census += fmt(" %.*s %u", int(fx::emittable_type_name(fx::EmittableType(k)).size()),
						fx::emittable_type_name(fx::EmittableType(k)).data(), kinds[k]);
			}
			census += fmt(
					", %u ParticleMesh emitters with their mesh loaded, %u point-light emitters", mesh_loaded, lights);
			m.notes.push_back(census);
		}
		std::map<std::string, std::shared_ptr<const fx::EffectDef>, std::less<>> drawable;
		uint32_t undrawn = 0;
		for (const core::level::PlacedEffect &e : placed) {
			auto found = drawable.find(e.blueprint);
			if (found == drawable.end()) {
				std::shared_ptr<const fx::EffectDef> d;
				if (core::Result<std::shared_ptr<const fx::EffectDef>> def = m.library->get(e.blueprint)) {
					d = *def;
				} else {
					m.notes.push_back("placed effect " + e.blueprint + ": " + def.error().message);
				}
				found = drawable.emplace(e.blueprint, std::move(d)).first;
			}
			if (found->second != nullptr) {
				std::vector<std::string> meshes;
				fx::collect_mesh_names(*found->second, meshes);
				for (const std::string &mesh : meshes) {
					undrawn += m.meshes.loaded(mesh) ? 0u : 1u;
				}
			}
			fx::Transform at;
			at.right = e.transform.right;
			at.up = e.transform.up;
			at.forward = e.transform.forward;
			at.trans = e.transform.trans;
			m.placed.add(found->second, at, e.auto_start);
			m.placed_keys.push_back(sim::logic::placed_logic_key(*p_in.db->world, e.object));
		}
		m.placed.set_undrawn_mesh_emitters(undrawn);
		const fx::PlacedEffectStats &ps = m.placed.stats();
		m.notes.push_back(fmt("placed effects: %u placed (%zu blueprints), %u start at load, %u without a blueprint, "
							  "%u mesh emitters not drawn",
				ps.placed, drawable.size(), ps.auto_start, ps.without_def, ps.mesh_emitters_undrawn));
	}
	if (stopping(p_in)) {
		return core::Error{ "Bf3GameFx::load: stopped" };
	}

	{
		const auto td = std::chrono::steady_clock::now();
		const std::vector<std::string> names = fx::impact_decal_names(*m.impacts);
		m.decal_assets = FxDecalAssets::load(lib, lib.mount(0), names);
		m.stats.decal_templates = uint32_t(m.decal_assets.templates.size());
		m.stats.decal_templates_untextured = m.decal_assets.without_texture;
		m.notes.push_back(fmt("decals: %zu templates of %zu names, %u without a texture, %zu textures (%u "
							  "magenta-keyed un-keyed, %u refused; %.1f MiB), %.2f s",
				m.decal_assets.templates.size(), names.size(), m.decal_assets.without_texture,
				m.decal_assets.textures.size(), m.decal_assets.unkeyed, m.decal_assets.refused,
				double(m.decal_assets.texture_bytes) / 1048576.0, seconds_since(td)));
		m.note_errors("decal", m.decal_assets.errors);
	}
	fx::FxWorldConfig wc;
	m.world = std::make_unique<fx::FxWorld>(wc);
	m.triggers = std::make_unique<fx::FxTriggers>(*m.world, *m.library, m.fire, m.config);
	m.stats.load_seconds = seconds_since(t0);
	m.loaded = true;
	return core::Status{};
}

bool Bf3GameFx::loaded() const {
	return impl_->loaded;
}

std::vector<std::string> Bf3GameFx::impact_sound_names() const {
	const Impl &m = *impl_;
	if (!m.impacts) {
		return {};
	}
	return m.library ? fx::impact_sound_names(*m.impacts, *m.library, m.preloaded) : fx::impact_sound_names(*m.impacts);
}

bool Bf3GameFx::build(Node3D *p_parent, std::string *r_error) {
	Impl &m = *impl_;
	auto fail = [r_error](std::string p_error) {
		if (r_error != nullptr) {
			*r_error = std::move(p_error);
		}
		return false;
	};
	if (!m.loaded) {
		return fail("Bf3GameFx::build: not loaded");
	}
	if (m.built) {
		return true;
	}
	if (p_parent == nullptr) {
		return fail("Bf3GameFx::build: no parent");
	}
	m.renderer = memnew(Bf3FxRenderer);
	m.renderer->set_name("GameFx");
	m.renderer_id = m.renderer->get_instance_id();
	p_parent->add_child(m.renderer);
	std::string err;
	if (!m.renderer->upload_atlas(m.pixels, &err)) {
		return fail("Bf3GameFx::build: atlas: " + err);
	}

	uint32_t uploaded = 0;
	for (const fx::AtlasEntry &e : m.atlas->entries()) {
		const bool inside = std::min(e.min_uv.x, e.max_uv.x) >= 0.0f && std::max(e.min_uv.x, e.max_uv.x) <= 1.0f && std::min(e.min_uv.y, e.max_uv.y) >= 0.0f &&
				std::max(e.min_uv.y, e.max_uv.y) <= 1.0f;
		uploaded += inside && m.renderer->stats().atlas_ok ? 1 : 0;
	}
	m.stats.atlas_entries = uint32_t(m.atlas->entries().size());
	m.stats.atlas_fallbacks = m.stats.atlas_entries - uploaded;
	m.pixels = fx::AtlasPixels{};
	if (!m.decals.build(m.renderer, m.decal_assets, &err)) {
		return fail("Bf3GameFx::build: " + err);
	}
	if (m.renderer->get_world_3d().is_null() || !m.meshes.build(m.renderer->get_world_3d()->get_scenario(), &err)) {
		return fail("Bf3GameFx::build: " + (err.empty() ? std::string("effect meshes: no world") : err));
	}
	m.renderer->set_meshes(&m.meshes);
	m.renderer->set_exclusion_volumes(m.exclusion);
	m.stats.decal_textures = m.decals.stats().textures;
	m.built = true;
	return true;
}

bool Bf3GameFx::built() const {
	return impl_->built;
}

void Bf3GameFx::step(const GameFrame &p_frame) {
	Impl &m = *impl_;
	if (!m.built) {
		return;
	}
	Bf3FxRenderer *renderer = m.live_renderer();
	if (renderer == nullptr) {
		return;
	}
	++m.stats.frames;
	fx::FxTriggerConfig cfg = m.config;
	cfg.own_slot = p_frame.own_slot;
	cfg.match_seed = p_frame.frame != nullptr ? p_frame.frame->match_seed : 0;
	if (cfg.own_slot != m.config.own_slot || cfg.match_seed != m.config.match_seed) {
		m.config = cfg;
		m.triggers->set_config(cfg);
	}

	m.muzzles.clear();
	for (const fx::MuzzleSample &s : p_frame.muzzles) {
		if (s.first_person && (p_frame.free_camera || !p_frame.own_alive)) {
			++m.stats.muzzles_dropped;
			continue;
		}
		m.muzzles.push_back(s);
	}
	if (p_frame.frame != nullptr) {
		for (const net::SoldierGear &gear : p_frame.frame->gear) {
			uint16_t effects = gear.def;
			for (const auto &[def, accessory, index] : m.accessory_effects) {
				if (def == gear.def && accessory != 0 &&
						std::find(gear.accessories.begin(), gear.accessories.end(), accessory) !=
								gear.accessories.end()) {
					effects = index;
				}
			}
			m.triggers->set_slot_effects(gear.slot, gear.def, effects);
		}
	}
	m.triggers->on_muzzles(m.muzzles);
	fx::FxView view;
	view.camera_position = p_frame.camera.trans;
	view.camera_forward = p_frame.camera.forward;
	m.events.assign(p_frame.events.begin(), p_frame.events.end());
	if (p_frame.world != nullptr && m.library->impacts() != nullptr) {
		fx::resolve_detonation_surfaces(m.events, *m.library->impacts(), *p_frame.world);
	}
	m.triggers->on_events(m.events, p_frame.clock, view);
	for (const fx::TriggerStart &s : m.triggers->starts()) {
		if (s.started && s.purpose == fx::TriggerPurpose::ImpactEffect && s.def &&
				std::any_of(s.def->emitters.begin(), s.def->emitters.end(),
						[](const fx::EmitterDef &e) { return (e.tmpl.flags & fx::kEmitterActAsPointLight) != 0; })) {
			m.impact_lit.push_back(Impl::ImpactLit{ s.handle, s.slot, s.def.get() });
			m.impact_latest[{ s.def.get(), s.slot }] = s.handle;
			++m.stats.impact_light_starts;
		}
	}
	for (const fx::DecalCue &c : m.triggers->decals()) {
		m.decals.place(c);
	}
	m.placed.step(*m.world, view, m.config.tier, m.start_seed);
	m.stats.placed = m.placed.stats();
	m.decals.step(p_frame.dt);
	renderer->step_world(*m.world, p_frame.dt, view);
	renderer->draw(*m.world, view, *m.atlas);
	m.effect_cues.clear();
	fx::effect_sound_cues(m.world->events(), m.effect_cues);
	m.stats.effect_sounds += m.effect_cues.size();

	m.stats.triggers = m.triggers->stats();
	m.stats.effects_live = m.world->stats().live_effects;
	m.stats.particles = uint32_t(m.world->particles().size());
	m.stats.drawn_instances = renderer->stats().drawn_instances;
	m.stats.mesh_particles = m.meshes.stats().drawn;
	m.stats.mesh_particles_undrawn = m.meshes.stats().unknown;
	m.stats.lights = renderer->stats().lights;
	{
		const fx::RenderPrepStats &ps = renderer->prep().stats();
		m.stats.light_candidates_max = std::max(m.stats.light_candidates_max, ps.light_candidates);
		m.stats.lights_max = std::max(m.stats.lights_max, ps.lights);
		m.stats.light_frames_over += ps.light_candidates > ps.lights ? 1u : 0u;
		m.stats.lights_dropped = ps.lights_dropped;
		m.stats.light_particles_max = std::max(m.stats.light_particles_max, m.world->stats().live_lights);
		std::erase_if(m.impact_lit, [&m](const Impl::ImpactLit &p_i) { return !m.world->alive(p_i.handle); });
		const std::span<const fx::ParticleLight> plights = m.world->lights();
		const std::span<const fx::FxLight> shown = renderer->prep().lights();
		fx::audit_fx_lights(*m.world, shown, m.stats.light_audit);
		uint32_t lit = 0, lit_emitters = 0, shown_impact = 0;
		std::map<std::pair<uint32_t, const fx::EffectDef *>, std::set<uint32_t>> lit_by_owner;
		const std::span<const fx::EmitterOutput> outputs = m.world->outputs();
		for (size_t i = 0; i < outputs.size(); ++i) {
			const fx::EmitterOutput &o = outputs[i];
			const auto impact = std::find_if(m.impact_lit.begin(), m.impact_lit.end(),
					[&o](const Impl::ImpactLit &p_i) { return p_i.handle == o.effect; });
			if (impact == m.impact_lit.end() || o.def == nullptr || o.emitter >= o.def->emitters.size()) {
				continue;
			}
			const bool lit_emitter = (o.def->emitters[o.emitter].tmpl.flags & fx::kEmitterActAsPointLight) != 0;
			if (lit_emitter && o.light_count == 0 && o.particle_count > 0 && !impact->light_ended) {
				impact->light_ended = true;
				const auto latest = m.impact_latest.find({ impact->def, impact->slot });
				const bool next_hit = latest != m.impact_latest.end() && !(latest->second == impact->handle);
				++(next_hit ? m.stats.impact_lights_ended_by_next_hit : m.stats.impact_lights_cut);
			}
			if (o.light_count == 0) {
				continue;
			}
			++lit;
			lit_by_owner[{ o.owner, o.def }].insert(o.effect.slot);
			float sum = 0;
			for (uint32_t k = 0; k < o.light_count; ++k) {
				sum += std::max(0.0f, plights[o.first_light + k].intensity);
			}
			if (sum <= 0) {
				continue;
			}
			++lit_emitters;
			for (const fx::FxLight &f : shown) {
				shown_impact += f.output == i && f.effect == o.effect && f.emitter == o.emitter ? 1u : 0u;
			}
		}
		m.stats.impact_over_max_frames += std::any_of(lit_by_owner.begin(), lit_by_owner.end(),
				[](const auto &p_group) { return p_group.second.size() > p_group.first.second->max_instance_count; });
		m.stats.impact_light_mismatch_frames += shown_impact != lit_emitters ? 1u : 0u;
		m.stats.impact_lights_max = std::max(m.stats.impact_lights_max, lit);
		m.stats.impact_light_frames += lit > 0 ? 1u : 0u;
	}
	m.stats.texture_fallbacks = uint32_t(renderer->prep().stats().unresolved_textures);
	const FxDecalStats &ds = m.decals.stats();
	m.stats.decals_live = ds.live;
	m.stats.decals_placed = ds.placed;
	m.stats.decals_replaced = ds.replaced;
	m.stats.decals_no_template = ds.no_template;
	m.stats.decals_untextured = ds.untextured;
}

std::span<const fx::ImpactSoundCue> Bf3GameFx::sound_cues() const {
	return impl_->built ? impl_->triggers->sounds() : std::span<const fx::ImpactSoundCue>{};
}

std::span<const fx::EffectSoundCue> Bf3GameFx::effect_sound_cues() const {
	return impl_->effect_cues;
}

fx::MuzzleSample Bf3GameFx::own_muzzle(const Bf3Viewmodel &p_viewmodel, uint16_t p_def, uint8_t p_slot, const fx::Vec3 &p_velocity) const {
	const Impl &m = *impl_;
	fx::MuzzleSample none;
	none.slot = p_slot;
	none.first_person = true;
	if (!m.loaded || !m.fire || p_def >= m.fire->defs.size() || m.fire->defs[p_def].first_person.empty()) {
		return none;
	}
	const fx::FireEffect &entry = m.fire->defs[p_def].first_person.front();
	return muzzle_sample(viewmodel_muzzle_transform(p_viewmodel, to_godot_transform(entry.offset)), p_slot, p_velocity);
}

void Bf3GameFx::set_first_person_projection(float p_fov_override, float p_z_clip_scale) {
	if (Bf3FxRenderer *r = impl_->live_renderer()) {
		r->set_first_person_projection(p_fov_override, p_z_clip_scale);
	}
}

void Bf3GameFx::logic_effects(const core::gameplay::LogicGraph &p_graph, const host::LogicEffectFeed &p_feed) {
	Impl &m = *impl_;
	if (!m.built) {
		return;
	}
	if (m.logic_graph != &p_graph) {
		std::vector<uint32_t> nodes(m.placed_keys.size(), fx::kNoPlacedLogicNode);
		for (size_t i = 0; i < m.placed_keys.size(); ++i) {
			const std::optional<uint32_t> n = m.placed_keys[i]
					? sim::logic::logic_node_of_key(p_graph, *m.placed_keys[i], core::gameplay::LogicTypeId::Effect)
					: std::nullopt;
			nodes[i] = n.value_or(fx::kNoPlacedLogicNode);
		}
		m.placed.bind_logic(nodes);
		m.logic_graph = &p_graph;
		m.logic_generation = p_feed.generation();
		m.logic_cursor = 0;
	}
	if (p_feed.generation() != m.logic_generation) {
		m.placed.logic_reset();
		m.logic_generation = p_feed.generation();
	}
	for (const host::LogicEffectEvent &e : p_feed.since(m.logic_cursor)) {
		m.placed.logic_event(e.node, e.start);
	}
	m.logic_cursor = p_feed.end();
}

core::Result<fx::EffectHandle> Bf3GameFx::start_effect(std::string_view p_blueprint, const fx::Transform &p_transform, float p_start_age) {
	Impl &m = *impl_;
	if (!m.loaded) {
		return core::Error{ "start_effect: not loaded" };
	}
	++m.stats.start_effect_calls;
	core::Result<std::shared_ptr<const fx::EffectDef>> def = m.resolve(p_blueprint);
	if (!def) {
		++m.stats.start_effect_failures;
		return def.error();
	}
	fx::EffectStart s;
	s.transform = p_transform;
	s.seed = sim::weapons::splitmix64(m.start_seed++);
	s.tier = m.config.tier;
	s.start_age = p_start_age;
	core::Result<fx::EffectHandle> h = m.world->start(*def, s);
	if (!h) {
		++m.stats.start_effect_failures;
	}
	return h;
}

void Bf3GameFx::stop_effect(fx::EffectHandle p_effect) {
	if (impl_->world) {
		impl_->world->stop(p_effect);
	}
}

void Bf3GameFx::move_effect(fx::EffectHandle p_effect, const fx::Transform &p_transform, const fx::Vec3 &p_velocity) {
	if (impl_->world) {
		impl_->world->set_transform(p_effect, p_transform, p_velocity);
	}
}

const fx::FxWorld *Bf3GameFx::world() const {
	return impl_->world.get();
}

const fx::ImpactTable *Bf3GameFx::impacts() const {
	return impl_->impacts ? &*impl_->impacts : nullptr;
}

const fx::FireEffectTable *Bf3GameFx::fire_effects() const {
	return impl_->fire.get();
}

const Bf3FxRenderer *Bf3GameFx::renderer() const {
	return impl_->live_renderer();
}

const fx::FxTriggers *Bf3GameFx::triggers() const {
	return impl_->triggers.get();
}

std::vector<std::string> Bf3GameFx::notes() const {
	return impl_->notes;
}

std::vector<std::string> Bf3GameFx::decal_texture_report(const std::string &p_dump_dir) const {
	return impl_->decals.texture_report(p_dump_dir);
}

GameFxStats Bf3GameFx::stats() const {
	return impl_->stats;
}

void Bf3GameFx::shutdown() {
	Impl &m = *impl_;
	if (m.built && m.placed.stats().placed > 0) {
		const fx::PlacedEffectStats &ps = m.placed.stats();
		UtilityFunctions::print(to_godot(fmt("[game] placed effects: %u placed, %u start at load, %u of them started, "
											 "%u in range, %u running, %u ended in "
											 "range, %llu starts, %llu stops, %llu start failures, %u without a "
											 "blueprint, %u mesh emitters not drawn, %u bound to the client logic, "
											 "%llu logic starts, %llu logic stops, %u started by logic",
				ps.placed, ps.auto_start, ps.started_placements, ps.in_range, ps.running, ps.ended_in_range,
				(unsigned long long)ps.starts, (unsigned long long)ps.stops, (unsigned long long)ps.start_failures,
				ps.without_def, ps.mesh_emitters_undrawn, ps.logic_bound, (unsigned long long)ps.logic_starts,
				(unsigned long long)ps.logic_stops, ps.logic_started_placements)));
	}
	if (m.built) {
		const GameFxStats &st = m.stats;
		UtilityFunctions::print(to_godot(fmt("[game] effect lights: %llu frames, at most %u light particles, %u lights "
											 "asked for and %u shown, %llu frames over the pool, %llu dropped; mesh "
											 "particles %llu drawn, %llu without a mesh; %llu effect instances "
											 "reset by MaxInstanceCount, %llu refused, %llu particle spawns dropped; "
											 "impacts %llu started, %llu culled, %llu with a light, at most %u impact "
											 "lights in %llu frames, %llu lit effects cut short, %llu impact lights "
											 "ended by their shooter's next hit, %llu frames with a shooter's lit "
											 "impacts over MaxInstanceCount, %llu frames where the shown impact lights "
											 "differ from the lit impact emitters, %llu of %llu particle lights off "
											 "their curve, %llu of %llu effect lights not at their particles' mean",
				(unsigned long long)st.frames, st.light_particles_max, st.light_candidates_max, st.lights_max,
				(unsigned long long)st.light_frames_over, (unsigned long long)st.lights_dropped,
				(unsigned long long)m.meshes.stats().drawn_total, (unsigned long long)m.meshes.stats().unknown_total,
				(unsigned long long)m.world->stats().stolen_instances,
				(unsigned long long)m.world->stats().refused_starts,
				(unsigned long long)m.world->stats().spawns_dropped, (unsigned long long)st.triggers.impacts,
				(unsigned long long)st.triggers.impacts_culled, (unsigned long long)st.impact_light_starts,
				st.impact_lights_max, (unsigned long long)st.impact_light_frames,
				(unsigned long long)(m.world->stats().lit_instances_killed + st.impact_lights_cut),
				(unsigned long long)st.impact_lights_ended_by_next_hit, (unsigned long long)st.impact_over_max_frames,
				(unsigned long long)st.impact_light_mismatch_frames, (unsigned long long)st.light_audit.off_curve,
				(unsigned long long)st.light_audit.particle_lights, (unsigned long long)st.light_audit.off_mean,
				(unsigned long long)st.light_audit.lit_emitters)));
	}
	if (m.built) {
		if (Bf3FxRenderer *r = m.live_renderer()) {
			const fx::RenderPrepStats &ps = r->prep().stats();
			std::string by_type;
			uint64_t undrawn = 0, no_drawer = 0;
			for (size_t t = 0; t < fx::kEmittableTypeCount; ++t) {
				const uint64_t shown = ps.instances_by_type[t] + ps.excluded_by_type[t];
				undrawn += ps.particles_by_type[t] > shown ? ps.particles_by_type[t] - shown : 0u;
				no_drawer += fx::fx_emittable_drawn(fx::EmittableType(t)) ? 0u : ps.particles_by_type[t];
				if (ps.particles_by_type[t] > 0) {
					by_type += fmt("%s%s %llu drawn and %llu excluded of %llu", by_type.empty() ? "" : ", ",
							std::string(fx::emittable_type_name(fx::EmittableType(t))).c_str(),
							(unsigned long long)ps.instances_by_type[t], (unsigned long long)ps.excluded_by_type[t],
							(unsigned long long)ps.particles_by_type[t]);
				}
			}
			UtilityFunctions::print(
					to_godot(fmt("[game] effect particles by type: %s; %llu particles not drawn, %llu of "
								 "a type without a drawer; mesh particles %llu drawn of %llu instances",
							by_type.c_str(), (unsigned long long)undrawn, (unsigned long long)no_drawer,
							(unsigned long long)m.meshes.stats().drawn_total,
							(unsigned long long)ps.instances_by_type[size_t(fx::EmittableType::ParticleMesh)])));
		}
	}
	m.placed.forget();
	m.decals.clear();
	if (Bf3FxRenderer *r = m.live_renderer()) {
		r->set_meshes(nullptr);
	}
	m.meshes.clear();
	m.impact_lit.clear();
	m.impact_latest.clear();
	if (Bf3FxRenderer *r = m.live_renderer()) {
		r->clear();
		if (r->is_inside_tree()) {
			r->queue_free();
		} else {
			memdelete(r);
		}
	}
	m.renderer = nullptr;
	m.renderer_id = 0;
	m.built = false;
	m.triggers.reset();
	m.world.reset();
	m.local_defs.clear();
	m.library.reset();
	m.wide.reset();
	m.sources.reset();
	m.loaded = false;
}

}
