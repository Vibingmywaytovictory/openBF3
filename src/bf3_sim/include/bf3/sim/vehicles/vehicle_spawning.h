#pragma once

#include <cstdint>

namespace bf3::sim::vehicles {

constexpr float vehicle_spawn_delay(
		uint32_t p_spawned, float p_initial_spawn_delay, float p_spawn_delay, float p_delay_modifier) {
	return (p_spawned > 0 ? p_spawn_delay : p_initial_spawn_delay) * p_delay_modifier;
}

}
