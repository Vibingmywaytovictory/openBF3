#pragma once

#include "bf3/core/result.h"
#include "bf3/host/game_server.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/command.h"
#include "bf3/sim/sim_types.h"
#include "bf3/sim/soldier_state.h"

#include <cstdint>

namespace bf3::host {

struct TargetBotConfig {
	sim::Pose pose = sim::Pose::Stand;
	uint16_t yaw = 0;
	int16_t pitch = 0;
};

class TargetBotSource final : public CommandSource {
public:
	explicit TargetBotSource(const TargetBotConfig &p_config);
	sim::Command command(sim::Tick p_tick, const sim::SoldierState &p_state) override;

	void set(const TargetBotConfig &p_config);
	const TargetBotConfig &config() const { return config_; }

private:
	TargetBotConfig config_;
};

inline constexpr float kSkyLaneHeight = 40.0f;
inline constexpr float kSkyLaneStart = 10.0f;
inline constexpr float kSkyLaneStep = 32.0f;
inline constexpr float kSkyLaneClearRay = 1000.0f;
inline constexpr float kSkyLaneSpare = 75.0f;
inline constexpr float kSkyLaneEyeHeight = 1.6f;

struct SkyLane {
	sim::Vec3 eye{};
	sim::Vec3 feet{};
	uint16_t yaw = 0;
	float clear_length = 0;
	float in_bounds_length = 0;
};

core::Result<SkyLane> find_sky_lane(const sim::CollisionWorld &p_world, float p_flight_m, float p_margin_m);

}
