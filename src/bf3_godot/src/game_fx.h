#pragma once

#include "game_frame.h"

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
	double load_seconds = 0;
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

	core::Result<fx::EffectHandle> start_effect(std::string_view p_blueprint, const fx::Transform &p_transform, float p_start_age = 0.0f);
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
