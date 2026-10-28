#pragma once

#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/eor_fading.h"
#include "bf3/core/gameplay/logic_graph.h"
#include "bf3/core/result.h"
#include "bf3/sim/logic/logic_runtime.h"
#include "bf3/sim/sim_types.h"

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bf3::host {

std::optional<core::EbxInstance> logic_node_instance(
		const core::EbxResolver &p_db, const core::gameplay::LogicGraph &p_graph, uint32_t p_node);
std::optional<double> logic_node_float(const core::EbxInstance &p_instance, std::string_view p_field);

struct LogicFadeRamp {
	float from = 0.0f;
	float to = 0.0f;
	sim::Tick start = 0;
	uint32_t ticks = 0;
	float at(sim::Tick p_tick) const;
};

enum class LogicVisibilitySource : uint8_t { Instance, Sequence, Runtime, Unresolved };

struct LogicEnvironmentState {
	uint32_t node = 0;
	std::string visual_environment;
	bool enabled = false;
	float visibility = 0.0f;
	LogicVisibilitySource source = LogicVisibilitySource::Instance;
};

struct LogicScreenEffectsFrame {
	float screen_faded = 0.0f;
	float ui_faded = 0.0f;
	std::vector<LogicEnvironmentState> environments;
	const LogicEnvironmentState *environment(std::string_view p_visual_environment) const;
};

struct LogicScreenEffectsStats {
	size_t fades = 0;
	size_t fade_times_from_properties = 0;
	size_t environments = 0;
	size_t tracked_visibilities = 0;
	size_t linked_visibilities = 0;
	uint64_t fade_events = 0;
	uint64_t environment_events = 0;
	uint64_t sequence_starts = 0;
};

class LogicScreenEffects {
public:
	static core::Result<LogicScreenEffects> bind(
			const core::EbxResolver &p_db, const core::gameplay::LogicGraph &p_graph);

	std::vector<std::pair<uint32_t, int32_t>> watch_ports() const;
	std::optional<uint32_t> event(uint32_t p_node, int32_t p_port, sim::Tick p_tick);
	void reset();
	LogicScreenEffectsFrame frame(sim::Tick p_tick, const sim::logic::LogicRuntime *p_runtime) const;
	const LogicScreenEffectsStats &stats() const { return stats_; }

private:
	struct Fade {
		bool screen = false;
		bool ui = false;
		uint32_t ticks = 0;
	};
	struct Environment {
		std::string visual_environment;
		float visibility = 0.0f;
		uint32_t sequence = core::gameplay::kNoLogicIndex;
		std::vector<core::gameplay::EorFadingKey> track;
		uint32_t linked = core::gameplay::kNoLogicIndex;
		int32_t linked_port = 0;
		bool enabled = false;
	};

	std::map<uint32_t, Fade> fades_;
	std::map<uint32_t, Environment> environments_;
	std::map<uint32_t, sim::Tick> sequence_starts_;
	LogicFadeRamp screen_;
	LogicFadeRamp ui_;
	LogicScreenEffectsStats stats_;
};

}
