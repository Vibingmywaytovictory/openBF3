#pragma once

#include <functional>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::ai {

class AIMain;
class BotPlacementManager;
class LodEnvironment;

struct ConsoleResult {
	bool ok = false;
	std::string text;
};

class AiConsole {
public:
	using Handler = std::function<ConsoleResult(std::span<const std::string>)>;

	void add(const std::string &p_name, size_t p_min_args, size_t p_max_args, Handler p_handler);
	ConsoleResult execute(std::string_view p_line) const;
	std::vector<std::string> names() const;

private:
	struct Command {
		size_t min_args = 0;
		size_t max_args = 0;
		Handler handler;
	};
	std::map<std::string, Command, std::less<>> commands_;
};

void register_main_commands(AiConsole &r_console, AIMain &r_main, const LodEnvironment *p_lod_environment);
void register_placement_commands(AiConsole &r_console, BotPlacementManager &r_placements);
}
