#pragma once
// Stationary target bots and the dev range helpers (docs/design/m2a.md §1 setup, §10.1; owner sv). The M2a
// criterion's "bot targets": S1's bot fill adds them (add_local_soldier, spawn_in_main), TargetBotSource writes an
// idle command with a fixed pose and facing, and the checks place them with the dev request SpawnTarget (applied in
// the rules step, clears spawn protection; the same code as GameServer::place_soldier) and read their server
// capsules with TargetInfo. Server thread only; the helpers are dev only.

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
	uint16_t yaw = 0; // Command quantisation
	int16_t pitch = 0;
};

class TargetBotSource final : public CommandSource {
public:
	explicit TargetBotSource(const TargetBotConfig &p_config);
	sim::Command command(sim::Tick p_tick, const sim::SoldierState &p_state) override;
	// Changes the pose and facing from the next command (SpawnTarget sets them with the placement).
	void set(const TargetBotConfig &p_config);
	const TargetBotConfig &config() const { return config_; }

private:
	TargetBotConfig config_;
};

// The sky lane of E2 (m2a.md §1): along the static world bounds' longest horizontal axis, starting at the bounds'
// minimum on that axis + 10 m, kSkyLaneHeight above the bounds' top, heading along the axis; lateral offsets across
// the bounds in kSkyLaneStep steps until a ray of kSkyLaneClearRay is clear. Accepted only when the in-bounds length
// (to the far bound + p_margin_m, ballistics.h world_margin) is >= p_flight_m + kSkyLaneSpare (M16A3: 975 + 75).
inline constexpr float kSkyLaneHeight = 40.0f;
inline constexpr float kSkyLaneStart = 10.0f;
inline constexpr float kSkyLaneStep = 32.0f;
inline constexpr float kSkyLaneClearRay = 1000.0f;
inline constexpr float kSkyLaneSpare = 75.0f;
inline constexpr float kSkyLaneEyeHeight = 1.6f; // sv: the stand eye above the teleport feet (Metro's SoldierDef)

struct SkyLane {
	sim::Vec3 eye{}; // the teleport point's eye
	sim::Vec3 feet{};
	uint16_t yaw = 0; // Command quantisation, along the axis
	float clear_length = 0; // metres of the clear ray (>= kSkyLaneClearRay when accepted)
	float in_bounds_length = 0; // to the far bound + margin
};
// Fails with a message naming both lengths when no lane holds the flight (E2's expiry row then uses wp's W2 world).
core::Result<SkyLane> find_sky_lane(const sim::CollisionWorld &p_world, float p_flight_m, float p_margin_m);

} // namespace bf3::host
