#pragma once

#include "bf3/sim/sim_types.h"
#include "bf3/sim/soldier_state.h"

#include <cstdint>
#include <span>
#include <vector>

namespace bf3::sim {
class CollisionWorld;
}

namespace bf3::sim::bots {

struct PerceptionParams {
	float fov_h_deg = 160.0f;
	float fov_v_deg = 90.0f;
	float max_range_m = 120.0f;
	uint32_t update_ticks = 6;
	uint32_t max_candidates = 4;
	float near_m = 10.0f;
	float detect_near_s = 0.2f;
	float far_m = 100.0f;
	float detect_far_s = 1.5f;
	float forget_s = 3.0f;
	float hearing_radius_m = 60.0f;
	float hearing_error_m = 5.0f;
	float head_height_m = 1.6f;
	float chest_height_m = 1.3f;
};

struct HeardShot {
	uint8_t shooter = 0;
	Vec3 origin{};
	Tick tick = 0;
};

struct PerceivedEnemy {
	uint8_t slot = 0;
	float detection = 0.0f;
	bool visible = false;
	bool detected = false;
	bool heard = false;
	Tick last_seen = 0;
	Vec3 last_position{};

	Tick last_visible = 0;
	bool ever_visible = false;
	Vec3 last_velocity{};
	bool chest_visible = false;
};

struct PerceptionInput {
	uint8_t self = 0;
	uint8_t team = 0;
	Tick tick = 0;
	Vec3 eye{};
	uint16_t yaw = 0;
	int16_t pitch = 0;
	std::span<const SoldierState> states;
	std::span<const uint8_t> teams;
	std::span<const uint8_t> alive;
	std::span<const uint8_t> firing;
	std::span<const HeardShot> shots;
	const CollisionWorld *world = nullptr;
	uint64_t seed = 0;

	float eye_heights[3] = { 1.6f, 1.0f, 0.3f };
	uint32_t ray_mask = 0;
};

bool perception_tick(uint8_t p_slot, Tick p_tick, uint32_t p_update_ticks);

class Perception {
public:
	explicit Perception(const PerceptionParams &p_params = {});
	void reset();

	uint32_t update(const PerceptionInput &p_input);
	std::span<const PerceivedEnemy> enemies() const;
	const PerceivedEnemy *find(uint8_t p_slot) const;
	const PerceptionParams &params() const;

	std::span<const uint8_t> newly_detected() const;
	uint32_t heard_last() const;

private:
	PerceptionParams params_;
	std::vector<PerceivedEnemy> enemies_;
	std::vector<uint8_t> newly_;
	uint32_t heard_ = 0;
};

float detection_time(const PerceptionParams &p_params, float p_distance);

Vec3 perception_head(const PerceptionParams &p_params, const SoldierState &p_state, const float p_eye_heights[3]);
Vec3 perception_chest(const PerceptionParams &p_params, const SoldierState &p_state, const float p_eye_heights[3]);

}
