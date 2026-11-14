#include "bf3/ai/main/bot_skill.h"

#include <algorithm>

namespace bf3::ai {

float trigger_skill(float p_bot_skill, const TriggerWeapon &p_weapon, bool p_soldier, bool p_human_vehicle_target) {
	float skill = p_bot_skill;
	if (p_weapon.thrown) {
		skill = 1.0f;
	} else if (p_weapon.explosion_radius == 0.0f) {
		if (!p_human_vehicle_target) {
			skill = 1.0f - (1.0f - skill) * 0.5f;
		}
	} else if (!p_soldier) {
		skill = 1.0f - (1.0f - skill) * 0.8f;
	} else {
		skill = 1.0f - (1.0f - skill) * 0.4f;
	}
	return p_weapon.guided ? 1.0f : skill;
}

float ai_deviation_deg(const WeaponTemplate &p_template, float p_skill, double p_seconds_since, float p_scale) {
	const double t = std::min(p_seconds_since * double(p_template.deviation_correction_rate), 1.0);
	return float((((1.0 - t) + 1.0) * 0.5) * (1.0 - double(p_skill)) * double(p_template.deviation) * double(p_scale));
}

float weapon_deviation_deg(const std::map<std::string, WeaponTemplate> &p_templates, std::string_view p_weapon,
		const TriggerWeapon &p_trigger, float p_bot_skill, double p_seconds_since) {
	return ai_deviation_deg(find_weapon_template(p_templates, p_weapon),
			trigger_skill(p_bot_skill, p_trigger, true, false), p_seconds_since, 1.0f);
}

float fire_delay_s(float p_bot_skill, float p_unit_draw) {
	return (1.0f - p_bot_skill) * (p_unit_draw + 1.5f);
}

bool fresh_fire_plan(bool p_fire_plan_running, float p_bot_skill) {
	return !p_fire_plan_running || p_bot_skill < 0.3f;
}

}
