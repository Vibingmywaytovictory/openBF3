#pragma once

#include "bf3/ai/main/ai_settings.h"
#include "bf3/core/result.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
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

struct WeaponTemplate {
	float deviation = 14.0f;
	float deviation_correction_time = 15.0f;
	float deviation_correction_rate = 0.06666667f;

	bool operator==(const WeaponTemplate &) const = default;
};

struct AiConfig {
	AiSettings settings;
	std::vector<std::string> behaviours;
	std::map<std::string, UrgeCurveDef> urge_curves;
	std::map<std::string, std::vector<float>> behaviour_modifiers;
	std::string active_modifiers;
	std::vector<VehicleBehaviour> vehicle_behaviours;
	std::map<std::string, WeaponTemplate> weapon_templates;
	std::string active_weapon_template;
	std::vector<std::string> scripts;

	int behaviour_id(std::string_view p_name) const;
	const WeaponTemplate &weapon_template(std::string_view p_name) const;

	bool operator==(const AiConfig &) const = default;
};

inline constexpr size_t kMaxAiScriptBytes = 64 * 1024;
inline constexpr size_t kMaxAiScriptLines = 2048;
inline constexpr std::string_view kAiDefaultScript = "AIDefault.ai";
inline constexpr std::string_view kAiBehavioursScript = "AIBehaviours.ai";

std::filesystem::path level_ai_script(const std::filesystem::path &p_root, std::string_view p_level);
core::Status run_ai_script(std::string_view p_text, std::string_view p_name, AiConfig &r_config);
core::Status run_ai_script_file(const std::filesystem::path &p_path, AiConfig &r_config);
core::Result<AiConfig> load_level_ai_config(const std::filesystem::path &p_root, std::string_view p_level);
core::Status apply_behaviour_weights(const AiConfig &p_config, BehaviourTuning &r_tuning);

}
