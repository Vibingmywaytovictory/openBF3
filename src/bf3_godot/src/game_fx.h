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
	// Client-local start (E8 (4)). p_blueprint: a full partition or a short name (find_partition).
	core::Result<fx::EffectHandle> start_effect(std::string_view p_blueprint, const fx::Transform &p_transform, float p_start_age = 0.0f);
	const fx::FxWorld *world() const; // null before load
	GameFxStats stats() const;
	void shutdown(); // frees the renderer's and the decals' RIDs (idempotent; main thread)

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace bf3::godot_side
