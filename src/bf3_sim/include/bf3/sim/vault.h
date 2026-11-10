#pragma once

#include "bf3/sim/sim_types.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>

namespace bf3::sim {

class CollisionWorld;

enum class VaultKind : uint8_t { OverHigh = 0, UpHigh = 1, OverLow = 2 };
inline constexpr size_t kVaultKindCount = 3;
inline constexpr size_t kMaxMoveKeys = 12;
inline constexpr size_t kMaxVaultVariants = 4;
inline constexpr uint8_t kNoMovePose = 0xFF;
inline constexpr uint8_t kPlainVaultMove = 0xFF;

struct MoveKey {
	Vec3 value{};
	float time = 0;
};

struct SpecialMoveParams {
	std::array<MoveKey, kMaxMoveKeys> motion{};
	std::array<MoveKey, kMaxMoveKeys> gravity{};
	uint8_t motion_count = 0;
	uint8_t gravity_count = 0;
	uint8_t override_pose = kNoMovePose;
	uint8_t reset_pose = kNoMovePose;
	bool scale_with_distance = false;
	bool keep_initial_direction = false;
	float optimum_distance = 0, optimum_height = 0, optimum_length = 0;
	float min_distance_scale = 1, max_distance_scale = 1;
	float min_height_scale = 1, max_height_scale = 1;
	float min_length_scale = 1, max_length_scale = 1;
	float exit_velocity = 0;
};

struct VaultVariant {
	float distance = 0, distance_tolerance = 0;
	float height = 0, height_tolerance = 0;
	float length = 0, length_tolerance = 0;
	SpecialMoveParams move;
};

struct VaultParams {
	bool present = false;
	std::array<SpecialMoveParams, kVaultKindCount> plain{};
	std::array<std::array<VaultVariant, kMaxVaultVariants>, kVaultKindCount> variants{};
	std::array<uint8_t, kVaultKindCount> variant_count{};
};
static_assert(std::is_trivially_copyable_v<VaultParams>);

struct VaultMeasure {
	float distance = 0;
	float height = 0;
	float length = 0;
};

struct VaultStart {
	VaultKind kind = VaultKind::OverHigh;
	VaultMeasure measure;
};

namespace vault_rules {
inline constexpr float kMinForwardInput = 0.2f;
inline constexpr float kMiddleHeight = 0.75f;
inline constexpr float kHighOffset = 0.5f;
inline constexpr float kMiddleOffset = -0.2f;
inline constexpr float kLowOffset = -0.4f;
inline constexpr float kProbeReach = 0.5f;
inline constexpr float kDownProbe = 1.8f;
inline constexpr float kMinHeight = 0.1f;
inline constexpr float kFrontEpsilon = 1e-6f;
inline constexpr float kUpLength = 0.25f;
inline constexpr float kUpTolerance = 0.2f;
inline constexpr float kSideOffset = 0.35f;
inline constexpr float kBackSearch = 4.0f;
inline constexpr float kMinLength = 0.05f;
inline constexpr float kPhaseMargin = 0.2f;
inline constexpr float kOptimumFloor = 1e-6f;
inline constexpr float kContactSqrDistance = 0.12f;
inline constexpr float kContactMinSqrSpeed = 1e-6f;
inline constexpr float kContactCos = -0.5f;
}

bool vault_input_allows(float p_forward);
float vault_probe_height(float p_fraction);
VaultKind vault_up_or_over(VaultKind p_kind, float p_front_height, float p_further_height);
uint8_t choose_vault_variant(const VaultParams &p_params, VaultKind p_kind, const VaultMeasure &p_measure);
const SpecialMoveParams &vault_move(const VaultParams &p_params, VaultKind p_kind, uint8_t p_variant);

struct VaultScales {
	bool on = false;
	float distance = 1;
	float height = 1;
	float length = 1;
};
VaultScales vault_scales(const SpecialMoveParams &p_move, const VaultMeasure &p_measure, bool p_disabled = false);
float vault_horizontal_scale(const VaultScales &p_scales, const VaultMeasure &p_measure, float p_moved);

std::optional<Vec3> sample_move_keys(std::span<const MoveKey> p_keys, float p_time);
float move_duration(const SpecialMoveParams &p_move);

bool vault_contact_aborts(const Vec3 &p_contact_offset, const Vec3 &p_normal, const Vec3 &p_motion);

std::optional<VaultStart> probe_vault(
		const CollisionWorld &p_world, const Vec3 &p_feet, const Vec3 &p_forward, uint32_t p_mask);

}
