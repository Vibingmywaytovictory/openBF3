#pragma once

#include "bf3/sim/collision_world.h"
#include "bf3/sim/soldier_motor.h"
#include "bf3/sim/spawn.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace bf3::sim {

inline constexpr float kVaultSpotRunUp = 3.0f;
inline constexpr float kVaultSpotReach = 8.0f;
inline constexpr float kVaultSpotLowest = 0.4f;
inline constexpr float kVaultSpotHighest = 1.2f;
inline constexpr float kVaultSpotFaceSpread = 0.1f;
inline constexpr float kVaultSpotFacing = 0.9f;
inline constexpr float kVaultSpotTopUp = 0.7f;
inline constexpr float kVaultSpotClearBeyond = 1.5f;
inline constexpr size_t kVaultSpotsPerPoint = 2;

struct VaultSpot {
	size_t point = 0;
	Vec3 feet{};
	uint16_t yaw = 0;
	float face = 0.0f;
	float top = 0.0f;
};

std::vector<VaultSpot> find_vault_spots(const SoldierMotor &p_motor, const CollisionWorld &p_world,
		std::span<const SpawnPoint> p_points, size_t p_limit);

float vault_spot_along(const VaultSpot &p_spot, const Vec3 &p_at);

inline constexpr float kVaultSpotRayMiss = -1.0f;

struct VaultSpotPress {
	bool grounded = false;
	uint8_t mode = 0;
	uint8_t pose = 0;
	uint16_t transition = 0;
	float speed = 0.0f, reach = 0.0f, face = 0.0f;
	std::array<float, 5> rays{};
	float front = 0.0f, further = 0.0f;
};

struct VaultSpotRun {
	VaultSpotPress press;
	bool pressed = false;
	bool vaulted = false;
	bool ended = false;
	bool stopped = false;
	bool crossed = false;
	uint8_t kind = 0;
	uint8_t variant = 0;
	float distance = 0.0f, height = 0.0f, length = 0.0f;
	uint32_t ticks = 0;
	float highest = 0.0f;
};

bool vault_spot_crossed(const VaultSpot &p_spot, const Vec3 &p_at);

VaultSpotRun run_at_vault_spot(const SoldierMotor &p_motor, const CollisionWorld &p_world, const VaultSpot &p_spot,
		float p_press_at, bool p_sprint);

}
