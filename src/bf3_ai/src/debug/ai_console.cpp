#include "bf3/ai/debug/ai_console.h"

#include "bf3/ai/main/ai_main.h"
#include "bf3/ai/main/bot_placement_manager.h"
#include "bf3/core/util/key_values.h"

#include <cstdint>
#include <sstream>

namespace bf3::ai {

namespace {

bool parse_number(const std::string &p_text, uint32_t &r_out) {
	return core::parse_u32_value(p_text, 0.0, double(UINT32_MAX), r_out).fault == core::KeyFault::None;
}

bool parse_number(const std::string &p_text, float &r_out) {
	return core::parse_float_value(p_text, -1.0e30, 1.0e30, r_out).fault == core::KeyFault::None;
}

bool parse_number(const std::string &p_text, double &r_out) {
	return core::parse_finite(p_text, r_out);
}

std::string format(int p_value) {
	return std::to_string(p_value);
}

std::string format(uint32_t p_value) {
	return std::to_string(p_value);
}

std::string format(double p_value) {
	return core::float_text(p_value);
}

ConsoleResult bad_argument(const std::string &p_text) {
	return { false, "bad argument: " + p_text };
}

template <class T, class F> ConsoleResult with_numbers(std::span<const std::string> p_args, F &&p_apply) {
	std::vector<T> values;
	for (const std::string &arg : p_args) {
		T value{};
		if (!parse_number(arg, value)) {
			return bad_argument(arg);
		}
		values.push_back(value);
	}
	return p_apply(values);
}

bool parse_vec3(const std::string &p_text, Vec3 &r_out) {
	const size_t first = p_text.find('/');
	const size_t second = first == std::string::npos ? first : p_text.find('/', first + 1);
	if (second == std::string::npos) {
		return false;
	}
	return parse_number(p_text.substr(0, first), r_out.x) &&
			parse_number(p_text.substr(first + 1, second - first - 1), r_out.y) &&
			parse_number(p_text.substr(second + 1), r_out.z);
}

std::string format(bool p_value) {
	return p_value ? "1" : "0";
}

template <class T> AiConsole::Handler getter(std::function<T()> p_read) {
	return [p_read](std::span<const std::string>) { return ConsoleResult{ true, format(p_read()) }; };
}

}

void AiConsole::add(const std::string &p_name, size_t p_min_args, size_t p_max_args, Handler p_handler) {
	commands_[p_name] = Command{ p_min_args, p_max_args, std::move(p_handler) };
}

ConsoleResult AiConsole::execute(std::string_view p_line) const {
	std::istringstream in{ std::string(p_line) };
	std::string name;
	if (!(in >> name)) {
		return { false, "empty command" };
	}
	std::vector<std::string> args;
	for (std::string arg; in >> arg;) {
		args.push_back(arg);
	}
	const auto found = commands_.find(name);
	if (found == commands_.end()) {
		return { false, "unknown command: " + name };
	}
	const Command &command = found->second;
	if (args.size() < command.min_args || args.size() > command.max_args) {
		return { false,
			name + ": takes " + std::to_string(command.min_args) + ".." + std::to_string(command.max_args) +
					" arguments" };
	}
	return command.handler(args);
}

std::vector<std::string> AiConsole::names() const {
	std::vector<std::string> out;
	for (const auto &entry : commands_) {
		out.push_back(entry.first);
	}
	return out;
}

void register_main_commands(AiConsole &r_console, AIMain &r_main, const LodEnvironment *p_lod_environment) {
	AIMain *const main = &r_main;
	BotManager *const bots = &r_main.bot_manager();
	r_console.add("getBotCount", 0, 0, getter<int>([bots] { return bots->bot_count(); }));
	r_console.add("getNumberOfActiveBots", 0, 0, getter<int>([bots] { return bots->number_of_active_bots(); }));
	r_console.add("getNumberOfDeactivedBots", 0, 0, getter<int>([bots] { return bots->number_of_deactivated_bots(); }));
	r_console.add("aiSystemQuotient", 0, 0, getter<double>([main] { return main->ai_system_quotient(); }));
	r_console.add("setLodLevelTicks", 3, 3, [bots](std::span<const std::string> p_args) {
		return with_numbers<uint32_t>(p_args, [bots](const std::vector<uint32_t> &p_v) {
			bots->set_lod_level_ticks(p_v[0], p_v[1], p_v[2]);
			return ConsoleResult{ true, {} };
		});
	});
	r_console.add("setLodLevelPriority", 3, 3, [bots](std::span<const std::string> p_args) {
		return with_numbers<uint32_t>(p_args, [bots](const std::vector<uint32_t> &p_v) {
			bots->set_lod_level_priority(p_v[0], p_v[1], p_v[2]);
			return ConsoleResult{ true, {} };
		});
	});
	r_console.add("setPlannedDecisionMakingThreshold", 3, 3, [bots](std::span<const std::string> p_args) {
		return with_numbers<double>(p_args, [bots](const std::vector<double> &p_v) {
			bots->set_planned_decision_making_threshold(p_v[0], p_v[1], p_v[2]);
			return ConsoleResult{ true, {} };
		});
	});
	r_console.add("setUnplannedDecisionMakingThreshold", 3, 3, [bots](std::span<const std::string> p_args) {
		return with_numbers<double>(p_args, [bots](const std::vector<double> &p_v) {
			bots->set_unplanned_decision_making_threshold(p_v[0], p_v[1], p_v[2]);
			return ConsoleResult{ true, {} };
		});
	});
	r_console.add("setDecisionMakingInterleave", 2, 2, [bots](std::span<const std::string> p_args) {
		return with_numbers<uint32_t>(p_args, [bots](const std::vector<uint32_t> &p_v) {
			bots->set_decision_making_interleave(p_v[0], p_v[1]);
			return ConsoleResult{ true, {} };
		});
	});
	r_console.add("setSensingQuotient", 2, 2, [bots](std::span<const std::string> p_args) {
		return with_numbers<uint32_t>(p_args, [bots](const std::vector<uint32_t> &p_v) {
			bots->set_sensing_quotient(p_v[0], p_v[1]);
			return ConsoleResult{ true, {} };
		});
	});
	r_console.add("setSystemQuotient", 3, 3, [bots](std::span<const std::string> p_args) {
		return with_numbers<uint32_t>(p_args, [bots](const std::vector<uint32_t> &p_v) {
			bots->set_system_quotient(p_v[0], p_v[1], p_v[2]);
			return ConsoleResult{ true, {} };
		});
	});
	const std::pair<const char *, BotLodLevel> levels[] = { { "Full", BotLodLevel::Full },
		{ "Medium", BotLodLevel::Medium }, { "Light", BotLodLevel::Light } };
	for (const auto &[suffix, level] : levels) {
		const BotLodLevel l = level;
		const std::string tail(suffix);
		r_console.add(
				"getLodLevel" + tail + "Ticks", 0, 0, getter<uint32_t>([bots, l] { return bots->lod_level_ticks(l); }));
		r_console.add("getLodLevel" + tail + "Priority", 0, 0,
				getter<uint32_t>([bots, l] { return bots->lod_level_priority(l); }));
		r_console.add("getPlannedDecisionMakingThreshold" + tail, 0, 0,
				getter<double>([bots, l] { return bots->planned_decision_making_threshold(l); }));
		r_console.add("getUnplannedDecisionMakingThreshold" + tail, 0, 0,
				getter<double>([bots, l] { return bots->unplanned_decision_making_threshold(l); }));
	}
	r_console.add("getPlannedDecisionMakingInterleave", 0, 0,
			getter<uint32_t>([bots] { return bots->planned_decision_making_interleave(); }));
	r_console.add("getUnplannedDecisionMakingInterleave", 0, 0,
			getter<uint32_t>([bots] { return bots->unplanned_decision_making_interleave(); }));
	r_console.add("getSensingQuotient", 0, 0, getter<double>([bots] { return bots->sensing_quotient(); }));
	r_console.add("getMemoryUpdateQuotient", 0, 0, getter<double>([bots] { return bots->memory_update_quotient(); }));
	r_console.add("getSystemDecisionMakingQuotient", 0, 0,
			getter<double>([bots] { return bots->system_decision_making_quotient(); }));
	r_console.add("getSystemPathfindingQuotient", 0, 0,
			getter<double>([bots] { return bots->system_pathfinding_quotient(); }));
	r_console.add("getSystemSensingQuotient", 0, 0, getter<double>([bots] { return bots->system_sensing_quotient(); }));
	if (p_lod_environment != nullptr) {
		r_console.add("createLODManager", 2, 2, [main, p_lod_environment](std::span<const std::string> p_args) {
			return with_numbers<float>(p_args, [main, p_lod_environment](const std::vector<float> &p_v) {
				const bool made = main->create_lod_manager(*p_lod_environment, p_v[0], p_v[1]) != nullptr;
				return ConsoleResult{ made, made ? "LOD manager created" : "no information manager" };
			});
		});
	}
	r_console.add("lodEnable", 0, 1, [main](std::span<const std::string> p_args) {
		AILODManager *const lod = main->lod_manager();
		if (lod == nullptr) {
			return ConsoleResult{ false, "no LOD manager" };
		}
		if (p_args.empty()) {
			return ConsoleResult{ true, format(int(lod->lod_enabled())) };
		}
		uint32_t flag = 0;
		bool enable = false;
		if (core::parse_u32_value(p_args[0], 0.0, 1.0, flag).fault == core::KeyFault::None) {
			enable = flag != 0;
		} else if (core::parse_bool_value(p_args[0], enable).fault != core::KeyFault::None) {
			return bad_argument(p_args[0]);
		}
		lod->lod_enable(enable);
		return ConsoleResult{ true, {} };
	});
	r_console.add("setBasicLODDistance", 1, 1, [main](std::span<const std::string> p_args) {
		AILODManager *const lod = main->lod_manager();
		if (lod == nullptr) {
			return ConsoleResult{ false, "no LOD manager" };
		}
		return with_numbers<float>(p_args, [lod](const std::vector<float> &p_v) {
			lod->set_basic_lod_distance(p_v[0]);
			return ConsoleResult{ true, {} };
		});
	});
}

void register_placement_commands(AiConsole &r_console, BotPlacementManager &r_placements) {
	BotPlacementManager *const placements = &r_placements;
	r_console.add("addPlacement", 6, 6, [placements](std::span<const std::string> p_args) {
		Vec3 position;
		Vec3 rotation;
		int32_t team = 0;
		if (!parse_vec3(p_args[1], position)) {
			return bad_argument(p_args[1]);
		}
		if (!parse_vec3(p_args[2], rotation)) {
			return bad_argument(p_args[2]);
		}
		if (core::parse_i32_value(p_args[4], -2147483648.0, 2147483647.0, team).fault != core::KeyFault::None) {
			return bad_argument(p_args[4]);
		}
		return ConsoleResult{ true,
			format(placements->add_placement(p_args[0], position, rotation, p_args[3], team, p_args[5])) };
	});
	r_console.add("spawnBot", 1, 1, [placements](std::span<const std::string> p_args) {
		int32_t team = 0;
		if (core::parse_i32_value(p_args[0], -2147483648.0, 2147483647.0, team).fault != core::KeyFault::None) {
			return bad_argument(p_args[0]);
		}
		return ConsoleResult{ true, format(placements->spawn_bot(team)) };
	});
}

}
