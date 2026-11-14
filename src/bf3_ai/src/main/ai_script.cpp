#include "bf3/ai/main/ai_script.h"

#include "bf3/ai/behaviour/behaviours.h"
#include "bf3/core/util/key_values.h"
#include "bf3/core/util/text.h"

#include <algorithm>
#include <fstream>
#include <limits>
#include <span>
#include <utility>

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

template <class T, class C> T *find_named(std::map<std::string, T, C> &r_map, std::string_view p_name) {
	const auto found = std::find_if(r_map.begin(), r_map.end(),
			[&](const auto &p_entry) { return core::iequals_ascii(p_entry.first, p_name); });
	return found != r_map.end() ? &found->second : nullptr;
}

template <class T, class C> const T *find_named(const std::map<std::string, T, C> &p_map, std::string_view p_name) {
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

constexpr std::array<std::string_view, kCombatStrengthTypeCount> kStrengthTypeNames{ "Infantry", "LightArmour",
	"HeavyArmour", "NavalArmour", "Helicopter", "AirPlane" };

constexpr std::array<std::string_view, 5> kSoldierPoseNames{ "NotAvailable", "Standing", "Crouching", "Lying",
	"Swimming" };

constexpr std::array<std::pair<std::string_view, PlayerInputMap>, 33> kPlayerInputNames{ {
		{ "PIYaw", PlayerInputMap::Yaw },
		{ "PIPitch", PlayerInputMap::Pitch },
		{ "PIRoll", PlayerInputMap::Roll },
		{ "PIThrottle", PlayerInputMap::Throttle },
		{ "PIMouseLookX", PlayerInputMap::MouseLookX },
		{ "PIMouseLookY", PlayerInputMap::MouseLookY },
		{ "PIFire", PlayerInputMap::Fire },
		{ "PIAction", PlayerInputMap::Action },
		{ "PIUse", PlayerInputMap::Use },
		{ "PIMouseLook", PlayerInputMap::MouseLook },
		{ "PISprint", PlayerInputMap::Sprint },
		{ "PIWeaponSelect1", PlayerInputMap::WeaponSelect1 },
		{ "PIWeaponSelect2", PlayerInputMap::WeaponSelect2 },
		{ "PIWeaponSelect3", PlayerInputMap::WeaponSelect3 },
		{ "PIWeaponSelect4", PlayerInputMap::WeaponSelect4 },
		{ "PIWeaponSelect5", PlayerInputMap::WeaponSelect5 },
		{ "PIWeaponSelect6", PlayerInputMap::WeaponSelect6 },
		{ "PIWeaponSelect7", PlayerInputMap::WeaponSelect7 },
		{ "PIWeaponSelect8", PlayerInputMap::WeaponSelect8 },
		{ "PIWeaponSelect9", PlayerInputMap::WeaponSelect9 },
		{ "PIPositionSelect1", PlayerInputMap::PositionSelect1 },
		{ "PIPositionSelect2", PlayerInputMap::PositionSelect2 },
		{ "PIPositionSelect3", PlayerInputMap::PositionSelect3 },
		{ "PIPositionSelect4", PlayerInputMap::PositionSelect4 },
		{ "PIPositionSelect5", PlayerInputMap::PositionSelect5 },
		{ "PIPositionSelect6", PlayerInputMap::PositionSelect6 },
		{ "PIPositionSelect7", PlayerInputMap::PositionSelect7 },
		{ "PIPositionSelect8", PlayerInputMap::PositionSelect8 },
		{ "PIAltFire", PlayerInputMap::AltFire },
		{ "PIReload", PlayerInputMap::Reload },
		{ "PILie", PlayerInputMap::Lie },
		{ "PICrouch", PlayerInputMap::Crouch },
		{ "PIFlareFire", PlayerInputMap::FlareFire },
} };

std::string_view player_input_name(PlayerInputMap p_input) {
	for (const auto &[name, input] : kPlayerInputNames) {
		if (input == p_input) {
			return name;
		}
	}
	return "PINone";
}

core::Status read_strength_type(std::string_view p_arg, CombatStrengthType &r_out) {
	for (size_t i = 0; i < kStrengthTypeNames.size(); ++i) {
		const std::string_view name = kStrengthTypeNames[i];
		const bool infantery = i == 0 && (p_arg == "CSTInfantery" || core::iequals_ascii(p_arg, "Infantery"));
		if (p_arg == std::to_string(i) || p_arg == "CST" + std::string(name) || core::iequals_ascii(p_arg, name) ||
				infantery) {
			r_out = CombatStrengthType(i);
			return core::Status::success();
		}
	}
	return core::make_error("'" + core::shown_text(p_arg) + "' is not a combat strength type (Infantry..AirPlane)");
}

SoldierPose soldier_pose(std::string_view p_arg) {
	for (size_t i = 0; i < kSoldierPoseNames.size(); ++i) {
		if (p_arg == kSoldierPoseNames[i] || p_arg == "SP" + std::string(kSoldierPoseNames[i])) {
			return SoldierPose(i);
		}
	}
	return SoldierPose::NotAvailable;
}

core::Status read_player_input(std::string_view p_arg, PlayerInputMap &r_out) {
	if (p_arg == "PINone") {
		r_out = PlayerInputMap::None;
		return core::Status::success();
	}
	for (const auto &[name, input] : kPlayerInputNames) {
		if (p_arg == name) {
			r_out = input;
			return core::Status::success();
		}
	}
	return core::make_error(
			"'" + core::shown_text(p_arg) + "' is not an input the AI reads (PIFire, PIWeaponSelect1..)");
}

template <float WeaponTemplate::*Field, int Lo, int Hi> core::Status weapon_float(Args p_args, AiConfig &r_config) {
	WeaponTemplate *w = active_weapon(r_config);
	return w != nullptr ? read_float(p_args[0], Lo, Hi, w->*Field) : no_weapon_template();
}

template <bool WeaponTemplate::*Field> core::Status weapon_bool(Args p_args, AiConfig &r_config) {
	WeaponTemplate *w = active_weapon(r_config);
	return w != nullptr ? read_bool(p_args[0], w->*Field) : no_weapon_template();
}

template <PlayerInputMap WeaponTemplate::*Field> core::Status weapon_input(Args p_args, AiConfig &r_config) {
	WeaponTemplate *w = active_weapon(r_config);
	return w != nullptr ? read_player_input(p_args[0], w->*Field) : no_weapon_template();
}

StrategicAreaDef *find_area(AiConfig &r_config, std::string_view p_name) {
	return const_cast<StrategicAreaDef *>(std::as_const(r_config).strategic_area(p_name));
}

StrategicAreaDef *active_area(AiConfig &r_config) {
	return r_config.active_area.empty() ? nullptr : find_area(r_config, r_config.active_area);
}

core::Status no_active_area() {
	return core::make_error("no active strategic area (aiStrategicArea.setActive)");
}

core::Status no_area(std::string_view p_name) {
	return core::make_error("'" + core::shown_text(p_name) + "' is not a strategic area (aiStrategicArea.create)");
}

core::Status read_pair(std::string_view p_arg, std::array<float, 2> &r_out) {
	const size_t slash = p_arg.find('/');
	if (slash == std::string_view::npos) {
		return core::make_error("'" + core::shown_text(p_arg) + "' is not an x/z pair");
	}
	BF3_TRY(read_float(p_arg.substr(0, slash), -1.0e6, 1.0e6, r_out[0]));
	return read_float(p_arg.substr(slash + 1), -1.0e6, 1.0e6, r_out[1]);
}

core::Status new_area(AiConfig &r_config, StrategicAreaDef p_area) {
	if (find_area(r_config, p_area.name) != nullptr) {
		r_config.active_area.clear();
		return core::make_error("strategic area '" + core::shown_text(p_area.name) + "' already exists");
	}
	r_config.active_area = p_area.name;
	r_config.strategic_areas.push_back(std::move(p_area));
	return core::Status::success();
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
					c.given_modifiers[std::string(a[0])] =
							std::vector<bool>(size_t(std::max(c.settings.behaviour_count, 0)), false);
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
				BF3_TRY(read_float(a[1], 0.0, 1.0e6, (*set)[size_t(id)]));
				find_named(c.given_modifiers, c.active_modifiers)->at(size_t(id)) = true;
				return core::Status::success();
			} },
	{ "aiSettings.setBasicBotWeights", 1, 1,
			[](Args a, AiConfig &c) -> core::Status {
				const std::vector<float> *set = modifier_set(c, a[0]);
				if (set == nullptr) {
					return no_modifiers(a[0]);
				}
				const std::vector<bool> &given = *find_named(c.given_modifiers, a[0]);
				for (size_t i = 0; i < given.size(); ++i) {
					if (!given[i]) {
						return core::make_error("'" + core::shown_text(a[0]) + "' gives " +
								(i < c.behaviours.size() ? c.behaviours[i] : std::to_string(i)) +
								" no modifier (aiSettings.setBehaviourModifier)");
					}
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
	{ "rem", 0, 255, [](Args, AiConfig &) -> core::Status { return core::Status::success(); } },
	{ "aiStrategicArea.createFromControlPoint", 3, 4,
			[](Args a, AiConfig &c) -> core::Status {
				StrategicAreaDef area;
				area.name = std::string(a[0]);
				area.from_control_point = true;
				BF3_TRY(read_uint(a[1], 0, 65535, area.control_point));
				BF3_TRY(read_float(a[2], 0.0, 1.0e5, area.radius));
				area.category = a.size() > 3 ? std::string(a[3]) : std::string();
				return new_area(c, std::move(area));
			} },
	{ "aiStrategicArea.create", 5, 6,
			[](Args a, AiConfig &c) -> core::Status {
				StrategicAreaDef area;
				area.name = std::string(a[0]);
				BF3_TRY(read_pair(a[1], area.corner));
				BF3_TRY(read_pair(a[2], area.centre));
				BF3_TRY(read_float(a[3], -1.0e5, 1.0e5, area.height));
				BF3_TRY(read_float(a[4], 0.0, 1.0e5, area.radius));
				area.category = a.size() > 5 ? std::string(a[5]) : std::string();
				return new_area(c, std::move(area));
			} },
	{ "aiStrategicArea.layer", 1, 1,
			[](Args a, AiConfig &c) -> core::Status {
				StrategicAreaDef *area = active_area(c);
				return area != nullptr ? read_int(a[0], std::numeric_limits<int32_t>::lowest(),
												 std::numeric_limits<int32_t>::max(), area->layer)
									   : no_active_area();
			} },
	{ "aiStrategicArea.setActive", 1, 1,
			[](Args a, AiConfig &c) -> core::Status {
				if (find_area(c, a[0]) == nullptr) {
					c.active_area.clear();
					return no_area(a[0]);
				}
				c.active_area = std::string(a[0]);
				return core::Status::success();
			} },
	{ "aiStrategicArea.addNeighbour", 1, 1,
			[](Args a, AiConfig &c) -> core::Status {
				StrategicAreaDef *area = active_area(c);
				if (area == nullptr) {
					return no_active_area();
				}
				const StrategicAreaDef *other = find_area(c, a[0]);
				if (other == nullptr) {
					return no_area(a[0]);
				}
				if (other == area) {
					return core::make_error("'" + core::shown_text(a[0]) + "' cannot neighbour itself");
				}
				if (other->layer != area->layer) {
					return core::make_error("'" + core::shown_text(a[0]) + "' is on layer " +
							std::to_string(other->layer) + ", not " + std::to_string(area->layer));
				}
				area->neighbours.push_back(other->name);
				return core::Status::success();
			} },
	{ "aiStrategicArea.setSide", 1, 1,
			[](Args a, AiConfig &c) -> core::Status {
				StrategicAreaDef *area = active_area(c);
				return area != nullptr ? read_int(a[0], 0, 2, area->side) : no_active_area();
			} },
	{ "aiStrategicArea.setTakeable", 2, 2,
			[](Args a, AiConfig &c) -> core::Status {
				StrategicAreaDef *area = active_area(c);
				if (area == nullptr) {
					return no_active_area();
				}
				int32_t side = 0;
				BF3_TRY(read_int(a[0], 1, 2, side));
				return read_bool(a[1], area->takeable[size_t(side - 1)]);
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
	{ "weaponTemplate.indirect", 1, 1, weapon_bool<&WeaponTemplate::indirect> },
	{ "weaponTemplate.useAimerOnly", 1, 1, weapon_bool<&WeaponTemplate::use_aimer_only> },
	{ "weaponTemplate.drag", 1, 1, weapon_float<&WeaponTemplate::drag, -1000000, 1000000> },
	{ "weaponTemplate.minRange", 1, 1, weapon_float<&WeaponTemplate::min_range, -100000, 100000> },
	{ "weaponTemplate.maxRange", 1, 1, weapon_float<&WeaponTemplate::max_range, -100000, 100000> },
	{ "weaponTemplate.weaponActivate", 1, 1, weapon_input<&WeaponTemplate::weapon_activate> },
	{ "weaponTemplate.weaponFire", 1, 1, weapon_input<&WeaponTemplate::weapon_fire> },
	{ "weaponTemplate.isThrown", 1, 1, weapon_bool<&WeaponTemplate::is_thrown> },
	{ "weaponTemplate.soundSphereRadius", 1, 1, weapon_float<&WeaponTemplate::sound_sphere_radius, -100000, 100000> },
	{ "weaponTemplate.exitVelocity", 1, 1, weapon_float<&WeaponTemplate::exit_velocity, -100000, 100000> },
	{ "weaponTemplate.getStrength", 1, 1,
			[](Args a, AiConfig &c) -> core::Status {
				CombatStrengthType type{};
				return active_weapon(c) != nullptr ? read_strength_type(a[0], type) : no_weapon_template();
			} },
	{ "weaponTemplate.setStrength", 2, 2,
			[](Args a, AiConfig &c) -> core::Status {
				WeaponTemplate *w = active_weapon(c);
				if (w == nullptr) {
					return no_weapon_template();
				}
				CombatStrengthType type{};
				BF3_TRY(read_strength_type(a[0], type));
				return read_int(a[1], std::numeric_limits<int32_t>::lowest(), std::numeric_limits<int32_t>::max(),
						w->strengths[size_t(type)]);
			} },
	{ "weaponTemplate.fireRate", 1, 1,
			[](Args a, AiConfig &c) -> core::Status {
				WeaponTemplate *w = active_weapon(c);
				return w != nullptr ? read_int(a[0], std::numeric_limits<int32_t>::lowest(),
											  std::numeric_limits<int32_t>::max(), w->fire_rate)
									: no_weapon_template();
			} },
	{ "weaponTemplate.firingPose", 1, 1,
			[](Args a, AiConfig &c) -> core::Status {
				WeaponTemplate *w = active_weapon(c);
				if (w == nullptr) {
					return no_weapon_template();
				}
				w->firing_pose = soldier_pose(a[0]);
				return core::Status::success();
			} },
	{ "weaponTemplate.optimalRangePercentage", 1, 1,
			weapon_float<&WeaponTemplate::optimal_range_percentage, -1000000, 1000000> },
	{ "weaponTemplate.explosionRadius", 1, 1, weapon_float<&WeaponTemplate::explosion_radius, -100000, 100000> },
	{ "weaponTemplate.allowedDeviation", 1, 1, weapon_float<&WeaponTemplate::allowed_deviation, -180, 180> },
	{ "weaponTemplate.firesThroughTransparent", 1, 1, weapon_bool<&WeaponTemplate::fires_through_transparent> },
};

const Command *find_command(std::string_view p_name) {
	const auto found = std::find_if(std::begin(kCommands), std::end(kCommands),
			[&](const Command &p_c) { return core::iequals_ascii(p_c.name, p_name); });
	return found != std::end(kCommands) ? &*found : nullptr;
}

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
	return find_weapon_template(weapon_templates, p_name);
}

const WeaponTemplate &find_weapon_template(const WeaponTemplateMap &p_templates, std::string_view p_weapon) {
	static const WeaponTemplate kDefault;
	const auto exact = p_templates.find(p_weapon);
	const WeaponTemplate *found = exact != p_templates.end() ? &exact->second : find_named(p_templates, p_weapon);
	return found != nullptr ? *found : kDefault;
}

const StrategicAreaDef *AiConfig::strategic_area(std::string_view p_name) const {
	const auto found = std::find_if(strategic_areas.begin(), strategic_areas.end(),
			[&](const StrategicAreaDef &p_area) { return p_area.name == p_name; });
	return found != strategic_areas.end() ? &*found : nullptr;
}

std::filesystem::path level_ai_script(const std::filesystem::path &p_root, std::string_view p_level) {
	return p_root / "Levels" / std::filesystem::path(std::u8string(p_level.begin(), p_level.end())) / "AI" / "AI.ai";
}

std::filesystem::path level_strategic_areas_script(const std::filesystem::path &p_root, std::string_view p_level) {
	return level_ai_script(p_root, p_level).parent_path() / kStrategicAreasScript;
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
		const Command *command = find_command(words[0]);
		if (command == nullptr) {
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

std::vector<StrategicAreaDef> strategic_areas_by_name(std::span<const StrategicAreaDef> p_areas) {
	std::vector<StrategicAreaDef> out(p_areas.begin(), p_areas.end());
	std::stable_sort(out.begin(), out.end(),
			[](const StrategicAreaDef &p_a, const StrategicAreaDef &p_b) { return p_a.name < p_b.name; });
	return out;
}

std::string format_strategic_areas(std::span<const StrategicAreaDef> p_areas) {
	std::string areas = "rem *** Create strategic areas ***\n", assignments;
	for (const StrategicAreaDef &a : strategic_areas_by_name(p_areas)) {
		if (a.from_control_point) {
			areas += "aiStrategicArea.createFromControlPoint " + a.name + " " + std::to_string(a.control_point) + " " +
					core::float_text(a.radius);
		} else {
			areas += "aiStrategicArea.create " + a.name + " " + core::float_text(a.corner[0]) + "/" +
					core::float_text(a.corner[1]) + " " + core::float_text(a.centre[0]) + "/" +
					core::float_text(a.centre[1]) + " " + core::float_text(a.height) + " " + core::float_text(a.radius);
		}
		areas += (a.category.empty() ? "" : " " + a.category) + "\n";
		areas += "aiStrategicArea.layer " + std::to_string(a.layer) + "\n\n";
		assignments += "aiStrategicArea.setActive " + a.name + "\n";
		for (const std::string &n : a.neighbours) {
			assignments += "aiStrategicArea.addNeighbour " + n + "\n";
		}
		assignments += "aiStrategicArea.setSide " + std::to_string(a.side) + "\n";
		for (size_t side = 0; side < a.takeable.size(); ++side) {
			if (!a.takeable[side]) {
				assignments += "aiStrategicArea.setTakeable " + std::to_string(side + 1u) + " 0\n";
			}
		}
		assignments += "\n";
	}
	return areas + assignments;
}

std::string format_weapon_template(std::string_view p_name, const WeaponTemplate &p_template) {
	const WeaponTemplate def;
	const WeaponTemplate &w = p_template;
	const std::string prefix = "weaponTemplate.";
	std::string out = prefix + "create " + std::string(p_name) + "\n";
	const auto real = [&](std::string_view p_field, float p_value, float p_default) {
		if (p_value != p_default) {
			out += prefix + std::string(p_field) + " " + core::float_text(p_value) + "\n";
		}
	};
	const auto flag = [&](std::string_view p_field, bool p_value, bool p_default) {
		if (p_value != p_default) {
			out += prefix + std::string(p_field) + (p_value ? " 1\n" : " 0\n");
		}
	};
	real("deviation", w.deviation, def.deviation);
	real("deviationCorrectionTime", w.deviation_correction_time, def.deviation_correction_time);
	flag("indirect", w.indirect, def.indirect);
	flag("useAimerOnly", w.use_aimer_only, def.use_aimer_only);
	real("drag", w.drag, def.drag);
	real("minRange", w.min_range, def.min_range);
	real("maxRange", w.max_range, def.max_range);
	if (w.weapon_activate != def.weapon_activate) {
		out += prefix + "weaponActivate " + std::string(player_input_name(w.weapon_activate)) + "\n";
	}
	if (w.weapon_fire != def.weapon_fire) {
		out += prefix + "weaponFire " + std::string(player_input_name(w.weapon_fire)) + "\n";
	}
	flag("isThrown", w.is_thrown, def.is_thrown);
	real("soundSphereRadius", w.sound_sphere_radius, def.sound_sphere_radius);
	real("exitVelocity", w.exit_velocity, def.exit_velocity);
	for (size_t i = 0; i < kCombatStrengthTypeCount; ++i) {
		if (w.strengths[i] != def.strengths[i]) {
			out += prefix + "setStrength " + std::string(kStrengthTypeNames[i]) + " " + std::to_string(w.strengths[i]) +
					"\n";
		}
	}
	if (w.fire_rate != def.fire_rate) {
		out += prefix + "fireRate " + std::to_string(w.fire_rate) + "\n";
	}
	if (w.firing_pose != def.firing_pose) {
		out += prefix + "firingPose " + std::string(kSoldierPoseNames[size_t(w.firing_pose)]) + "\n";
	}
	real("optimalRangePercentage", w.optimal_range_percentage, def.optimal_range_percentage);
	real("explosionRadius", w.explosion_radius, def.explosion_radius);
	real("allowedDeviation", w.allowed_deviation, def.allowed_deviation);
	flag("firesThroughTransparent", w.fires_through_transparent, def.fires_through_transparent);
	return out;
}

core::Status run_weapon_template_files(const std::filesystem::path &p_root, AiConfig &r_config) {
	const std::filesystem::path dir = p_root / kWeaponTemplatesDir;
	std::error_code ec;
	if (!std::filesystem::is_directory(dir, ec)) {
		return core::Status::success();
	}
	std::vector<std::filesystem::path> files;
	for (const std::filesystem::directory_entry &e : std::filesystem::directory_iterator(dir, ec)) {
		if (e.is_regular_file(ec) && core::iequals_ascii(e.path().extension().string(), ".ai")) {
			files.push_back(e.path());
		}
	}
	if (ec) {
		return core::make_error("ai scripts: cannot list " + dir.generic_string());
	}
	std::sort(files.begin(), files.end());
	for (const std::filesystem::path &f : files) {
		BF3_TRY(run_ai_script_file(f, r_config));
	}
	return core::Status::success();
}

void add_generated_weapon_templates(const WeaponTemplateMap &p_generated, AiConfig &r_config) {
	for (const auto &[name, w] : p_generated) {
		if (find_named(r_config.weapon_templates, name) == nullptr) {
			r_config.weapon_templates.emplace(name, w);
		}
	}
}

core::Result<AiConfig> load_level_ai_config(const std::filesystem::path &p_root, std::string_view p_level,
		std::string_view p_derived_areas, const WeaponTemplateMap &p_generated_weapons) {
	AiConfig config;
	BF3_TRY(run_ai_script_file(p_root / kAiDefaultScript, config));
	BF3_TRY(run_ai_script_file(p_root / kAiBehavioursScript, config));
	const std::filesystem::path areas = level_strategic_areas_script(p_root, p_level);
	std::error_code ec;
	if (std::filesystem::is_regular_file(areas, ec)) {
		BF3_TRY(run_ai_script_file(areas, config));
	} else {
		BF3_TRY(run_ai_script(p_derived_areas, "derived:" + areas.generic_string(), config));
	}
	BF3_TRY(run_weapon_template_files(p_root, config));
	add_generated_weapon_templates(p_generated_weapons, config);
	const std::filesystem::path level = level_ai_script(p_root, p_level);
	if (std::filesystem::is_regular_file(level, ec)) {
		BF3_TRY(run_ai_script_file(level, config));
	}
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
