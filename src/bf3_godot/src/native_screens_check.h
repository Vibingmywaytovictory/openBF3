#pragma once

#include "bf3/core/ui/ui_input.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace bf3::godot_side {

class Bf3UiScreens;
class NativeFlow;

inline constexpr double kNativeScreensCheckSeconds = 90.0;
inline constexpr double kNativeScreensCheckHoldSeconds = 1.0;
inline constexpr size_t kNativeScreensCheckMaxLines = 8192;

class NativeScreensCheck {
public:
	bool frame(Bf3UiScreens &r_screens, NativeFlow &r_flow, double p_now_s);
	std::string report() const;
	int exit_code() const { return passed_ ? 0 : 1; }

private:
	using Concept = bf3::core::ui::UiInputConcept;
	struct Step {
		const char *line;
		std::optional<Concept> press;
		bool sent = false;
		double seen_s = -1.0;
	};
	void take(std::string p_line, double p_now_s);
	void collect(Bf3UiScreens &r_screens, NativeFlow &r_flow, double p_now_s);
	void advance(double p_now_s);

	std::vector<Step> steps_{ { "screens entered spawnscreenpc", std::nullopt },
		{ "flow action Spawn(", Concept::Activate }, { "screens exited spawnscreenpc", std::nullopt },
		{ "screens entered hudmpscreen", std::nullopt }, { "screens entered ingamemenump", Concept::Menu },
		{ "screens exited ingamemenump", Concept::Deactivate }, { "screens entered hudmpscreen", std::nullopt } };
	size_t next_ = 0;
	size_t cursor_ = 0;
	double start_s_ = -1.0;
	bool done_ = false;
	bool passed_ = false;
	std::vector<std::string> lines_;
	std::string failure_;
	uint64_t actions_ = 0;
	double load_seconds_ = 0.0;
};

}
