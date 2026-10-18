#pragma once
// Bf3GameFx: the game's effects (docs/design/m2a.md §12.1, §12.2, E8, E9; owner fx3; W3.0 contract,
// docs/design/m2a-w3.md). Owns the FxWorld, the EffectLibrary (with the level's impact table and atlas), the fire
// effect table, the FxTriggers, fx2's Bf3FxRenderer (light pool and 1p pass are fx2's: render_prep.h,
// Bf3FxRenderer::step_world / draw / set_first_person_projection) and the decal pool (fx_decals, <= fx::kMaxDecals
// Godot Decals, oldest replaced, faded after fx::kDecalFadeSeconds). A plain class owned by the level session (gx).
//   load         loader thread, no Godot API: library over the level, ImpactTable, load_fire_effects over the
//                GunplayLevel defs, atlas pixels; preloads every blueprint the kits and the impact table name
//                (E8 (1): 0 failures)
//   build        main thread: the renderer under p_parent, atlas upload (E8 (6): 52/52, 0 fallbacks), decal pool
//   step         main thread, every display frame (m2a_game.h order: after the crowd, before the audio): triggers
//                (on_muzzles, on_events), FxWorld step on the Godot pool, draw, decals; the frame's impact sound
//                cues go to Bf3GameAudio
//   start_effect client-local start of any blueprint (E8 (4) oilfire on Metro; only m2a_check calls it, never the
//                server or the wire)
// fx3 as built (docs/design/m2a-fx3.md §4): the EffectLibrary is uncached (M2aLoadInputs::cache is valid during load
// only; the preload of the 72 Metro blueprints takes milliseconds uncached); start_effect resolves a short name over
// the level's EffectBlueprint and EmitterDocument names (an EmitterDocument goes through compile_emitter_effect), and
// a full name the level does not hold over a lazily opened @all-mp library (seconds on the main thread, once: the
// oilfire is MP_012's, a check-only path). own_muzzle (G6) gives gx the own 1p MuzzleSample at the def's first
// FireEffects1p entry; effect_sound_cues gives au3 the frame's effect sounds (audio::EffectSoundRequest's fields).

#include "m2a_game.h"

#include "bf3/core/result.h"
#include "bf3/fx/simulator.h"
#include "bf3/fx/triggers.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace godot {
class Node3D;
} // namespace godot

namespace bf3::godot_side {

class Bf3FxRenderer;
class Bf3Viewmodel;

struct GameFxStats {
	fx::FxTriggerStats triggers;
	uint64_t frames = 0;
	uint32_t effects_live = 0;
	uint32_t particles = 0; // FxWorld::particles() this frame
	uint32_t drawn_instances = 0; // the renderer's instance count this frame (E8 (3): equals particles)
	uint32_t lights = 0; // <= fx::kMaxFxLights (E9)
	uint32_t decals_live = 0; // <= fx::kMaxDecals
	uint32_t atlas_entries = 0, atlas_fallbacks = 0; // E8 (6)
	uint32_t preloaded = 0, preload_failures = 0; // E8 (1)
	// ---- fx3 additions ----
	uint64_t decals_placed = 0, decals_replaced = 0, decals_no_template = 0, decals_untextured = 0;
	uint32_t decal_templates = 0, decal_textures = 0, decal_templates_untextured = 0;
	uint64_t effect_sounds = 0; // effect sound cues handed out (SoundStart + SoundStop)
	uint64_t muzzles_dropped = 0; // 1p muzzles dropped (free camera, own soldier dead)
	uint64_t start_effect_calls = 0, start_effect_failures = 0;
	uint32_t texture_fallbacks = 0; // RenderPrep's default-texture fallbacks this frame
	double load_seconds = 0;
};

class Bf3GameFx {
public:
	Bf3GameFx();
	~Bf3GameFx(); // shutdown()
	Bf3GameFx(const Bf3GameFx &) = delete;
	Bf3GameFx &operator=(const Bf3GameFx &) = delete;

	core::Status load(const M2aLoadInputs &p_in);
	bool loaded() const;
	// After load: every ImpactCell::impact_sound of the level's table (fx::impact_sound_names), for Bf3GameAudio::load.
	std::vector<std::string> impact_sound_names() const;
	bool build(godot::Node3D *p_parent, std::string *r_error);
	bool built() const;
	void step(const M2aFrame &p_frame);
	// This frame's impact sound cues (valid until the next step).
	std::span<const fx::ImpactSoundCue> sound_cues() const;
	// fx3: this frame's effect sound starts / stops (fx::effect_sound_cues of the FxWorld's events; valid until the
	// next step). Field for field au3's audio::EffectSoundRequest (ImpactAudio::on_effect_sound).
	std::span<const fx::EffectSoundCue> effect_sound_cues() const;
	// fx3 (G6): the own 1p muzzle for M2aFrame::muzzles: viewmodel_muzzle_transform at the def's first FireEffects1p
	// entry, as an effect frame (viewmodel_muzzle.h muzzle_sample). Invalid before load, for a def without 1p effects
	// (the stand-in muzzle is used then) or when the viewmodel has no muzzle.
	fx::MuzzleSample own_muzzle(const Bf3Viewmodel &p_viewmodel, uint16_t p_def, uint8_t p_slot, const fx::Vec3 &p_velocity) const;
	// fx3: the viewmodel's projection for the FirstPerson pass (Bf3FxRenderer::set_first_person_projection; after build).
	void set_first_person_projection(float p_fov_override, float p_z_clip_scale);
	// Client-local start (E8 (4)). p_blueprint: a full partition or a short name (find_partition).
	core::Result<fx::EffectHandle> start_effect(std::string_view p_blueprint, const fx::Transform &p_transform, float p_start_age = 0.0f);
	const fx::FxWorld *world() const; // null before load
	// fx3: the loaded tables (null before load) and the renderer (null before build), for checks and demos.
	const fx::ImpactTable *impacts() const;
	const fx::FireEffectTable *fire_effects() const;
	const Bf3FxRenderer *renderer() const;
	const fx::FxTriggers *triggers() const;
	// The load's notes (sizes, times, the first errors), for the session log.
	std::vector<std::string> notes() const;
	GameFxStats stats() const;
	void shutdown(); // frees the renderer's and the decals' RIDs (idempotent; main thread)

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace bf3::godot_side
