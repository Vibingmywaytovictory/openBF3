#pragma once

#include "game_frame.h"

#include "bf3/core/result.h"
#include "bf3/fx/placed_effects.h"
#include "bf3/fx/simulator.h"
#include "bf3/fx/triggers.h"
#include "bf3/host/logic_effect_feed.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace godot {
class Node3D;
}

namespace bf3::godot_side {

class Bf3FxRenderer;
class Bf3Viewmodel;

struct GameFxStats {
	fx::FxTriggerStats triggers;
	uint64_t frames = 0;
	uint32_t effects_live = 0;
	uint32_t particles = 0;
	uint32_t drawn_instances = 0;
	uint32_t lights = 0;
	uint32_t decals_live = 0;
	uint32_t atlas_entries = 0, atlas_fallbacks = 0;
	uint32_t preloaded = 0, preload_failures = 0;

	uint64_t decals_placed = 0, decals_replaced = 0, decals_no_template = 0, decals_untextured = 0;
	uint32_t decal_templates = 0, decal_textures = 0, decal_templates_untextured = 0;
	uint64_t effect_sounds = 0;
	uint64_t muzzles_dropped = 0;
	uint64_t start_effect_calls = 0, start_effect_failures = 0;
	uint32_t texture_fallbacks = 0;
	uint32_t effect_meshes = 0, effect_meshes_failed = 0;
	uint32_t mesh_particles = 0, mesh_particles_undrawn = 0;
	uint32_t light_candidates_max = 0, lights_max = 0, light_particles_max = 0;
	uint64_t light_frames_over = 0, lights_dropped = 0;
	uint64_t impact_light_starts = 0, impact_light_frames = 0;
	uint32_t impact_lights_max = 0;
	uint64_t impact_light_mismatch_frames = 0, lights_over_data = 0;
	double load_seconds = 0;
	fx::PlacedEffectStats placed;
};

class Bf3GameFx {
public:
	Bf3GameFx();
	~Bf3GameFx();
	Bf3GameFx(const Bf3GameFx &) = delete;
	Bf3GameFx &operator=(const Bf3GameFx &) = delete;

	core::Status load(const GameLoadInputs &p_in);
	bool loaded() const;

	std::vector<std::string> impact_sound_names() const;
	bool build(godot::Node3D *p_parent, std::string *r_error);
	bool built() const;
	void step(const GameFrame &p_frame);

	std::span<const fx::ImpactSoundCue> sound_cues() const;

	std::span<const fx::EffectSoundCue> effect_sound_cues() const;

	fx::MuzzleSample own_muzzle(const Bf3Viewmodel &p_viewmodel, uint16_t p_def, uint8_t p_slot, const fx::Vec3 &p_velocity) const;

	void set_first_person_projection(float p_fov_override, float p_z_clip_scale);
	void logic_effects(const core::gameplay::LogicGraph &p_graph, const host::LogicEffectFeed &p_feed);

	core::Result<fx::EffectHandle> start_effect(std::string_view p_blueprint, const fx::Transform &p_transform, float p_start_age = 0.0f);
	void stop_effect(fx::EffectHandle p_effect);
	void move_effect(fx::EffectHandle p_effect, const fx::Transform &p_transform, const fx::Vec3 &p_velocity);
	const fx::FxWorld *world() const;

	const fx::ImpactTable *impacts() const;
	const fx::FireEffectTable *fire_effects() const;
	const Bf3FxRenderer *renderer() const;
	const fx::FxTriggers *triggers() const;

	std::vector<std::string> notes() const;

	std::vector<std::string> decal_texture_report(const std::string &p_dump_dir = {}) const;
	GameFxStats stats() const;
	void shutdown();

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

}
