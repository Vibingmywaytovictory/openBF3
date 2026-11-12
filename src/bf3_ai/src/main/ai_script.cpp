#include "bf3/ai/main/ai_script.h"

#include "bf3/ai/behaviour/behaviours.h"
#include "bf3/core/util/key_values.h"
#include "bf3/core/util/text.h"

#include <algorithm>
#include <fstream>
#include <span>

namespace bf3::ai {

namespace {

using Args = std::span<const std::string_view>;
using core::KeyFault;
using core::KeyOutcome;

struct Command {
	std::string_view name;
	uint8_t min_args = 0;
	uint8_t max_args = 0;
	core::Status (*run)(Args p_args, AiConfig &r_config);
};

core::Error fault(std::string_view p_arg, const KeyOutcome &p_outcome, double p_min, double p_max) {
	const std::string shown = "'" + core::shown_text(p_arg) + "'";
	switch (p_outcome.fault) {
		case KeyFault::NotAWholeNumber:
			return core::make_error(shown + " is not a whole number");
		case KeyFault::OutOfRange:
			return core::make_error(shown + " is outside " + core::float_text(p_min) + ".." + core::float_text(p_max));
		default:
			return core::make_error(shown + " is not a number");
	}
}

core::Status read_float(std::string_view p_arg, double p_min, double p_max, float &r_out) {
	const KeyOutcome o = core::parse_float_value(p_arg, p_min, p_max, r_out);
	return o.fault == KeyFault::None ? core::Status::success() : core::Status(fault(p_arg, o, p_min, p_max));
}

core::Status read_int(std::string_view p_arg, double p_min, double p_max, int32_t &r_out) {
	const KeyOutcome o = core::parse_i32_value(p_arg, p_min, p_max, r_out);
	return o.fault == KeyFault::None ? core::Status::success() : core::Status(fault(p_arg, o, p_min, p_max));
}

core::Status read_bool(std::string_view p_arg, bool &r_out) {
	uint32_t v = 0;
	const KeyOutcome o = core::parse_u32_value(p_arg, 0, 1, v);
	if (o.fault != KeyFault::None) {
		return fault(p_arg, o, 0, 1);
	}
	r_out = v != 0;
	return core::Status::success();
}

core::Status read_uint(std::string_view p_arg, double p_min, double p_max, uint32_t &r_out) {
	const KeyOutcome o = core::parse_u32_value(p_arg, p_min, p_max, r_out);
	return o.fault == KeyFault::None ? core::Status::success() : core::Status(fault(p_arg, o, p_min, p_max));
}

template <class T> T *find_named(std::map<std::string, T> &r_map, std::string_view p_name) {
	const auto found = std::find_if(r_map.begin(), r_map.end(),
			[&](const auto &p_entry) { return core::iequals_ascii(p_entry.first, p_name); });
	return found != r_map.end() ? &found->second : nullptr;
}

template <class T> const T *find_named(const std::map<std::string, T> &p_map, std::string_view p_name) {
	const auto found = std::find_if(p_map.begin(), p_map.end(),
			[&](const auto &p_entry) { return core::iequals_ascii(p_entry.first, p_name); });
	return found != p_map.end() ? &found->second : nullptr;
}

core::Status no_vehicle(std::string_view p_name) {
	return core::make_error("'" + core::shown_text(p_name) + "' is not a vehicle (aiSettings.setVehicle)");
}

core::Status no_behaviour(std::string_view p_name) {
	return core::make_error("'" + core::shown_text(p_name) + "' is not a behaviour (aiSettings.setBehaviour)");
}

core::Status no_modifiers(std::string_view p_name) {
	return core::make_error(
			"'" + core::shown_text(p_name) + "' is not a modifier set (aiSettings.createBehaviourModifiers)");
}

const std::vector<float> *modifier_set(const AiConfig &p_config, std::string_view p_name) {
	return find_named(p_config.behaviour_modifiers, p_name);
}

core::Status curve(Args p_args, UrgeCurveDef::Kind p_kind, AiConfig &r_config) {
	UrgeCurveDef def;
	def.kind = p_kind;
	for (size_t i = 1; i < p_args.size(); ++i) {
		BF3_TRY(read_float(p_args[i], -1.0e6, 1.0e6, def.values[i - 1]));
	}
	r_config.urge_curves[std::string(p_args[0])] = def;
	return core::Status::success();
}

WeaponTemplate *active_weapon(AiConfig &r_config) {
	return r_config.active_weapon_template.empty()
			? nullptr
			: find_named(r_config.weapon_templates, r_config.active_weapon_template);
}

core::Status no_weapon_template() {
	return core::make_error("no active weapon template (weaponTemplate.create or weaponTemplate.active)");
}

const Command kCommands[] = {
	{ "aiSettings.setMaxNBots", 1, 1,
			[](Args a, AiConfig &c) -> core::Status { return read_int(a[0], -1, 64, c.settings.max_bots); } },
	{ "aiSettings.maxBotsIncludeHumans", 1, 1,
			[](Args a, AiConfig &c) -> core::Status { return read_bool(a[0], c.settings.max_bots_include_humans); } },
	{ "aiSettings.setIgnoreCommanderDistance", 1, 1,
			[](Args a, AiConfig &c) -> core::Status {
				return read_float(a[0], 0.0, 1.0e5, c.settings.ignore_commander_distance_m);
			} },
	{ "aiSettings.setMaxNumberOfBehaviours", 1, 1,
			[](Args a, AiConfig &c) -> core::Status {
				BF3_TRY(read_int(a[0], 0, 255, c.settings.behaviour_count));
				c.behaviours.resize(size_t(c.settings.behaviour_count));
				return core::Status::success();
			} },
	{ "aiSettings.setViewDistance", 1, 1,
			[](Args a, AiConfig &c) -> core::Status {
				return read_float(a[0], -1.0, 1.0e5, c.settings.view_distance_m);
			} },
	{ "aiSettings.setStatsViewDistance", 1, 1,
			[](Args a, AiConfig &c) -> core::Status {
				return read_float(a[0], 0.0, 1.0e5, c.settings.stats_view_distance_m);
			} },
	{ "aiSettings.setDrawBotStats", 1, 1,
			[](Args a, AiConfig &c) -> core::Status { return read_bool(a[0], c.settings.draw_bot_stats); } },
	{ "aiSettings.setInformationGridDimension", 1, 1,
			[](Args a, AiConfig &c) -> core::Status {
				return read_uint(a[0], 1, 1024, c.settings.information_grid_dimension);
			} },
	{ "aiSettings.setVehicle", 1, 1,
			[](Args a, AiConfig &c) -> core::Status {
				c.settings.set_vehicle(std::string(a[0]));
				return core::Status::success();
			} },
	{ "aiSettings.setManDownEquipment", 1, 1,
			[](Args a, AiConfig &c) -> core::Status {
				c.settings.man_down_equipment = std::string(a[0]);
				return core::Status::success();
			} },
	{ "aiSettings.setNSides", 1, 1,
			[](Args a, AiConfig &c) -> core::Status { return read_uint(a[0], 0, 2, c.settings.sides); } },
	{ "aiSettings.setBotSkill", 1, 1,
			[](Args a, AiConfig &c) -> core::Status { return read_float(a[0], 0.0, 1.0, c.settings.bot_skill); } },
	{ "aiSettings.setSAIUpdateFrequency", 1, 1,
			[](Args a, AiConfig &c) -> core::Status {
				return read_float(a[0], 0.01, 1000.0, c.settings.sai_update_frequency);
			} },
	{ "aiSettings.setRespawnAllowed", 1, 1,
			[](Args a, AiConfig &c) -> core::Status { return read_bool(a[0], c.settings.respawn_allowed); } },
	{ "aiSettings.overrideMenuSettings", 1, 1,
			[](Args a, AiConfig &c) -> core::Status { return read_bool(a[0], c.settings.override_menu_settings); } },
	{ "aiSettings.autoSpawnBots", 1, 1,
			[](Args a, AiConfig &c) -> core::Status { return read_bool(a[0], c.settings.auto_spawn_bots); } },
	{ "aiSettings.setBehaviour", 2, 2,
			[](Args a, AiConfig &c) -> core::Status {
				int32_t id = 0;
				BF3_TRY(read_int(a[0], 0, double(c.settings.behaviour_count) - 1.0, id));
				c.behaviours[size_t(id)] = std::string(a[1]);
				return core::Status::success();
			} },
	{ "aiSettings.createUCConstant", 2, 2,
			[](Args a, AiConfig &c) -> core::Status { return curve(a, UrgeCurveDef::Kind::Constant, c); } },
	{ "aiSettings.createUCLinear", 3, 3,
			[](Args a, AiConfig &c) -> core::Status { return curve(a, UrgeCurveDef::Kind::Linear, c); } },
	{ "aiSettings.createUCXInverse", 5, 5,
			[](Args a, AiConfig &c) -> core::Status { return curve(a, UrgeCurveDef::Kind::XInverse, c); } },
	{ "aiSettings.createBehaviourModifiers", 1, 1,
			[](Args a, AiConfig &c) -> core::Status {
				if (modifier_set(c, a[0]) == nullptr) {
					c.behaviour_modifiers[std::string(a[0])] =
							std::vector<float>(size_t(std::max(c.settings.behaviour_count, 0)), 0.0f);
					c.active_modifiers = std::string(a[0]);
				}
				return core::Status::success();
			} },
	{ "aiSettings.activeBehaviourModifiers", 1, 1,
			[](Args a, AiConfig &c) -> core::Status {
				if (modifier_set(c, a[0]) == nullptr) {
					return no_modifiers(a[0]);
				}
				c.active_modifiers = std::string(a[0]);
				return core::Status::success();
			} },
	{ "aiSettings.setBehaviourModifier", 2, 2,
			[](Args a, AiConfig &c) -> core::Status {
				std::vector<float> *set = find_named(c.behaviour_modifiers, c.active_modifiers);
				if (c.active_modifiers.empty() || set == nullptr) {
					return core::Status(core::make_error("no active modifier set"));
				}
				const int id = c.behaviour_id(a[0]);
				if (id < 0 || size_t(id) >= set->size()) {
					return no_behaviour(a[0]);
				}
				return read_float(a[1], 0.0, 1.0e6, (*set)[size_t(id)]);
			} },
	{ "aiSettings.setBasicBotWeights", 1, 1,
			[](Args a, AiConfig &c) -> core::Status {
				const std::vector<float> *set = modifier_set(c, a[0]);
				if (set == nullptr) {
					return no_modifiers(a[0]);
				}
				c.settings.basic_bot_weights = *set;
				return core::Status::success();
			} },
	{ "aiSettings.setStandardPersonality", 2, 2,
			[](Args a, AiConfig &c) -> core::Status {
				const std::vector<float> *set = modifier_set(c, a[0]);
				if (set == nullptr) {
					return no_modifiers(a[0]);
				}
				int32_t side = 0;
				BF3_TRY(read_int(a[1], 1, 2, side));
				c.settings.standard_personality[size_t(side - 1)] = *set;
				return core::Status::success();
			} },
	{ "aiSettings.setVehicleBehaviour", 7, 7,
			[](Args a, AiConfig &c) -> core::Status {
				if (c.settings.vehicle(a[0]) < 0) {
					return no_vehicle(a[0]);
				}
				VehicleBehaviour b;
				b.behaviour = c.behaviour_id(a[1]);
				if (b.behaviour < 0) {
					return no_behaviour(a[1]);
				}
				if (find_named(c.urge_curves, a[5]) == nullptr) {
					return core::Status(core::make_error(
							"'" + core::shown_text(a[5]) + "' is not an urge curve (aiSettings.createUC*)"));
				}
				if (modifier_set(c, a[6]) == nullptr) {
					return no_modifiers(a[6]);
				}
				BF3_TRY(read_int(a[4], 0, 255, b.mode));
				b.create_function = std::string(a[2]);
				b.urge_curve = std::string(a[5]);
				b.modifiers = std::string(a[6]);
				const auto same = std::find_if(c.vehicle_behaviours.begin(), c.vehicle_behaviours.end(),
						[&](const VehicleBehaviour &p_b) { return p_b.behaviour == b.behaviour; });
				if (same != c.vehicle_behaviours.end()) {
					*same = std::move(b);
				} else {
					c.vehicle_behaviours.push_back(std::move(b));
				}
				return core::Status::success();
			} },
	{ "weaponTemplate.create", 1, 1,
			[](Args a, AiConfig &c) -> core::Status {
				if (find_named(c.weapon_templates, a[0]) != nullptr) {
					c.active_weapon_template.clear();
					return core::Status(
							core::make_error("weapon template '" + core::shown_text(a[0]) + "' already exists"));
				}
				c.weapon_templates[std::string(a[0])] = WeaponTemplate{};
				c.active_weapon_template = std::string(a[0]);
				return core::Status::success();
			} },
	{ "weaponTemplate.active", 1, 1,
			[](Args a, AiConfig &c) -> core::Status {
				if (find_named(c.weapon_templates, a[0]) == nullptr) {
					c.active_weapon_template.clear();
					return core::Status(core::make_error("no weapon template '" + core::shown_text(a[0]) + "'"));
				}
				c.active_weapon_template = std::string(a[0]);
				return core::Status::success();
			} },
	{ "weaponTemplate.deviation", 1, 1,
			[](Args a, AiConfig &c) -> core::Status {
				WeaponTemplate *w = active_weapon(c);
				return w != nullptr ? read_float(a[0], 0.0, 180.0, w->deviation) : no_weapon_template();
			} },
	{ "weaponTemplate.deviationCorrectionTime", 1, 1,
			[](Args a, AiConfig &c) -> core::Status {
				WeaponTemplate *w = active_weapon(c);
				if (w == nullptr) {
					return no_weapon_template();
				}
				BF3_TRY(read_float(a[0], 0.0, 3600.0, w->deviation_correction_time));
				w->deviation_correction_rate =
						w->deviation_correction_time != 0.0f ? 1.0f / w->deviation_correction_time : 0.0f;
				return core::Status::success();
			} },
};

std::vector<std::string_view> tokens(std::string_view p_line) {
	std::vector<std::string_view> out;
	size_t i = 0;
	while (i < p_line.size()) {
		while (i < p_line.size() && (p_line[i] == ' ' || p_line[i] == '\t')) {
			++i;
		}
		const size_t start = i;
		while (i < p_line.size() && p_line[i] != ' ' && p_line[i] != '\t') {
			++i;
		}
		if (i > start) {
			out.push_back(p_line.substr(start, i - start));
		}
	}
	return out;
}

}

int AiConfig::behaviour_id(std::string_view p_name) const {
	for (size_t i = 0; i < behaviours.size(); ++i) {
		if (!behaviours[i].empty() && core::iequals_ascii(behaviours[i], p_name)) {
			return int(i);
		}
	}
	return -1;
}

const WeaponTemplate &AiConfig::weapon_template(std::string_view p_name) const {
	static const WeaponTemplate kDefault;
	const WeaponTemplate *found = find_named(weapon_templates, p_name);
	return found != nullptr ? *found : kDefault;
}

std::filesystem::path level_ai_script(const std::filesystem::path &p_root, std::string_view p_level) {
	return p_root / "Levels" / std::filesystem::path(std::u8string(p_level.begin(), p_level.end())) / "AI" / "AI.ai";
}

core::Status run_ai_script(std::string_view p_text, std::string_view p_name, AiConfig &r_config) {
	const std::string name(p_name);
	if (p_text.size() > kMaxAiScriptBytes) {
		return core::make_error(
				name + ": " + std::to_string(p_text.size()) + " bytes, more than " + std::to_string(kMaxAiScriptBytes));
	}
	AiConfig next = r_config;
	size_t line_no = 0;
	size_t at = 0;
	while (at <= p_text.size()) {
		const size_t end = std::min(p_text.find('\n', at), p_text.size());
		std::string_view line = p_text.substr(at, end - at);
		at = end + 1;
		if (++line_no > kMaxAiScriptLines) {
			return core::make_error(name + ": more than " + std::to_string(kMaxAiScriptLines) + " lines");
		}
		if (!line.empty() && line.back() == '\r') {
			line.remove_suffix(1);
		}
		const std::vector<std::string_view> words = tokens(line);
		if (words.empty()) {
			continue;
		}
		const std::string where = name + ":" + std::to_string(line_no);
		const auto command = std::find_if(std::begin(kCommands), std::end(kCommands),
				[&](const Command &p_c) { return core::iequals_ascii(p_c.name, words[0]); });
		if (command == std::end(kCommands)) {
			return core::make_error(where + ": unknown command '" + core::shown_text(words[0]) + "'");
		}
		const Args args(words.data() + 1, words.size() - 1);
		if (args.size() < command->min_args || args.size() > command->max_args) {
			return core::make_error(where + ": " + std::string(command->name) + " takes " +
					std::to_string(command->min_args) +
					(command->max_args != command->min_args ? ".." + std::to_string(command->max_args) : "") +
					" arguments, not " + std::to_string(args.size()));
		}
		const core::Status st = command->run(args, next);
		if (!st.ok()) {
			return core::error_at(where + ": " + std::string(command->name), st.error());
		}
	}
	next.scripts.push_back(name);
	r_config = std::move(next);
	return core::Status::success();
}

core::Status run_ai_script_file(const std::filesystem::path &p_path, AiConfig &r_config) {
	const std::string name = p_path.generic_string();
	std::ifstream f(p_path, std::ios::binary);
	if (!f) {
		return core::make_error("ai script: cannot open " + name);
	}
	std::string text;
	text.resize(kMaxAiScriptBytes + 1);
	f.read(text.data(), std::streamsize(text.size()));
	text.resize(size_t(f.gcount()));
	return run_ai_script(text, name, r_config);
}

core::Result<AiConfig> load_level_ai_config(const std::filesystem::path &p_root, std::string_view p_level) {
	AiConfig config;
	BF3_TRY(run_ai_script_file(p_root / kAiDefaultScript, config));
	BF3_TRY(run_ai_script_file(p_root / kAiBehavioursScript, config));
	BF3_TRY(run_ai_script_file(level_ai_script(p_root, p_level), config));
	return config;
}

core::Status apply_behaviour_weights(const AiConfig &p_config, BehaviourTuning &r_tuning) {
	const int infantry = p_config.settings.vehicle("Infantry");
	if (infantry < 0) {
		return no_vehicle("Infantry");
	}
	if (p_config.settings.basic_bot_weights.size() != kInfantryBehaviourCount) {
		return core::make_error("ai config: the basic bot weights (aiSettings.setBasicBotWeights) hold " +
				std::to_string(p_config.settings.basic_bot_weights.size()) + " behaviours, not " +
				std::to_string(kInfantryBehaviourCount));
	}
	BehaviourTuning tuning = r_tuning;
	for (size_t i = 0; i < kInfantryBehaviourCount; ++i) {
		const std::string_view name = infantry_behaviour_kind(InfantryBehaviour(i)).substr(2);
		if (p_config.behaviour_id(name) != int(i)) {
			return core::make_error("ai config: behaviour " + std::to_string(i) + " is not " + std::string(name));
		}
		const auto bound = std::find_if(p_config.vehicle_behaviours.begin(), p_config.vehicle_behaviours.end(),
				[&](const VehicleBehaviour &p_b) { return p_b.behaviour == int(i); });
		if (bound == p_config.vehicle_behaviours.end()) {
			return core::make_error(
					"ai config: Infantry has no " + std::string(name) + " (aiSettings.setVehicleBehaviour)");
		}
		const std::string kind(infantry_behaviour_kind(InfantryBehaviour(i)));
		if (!core::iequals_ascii(bound->create_function, kind)) {
			return core::make_error("ai config: Infantry " + std::string(name) + " is created by '" +
					bound->create_function + "', not " + kind);
		}
		const UrgeCurveDef &curve = *find_named(p_config.urge_curves, bound->urge_curve);
		BehaviourCurve &out = tuning.curves[i];
		out.weight = p_config.settings.basic_bot_weights[i];
		if (curve.kind == UrgeCurveDef::Kind::Linear) {
			out.slope = curve.values[0];
			out.offset = curve.values[1];
		} else if (curve.kind == UrgeCurveDef::Kind::Constant) {
			out.slope = 0.0f;
			out.offset = curve.values[0];
		} else {
			return core::make_error("ai config: Infantry " + std::string(name) + "'s urge curve '" + bound->urge_curve +
					"' is a UCXInverse; a behaviour's memory curve is a UCConstant or UCLinear");
		}
	}
	r_tuning = tuning;
	return core::Status::success();
}

}
