// Bf3GameFx: the game's effects (docs/design/m2a-fx3.md §4). See game_fx.h.
#include "game_fx.h"

#include "fx_decals.h"
#include "fx_renderer.h"
#include "viewmodel_muzzle.h"

#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/material_grid.h"
#include "bf3/core/gameplay/weapon_blob.h"
#include "bf3/core/gameplay/weapon_catalog.h"
#include "bf3/core/install.h"
#include "bf3/fx/atlas_pixels.h"
#include "bf3/fx/effect_compiler.h"
#include "bf3/fx/emitter_atlas.h"
#include "bf3/fx/impact_table.h"
#include "bf3/host/gunplay.h"

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/core/object.hpp>

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <optional>

namespace bf3::godot_side {

using namespace godot;

namespace {

constexpr size_t kMaxStartEffectDefs = 256; // start_effect's compiled defs kept (beyond: the call compiles again)
constexpr size_t kMaxNotesErrors = 8;

std::string fmt(const char *p_format, ...) {
	char buf[1024];
	va_list args;
	va_start(args, p_format);
	std::vsnprintf(buf, sizeof(buf), p_format, args);
	va_end(args);
	return buf;
}

double since(std::chrono::steady_clock::time_point p_t0) {
	return std::chrono::duration<double>(std::chrono::steady_clock::now() - p_t0).count();
}

bool stopping(const M2aLoadInputs &p_in) {
	return p_in.stop != nullptr && p_in.stop->load(std::memory_order_relaxed);
}

} // namespace

struct Bf3GameFx::Impl {
	std::shared_ptr<const core::Install> install;
	std::unique_ptr<core::gameplay::WeaponSources> sources;
	std::optional<fx::EmitterAtlas> atlas;
	fx::AtlasPixels pixels; // released after the upload
	std::optional<fx::ImpactTable> impacts;
	std::unique_ptr<fx::EffectLibrary> library;
	std::shared_ptr<const fx::FireEffectTable> fire;
	std::unique_ptr<fx::FxWorld> world;
	std::unique_ptr<fx::FxTriggers> triggers;
	fx::FxTriggerConfig config;
	FxDecalAssets decal_assets;
	FxDecalPool decals;
	Bf3FxRenderer *renderer = nullptr;
	uint64_t renderer_id = 0;
	std::vector<std::string> catalogue; // the level's EffectBlueprint and EmitterDocument names (sorted)
	std::unique_ptr<core::EbxLibrary> wide; // @all-mp, opened by the first start_effect of a name outside the level
	bool wide_tried = false;
	std::map<std::string, std::shared_ptr<const fx::EffectDef>, std::less<>> local_defs; // start_effect's compiles
	std::vector<fx::MuzzleSample> muzzles;
	std::vector<fx::EffectSoundCue> effect_cues;
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

	// A def for start_effect: the level's blueprint through the library, an EmitterDocument wrapped, or the @all-mp
	// library's.
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
					notes.push_back(fmt("start_effect: @all-mp opened in %.2f s for %.*s", since(t0), int(p_name.size()), p_name.data()));
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
		const std::string_view type = part->primary().type_name();
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

core::Status Bf3GameFx::load(const M2aLoadInputs &p_in) {
	Impl &m = *impl_;
	const auto t0 = std::chrono::steady_clock::now();
	if (m.loaded) {
		return core::Error{ "Bf3GameFx::load: loaded already" };
	}
	if (p_in.db == nullptr || !p_in.db->install || !p_in.shared) {
		return core::Error{ "Bf3GameFx::load: no level database or shared level" };
	}
	m.install = p_in.db->install;
	const std::string &level_id = p_in.shared->level;
	const std::optional<core::Level> info = m.install->level(level_id);
	if (!info) {
		return core::Error{ "Bf3GameFx::load: unknown level " + level_id };
	}
	// The level's sources (its own EbxLibrary: the database's world is Phase B's and goes away after the load).
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
	// The emitter atlas and its pixels (the cache is valid here only).
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
				m.pixels.from_cache ? "from the cache" : "converted", since(ta), double(m.pixels.base.bytes.size()) / 1048576.0,
				double(m.pixels.normal.bytes.size()) / 1048576.0));
	}
	if (stopping(p_in)) {
		return core::Error{ "Bf3GameFx::load: stopped" };
	}
	// The impact table of the level's material grid.
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
	// The library: uncached (the cache pointer dies with the load; the preload below compiles in milliseconds).
	fx::EffectLibrary::Sources s;
	s.fx.db = &lib;
	s.fx.digest = core::gameplay::library_partition_digest(lib);
	s.cache = nullptr;
	s.atlas = &*m.atlas;
	s.impacts = &*m.impacts;
	m.library = std::make_unique<fx::EffectLibrary>(s);
	// The fire effect table over the match's defs.
	if (p_in.shared->gunplay) {
		core::Result<fx::FireEffectTable> fire = fx::load_fire_effects(lib, p_in.shared->gunplay->defs);
		if (!fire) {
			return core::Error{ "Bf3GameFx::load: fire effects: " + fire.error().message };
		}
		m.fire = std::make_shared<const fx::FireEffectTable>(std::move(*fire));
	} else {
		m.fire = std::make_shared<const fx::FireEffectTable>();
		m.notes.push_back("no gunplay level: no fire effects (impacts and start_effect only)");
	}
	// E8 (1): every blueprint the kits and the impact table name.
	{
		const auto tp = std::chrono::steady_clock::now();
		std::vector<std::string> names = m.fire->blueprints();
		for (std::string &n : fx::impact_effect_names(*m.impacts)) {
			names.push_back(std::move(n));
		}
		std::sort(names.begin(), names.end());
		names.erase(std::unique(names.begin(), names.end()), names.end());
		const size_t failed = m.library->preload(names, p_in.threads);
		m.stats.preloaded = uint32_t(names.size());
		m.stats.preload_failures = uint32_t(failed);
		m.notes.push_back(fmt("preload: %zu blueprints (%zu fire, %zu impact), %zu failed, %.3f s", names.size(), m.fire->blueprints().size(),
				fx::impact_effect_names(*m.impacts).size(), failed, since(tp)));
	}
	if (stopping(p_in)) {
		return core::Error{ "Bf3GameFx::load: stopped" };
	}
	// The decal templates and textures.
	{
		const auto td = std::chrono::steady_clock::now();
		const std::vector<std::string> names = fx::impact_decal_names(*m.impacts);
		m.decal_assets = FxDecalAssets::load(lib, lib.mount(0), names);
		m.stats.decal_templates = uint32_t(m.decal_assets.templates.size());
		m.stats.decal_templates_untextured = m.decal_assets.without_texture;
		m.notes.push_back(fmt("decals: %zu templates of %zu names, %u without a texture, %zu textures (%u magenta-keyed BC1 un-keyed; %.1f MiB), %.2f s",
				m.decal_assets.templates.size(), names.size(), m.decal_assets.without_texture, m.decal_assets.textures.size(), m.decal_assets.unkeyed,
				double(m.decal_assets.texture_bytes) / 1048576.0, since(td)));
		m.note_errors("decal", m.decal_assets.errors);
	}
	fx::FxWorldConfig wc;
	m.world = std::make_unique<fx::FxWorld>(wc);
	m.triggers = std::make_unique<fx::FxTriggers>(*m.world, *m.library, m.fire, m.config);
	m.stats.load_seconds = since(t0);
	m.loaded = true;
	return core::Status{};
}

