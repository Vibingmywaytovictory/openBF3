#include "level_lights.h"

#include "level_upload.h"

#include "bf3/core/util/format.h"
#include "bf3/fx/render_prep.h"
#include "bf3/fx/triggers.h"
#include "bf3/render/weapon_lights.h"

#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/variant/color.hpp>

#include <algorithm>
#include <cmath>

namespace bf3::godot_side {

using namespace godot;
namespace render = bf3::render;

size_t level_light_budget(int64_t p_clustered_elements) {
	const int64_t taken = int64_t(bf3::fx::kMaxFxLights) + int64_t(bf3::fx::kMaxDecals) + int64_t(kLevelLightReserve);
	return size_t(std::max<int64_t>(p_clustered_elements - taken, 0));
}

LevelLights::~LevelLights() {
	free_all();
}

void LevelLights::build(const render::SceneLightPlan &p_plan, const RID &p_scenario) {
	free_all();
	RenderingServer *rs = RenderingServer::get_singleton();
	plan_ = p_plan.lights;
	stats_ = p_plan.stats;
	budget_ = level_light_budget(int64_t(ProjectSettings::get_singleton()->get_setting_with_override(
			"rendering/limits/cluster_builder/max_clustered_elements")));
	lights_.reserve(plan_.size());
	for (const render::SceneLight &l : plan_) {
		Light made;
		made.light = l.spot ? rs->spot_light_create() : rs->omni_light_create();
		rs->light_set_color(made.light,
				Color(std::fmax(l.color[0], 0.0f), std::fmax(l.color[1], 0.0f), std::fmax(l.color[2], 0.0f)));
		// INFERRED: energy is Intensity and range Radius; AttenuationOffset and the frustum spots' textures are not
		// applied. Settle: a note on how BF3 lights with Intensity, Radius and AttenuationOffset.
		rs->light_set_param(made.light, RenderingServer::LIGHT_PARAM_ENERGY, l.intensity);
		rs->light_set_param(made.light, RenderingServer::LIGHT_PARAM_RANGE, l.radius);
		rs->light_set_param(made.light, RenderingServer::LIGHT_PARAM_ATTENUATION, kLevelLightAttenuation);
		rs->light_set_param(made.light, RenderingServer::LIGHT_PARAM_SPECULAR, l.specular ? 1.0f : 0.0f);
		rs->light_set_shadow(made.light, false);
		if (l.spot) {
			rs->light_set_param(
					made.light, RenderingServer::LIGHT_PARAM_SPOT_ANGLE, render::spot_light_half_angle_deg(*l.spot));
			++spots_;
		} else {
			++points_;
		}
		made.instance = rs->instance_create2(made.light, p_scenario);
		bf3::core::level::LevelTransform placed = l.transform.normalized();
		placed.scale = 1.0f;
		rs->instance_set_transform(made.instance, instance_transform(placed));
		rs->instance_set_visible(made.instance, false);
		lights_.push_back(made);
	}
	shadows_applied_ = false;
	set_shadow_quality(shadow_quality_);
}

void LevelLights::set_shadow_quality(uint8_t p_quality) {
	if (shadows_applied_ && shadow_quality_ == p_quality) {
		return;
	}
	shadows_applied_ = true;
	shadow_quality_ = p_quality;
	RenderingServer *rs = RenderingServer::get_singleton();
	shadowed_ = 0;
	for (size_t i = 0; i < lights_.size(); ++i) {
		if (!plan_[i].spot) {
			continue;
		}
		const bool shadows = bf3::core::level::spot_light_shadows(*plan_[i].spot, p_quality);
		rs->light_set_shadow(lights_[i].light, shadows);
		shadowed_ += shadows ? 1 : 0;
	}
}

void LevelLights::update(const Vector3 &p_camera) {
	if (lights_.empty() || (selected_ && p_camera.distance_to(selected_at_) < kLevelLightReselectDistance)) {
		return;
	}
	selected_ = true;
	selected_at_ = p_camera;
	render::nearest_lights(plan_, { float(p_camera.x), float(p_camera.y), float(p_camera.z) }, budget_, shown_);
	RenderingServer *rs = RenderingServer::get_singleton();
	size_t next = 0;
	for (uint32_t i = 0; i < lights_.size(); ++i) {
		const bool show = next < shown_.size() && shown_[next] == i;
		next += show ? 1 : 0;
		if (lights_[i].shown != show) {
			rs->instance_set_visible(lights_[i].instance, show);
			lights_[i].shown = show;
		}
	}
}

void LevelLights::free_all() {
	RenderingServer *rs = RenderingServer::get_singleton();
	for (Light &l : lights_) {
		if (l.instance.is_valid()) {
			rs->free_rid(l.instance);
		}
		if (l.light.is_valid()) {
			rs->free_rid(l.light);
		}
	}
	lights_.clear();
	plan_.clear();
	shown_.clear();
	selected_ = false;
	points_ = spots_ = shadowed_ = 0;
}

std::string LevelLights::describe() const {
	return bf3::core::fmt("lights: created %zu of %zu the data places (%zu point and %zu spot placed, "
						  "%zu point and %zu spot in intact health states; left out: %zu invisible, %zu switched off, "
						  "%zu unlit, %zu end of round, %zu unusable); %zu point, %zu spot, %zu spot shadows; "
						  "shown %zu of a budget of %zu",
			lights_.size(), stats_.found(), stats_.placed_point, stats_.placed_spot, stats_.health_state_point,
			stats_.health_state_spot, stats_.invisible, stats_.switched_off, stats_.unlit, stats_.end_of_round,
			stats_.unusable, points_, spots_, shadowed_, shown_.size(), budget_);
}

}
