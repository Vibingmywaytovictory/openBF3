#pragma once

#include "bf3/fx/effect_def.h"
#include "bf3/fx/simulator.h"

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace bf3::fx {

inline constexpr uint32_t kNoPlacedLogicNode = 0xFFFFFFFFu;

struct DrawableEffect {
	std::shared_ptr<const EffectDef> def;
	uint32_t mesh_emitters = 0;
};

DrawableEffect without_mesh_emitters(const std::shared_ptr<const EffectDef> &p_def);


struct PlacedEffectStats {
	uint32_t placed = 0;
	uint32_t auto_start = 0;
	uint32_t without_def = 0;
	uint32_t in_range = 0;
	uint32_t running = 0;
	uint32_t ended_in_range = 0;
	uint32_t mesh_emitters_undrawn = 0;
	uint32_t started_placements = 0;
	uint32_t logic_bound = 0;
	uint32_t logic_started_placements = 0;
	uint64_t logic_starts = 0;
	uint64_t logic_stops = 0;
	uint64_t starts = 0;
	uint64_t stops = 0;
	uint64_t start_failures = 0;
};

class PlacedEffects {
public:
	void add(std::shared_ptr<const EffectDef> p_def, const Transform &p_transform, bool p_auto_start);
	void step(FxWorld &r_world, const FxView &p_view, uint8_t p_tier, uint64_t p_seed);
	void bind_logic(std::span<const uint32_t> p_nodes);
	void logic_event(uint32_t p_node, bool p_start);
	void logic_reset();
	void forget();
	void set_undrawn_mesh_emitters(uint32_t p_count) { stats_.mesh_emitters_undrawn = p_count; }
	const PlacedEffectStats &stats() const { return stats_; }

private:
	struct Slot {
		std::shared_ptr<const EffectDef> def;
		Transform transform;
		bool auto_start = false;
		bool wanted = false;
		bool finished = false;
		bool started = false;
		bool logic_started = false;
		uint32_t logic_node = kNoPlacedLogicNode;
		EffectHandle handle;
	};
	std::vector<Slot> slots_;
	PlacedEffectStats stats_;
};

}