bool Bf3GameFx::loaded() const {
	return impl_->loaded;
}

std::vector<std::string> Bf3GameFx::impact_sound_names() const {
	return impl_->impacts ? fx::impact_sound_names(*impl_->impacts) : std::vector<std::string>{};
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
	// E8 (6): every entry's rect inside its uploaded atlas texture.
	uint32_t uploaded = 0;
	for (const fx::AtlasEntry &e : m.atlas->entries()) {
		const bool inside = std::min(e.min_uv.x, e.max_uv.x) >= 0.0f && std::max(e.min_uv.x, e.max_uv.x) <= 1.0f && std::min(e.min_uv.y, e.max_uv.y) >= 0.0f &&
				std::max(e.min_uv.y, e.max_uv.y) <= 1.0f;
		uploaded += inside && m.renderer->stats().atlas_ok ? 1 : 0;
	}
	m.stats.atlas_entries = uint32_t(m.atlas->entries().size());
	m.stats.atlas_fallbacks = m.stats.atlas_entries - uploaded;
	m.pixels = fx::AtlasPixels{}; // the GPU has them now
	if (!m.decals.build(m.renderer, m.decal_assets, &err)) {
		return fail("Bf3GameFx::build: " + err);
	}
	m.stats.decal_textures = m.decals.stats().textures;
	m.built = true;
	return true;
}

bool Bf3GameFx::built() const {
	return impl_->built;
}

void Bf3GameFx::step(const M2aFrame &p_frame) {
	Impl &m = *impl_;
	if (!m.built) {
		return;
	}
	Bf3FxRenderer *renderer = m.live_renderer();
	if (renderer == nullptr) {
		return; // the parent freed it
	}
	++m.stats.frames;
	fx::FxTriggerConfig cfg = m.config;
	cfg.own_slot = p_frame.own_slot;
	cfg.match_seed = p_frame.frame != nullptr ? p_frame.frame->match_seed : 0;
	if (cfg.own_slot != m.config.own_slot || cfg.match_seed != m.config.match_seed) {
		m.config = cfg;
		m.triggers->set_config(cfg);
	}
	// The 1p muzzle is hidden with the viewmodel (free camera, own death): the own shots then play their 3p list.
	m.muzzles.clear();
	for (const fx::MuzzleSample &s : p_frame.muzzles) {
		if (s.first_person && (p_frame.free_camera || !p_frame.own_alive)) {
			++m.stats.muzzles_dropped;
			continue;
		}
		m.muzzles.push_back(s);
	}
	m.triggers->on_muzzles(m.muzzles);
	fx::FxView view;
	view.camera_position = p_frame.camera.trans;
	view.camera_forward = p_frame.camera.forward;
	m.triggers->on_events(p_frame.events, p_frame.clock, view);
	for (const fx::DecalCue &c : m.triggers->decals()) {
		m.decals.place(c);
	}
	m.decals.step(p_frame.dt);
	renderer->step_world(*m.world, p_frame.dt, view);
	renderer->draw(*m.world, view, *m.atlas);
	m.effect_cues.clear();
	fx::effect_sound_cues(m.world->events(), m.effect_cues);
	m.stats.effect_sounds += m.effect_cues.size();
	// The frame's numbers.
	m.stats.triggers = m.triggers->stats();
	m.stats.effects_live = m.world->stats().live_effects;
	m.stats.particles = uint32_t(m.world->particles().size());
	m.stats.drawn_instances = renderer->stats().drawn_instances;
	m.stats.lights = renderer->stats().lights;
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
	m.decals.clear();
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

} // namespace bf3::godot_side
