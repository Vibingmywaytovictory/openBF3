#pragma once

#include "bf3/ai/knowledge/ai_object.h"
#include "bf3/ai/main/ai_settings.h"
#include "bf3/core/result.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::ai {

struct BehaviourTuning;

struct UrgeCurveDef {
	enum class Kind : uint8_t { Constant, Linear, XInverse };
	Kind kind = Kind::Constant;
	std::array<float, 4> values{};

	bool operator==(const UrgeCurveDef &) const = default;
};

struct VehicleBehaviour {
	int behaviour = -1;
	std::string create_function;
	int mode = 0;
	std::string urge_curve;
	std::string modifiers;

	bool operator==(const VehicleBehaviour &) const = default;
};

enum class CombatStrengthType : uint8_t {
	Infantry = 0,
	LightArmour = 1,
	HeavyArmour = 2,
	NavalArmour = 3,
	Helicopter = 4,
	AirPlane = 5
};
inline constexpr size_t kCombatStrengthTypeCount = 6;

struct WeaponTemplate {
	bool indirect = false;
	bool is_thrown = false;
	bool use_aimer_only = false;
	int32_t fire_rate = -1;
	float drag = 0.0f;
	float min_range = 0.0f;
	float max_range = 0.0f;
	float explosion_radius = 0.0f;
	float sound_sphere_radius = 50.0f;
	float exit_velocity = 0.0f;
	float deviation = 14.0f;
	float deviation_correction_time = 15.0f;
	float deviation_correction_rate = 0.06666667f;
	std::array<int32_t, kCombatStrengthTypeCount> strengths{};
	SoldierPose firing_pose = SoldierPose::Standing;
	float allowed_deviation = 0.5f;
	float optimal_range_percentage = 0.35f;
	bool fires_through_transparent = true;
	PlayerInputMap weapon_activate = PlayerInputMap::None;
	PlayerInputMap weapon_fire = PlayerInputMap::None;

	int32_t strength(CombatStrengthType p_type) const { return strengths[size_t(p_type)]; }

	bool operator==(const WeaponTemplate &) const = default;
};

struct StrategicAreaDef {
	std::string name;
	std::string category;
	bool from_control_point = false;
	uint32_t control_point = 0;
	std::array<float, 2> corner{};
	std::array<float, 2> centre{};
	float height = 0.0f;
	float radius = 0.0f;
	int32_t layer = -1;
	int32_t side = 0;
	std::array<bool, 2> takeable{ true, true };
	std::vector<std::string> neighbours;

	bool operator==(const StrategicAreaDef &) const = default;
};

struct AiConfig {
	AiSettings settings;
	std::vector<std::string> behaviours;
	std::map<std::string, UrgeCurveDef> urge_curves;
	std::map<std::string, std::vector<float>> behaviour_modifiers;
	std::map<std::string, std::vector<bool>> given_modifiers;
	std::string active_modifiers;
	std::vector<VehicleBehaviour> vehicle_behaviours;
	std::map<std::string, WeaponTemplate> weapon_templates;
	std::string active_weapon_template;
	std::vector<StrategicAreaDef> strategic_areas;
	std::string active_area;
	std::vector<std::string> scripts;

	int behaviour_id(std::string_view p_name) const;
	const StrategicAreaDef *strategic_area(std::string_view p_name) const;
	const WeaponTemplate &weapon_template(std::string_view p_name) const;

	bool operator==(const AiConfig &) const = default;
};

inline constexpr size_t kMaxAiScriptBytes = 64 * 1024;
inline constexpr size_t kMaxAiScriptLines = 2048;
inline constexpr std::string_view kAiDefaultScript = "AIDefault.ai";
inline constexpr std::string_view kAiBehavioursScript = "AIBehaviours.ai";
inline constexpr std::string_view kStrategicAreasScript = "StrategicAreas.ai";

std::filesystem::path level_ai_script(const std::filesystem::path &p_root, std::string_view p_level);
std::filesystem::path level_strategic_areas_script(const std::filesystem::path &p_root, std::string_view p_level);
const WeaponTemplate &find_weapon_template(
		const std::map<std::string, WeaponTemplate> &p_templates, std::string_view p_weapon);
core::Status run_ai_script(std::string_view p_text, std::string_view p_name, AiConfig &r_config);
core::Status run_ai_script_file(const std::filesystem::path &p_path, AiConfig &r_config);
std::vector<StrategicAreaDef> strategic_areas_by_name(std::span<const StrategicAreaDef> p_areas);
std::string format_strategic_areas(std::span<const StrategicAreaDef> p_areas);
std::string format_weapon_template(std::string_view p_name, const WeaponTemplate &p_template);
core::Result<AiConfig> load_level_ai_config(
		const std::filesystem::path &p_root, std::string_view p_level, std::string_view p_derived_areas);
core::Status apply_behaviour_weights(const AiConfig &p_config, BehaviourTuning &r_tuning);

}
