#pragma once

#include "bf3/ai/main/ai_script.h"

namespace bf3::ai {

struct TriggerWeapon {
	bool thrown = false;
	float explosion_radius = 0.0f;
	bool guided = false;
};

float trigger_skill(float p_bot_skill, const TriggerWeapon &p_weapon, bool p_soldier, bool p_human_vehicle_target);
float ai_deviation_deg(const WeaponTemplate &p_template, float p_skill, double p_seconds_since, float p_scale);
float fire_delay_s(float p_bot_skill, float p_unit_draw);
bool fresh_fire_plan(bool p_fire_plan_running, float p_bot_skill);

}
