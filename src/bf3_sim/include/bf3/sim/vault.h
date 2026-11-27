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
// SOURCE: notes Q10 1 and its checker (forward input at least 0.2; exactly 0.2 passes).
inline constexpr float kMinForwardInput = 0.2f;
// STAND-IN: the probes' middle reference height, which notes R-B4 leave open (their guess about 0.9 m).
// Settle: a note that reads the middle height.
inline constexpr float kMiddleHeight = 0.75f;
// SOURCE: notes R-B4 B (high, middle and low probes +0.5, -0.2 and -0.4 m from the middle height).
inline constexpr float kHighOffset = 0.5f;
inline constexpr float kMiddleOffset = -0.2f;
inline constexpr float kLowOffset = -0.4f;
// SOURCE: notes R-B4 checker (each height probe runs 0.5 m along one horizontal local axis).
inline constexpr float kProbeReach = 0.5f;
// SOURCE: notes Q10 constants (detection ray scales 1.2 and 5; the note infers their meaning).
inline constexpr float kDetectionRayMinScale = 1.2f;
inline constexpr float kDetectionRayMaxScale = 5.0f;
// SOURCE: notes Q10 3 and its checker (1.8 m down rays, 0.1 m least height, 1e-6 m past the face, vault-up 0.25 m
// within 0.2 m, side rays 0.35 m, 4 m back search, 0.05 m least length), this constant and the seven below it.
inline constexpr float kDownProbe = 1.8f;
inline constexpr float kMinHeight = 0.1f;
inline constexpr float kFrontEpsilon = 1e-6f;
inline constexpr float kUpLength = 0.25f;
inline constexpr float kUpTolerance = 0.2f;
inline constexpr float kSideOffset = 0.35f;
inline constexpr float kBackSearch = 4.0f;
inline constexpr float kMinLength = 0.05f;
// SOURCE: notes R-B4 E5 (scale phase margin 0.2 m before and after the obstacle, Optimum threshold 1e-6).
inline constexpr float kPhaseMargin = 0.2f;
inline constexpr float kOptimumFloor = 1e-6f;
// SOURCE: notes Q10 6 and its checker (squared distance 0.12 m^2, squared speed floor 1e-6, abort cone cos -0.5).
inline constexpr float kContactSqrDistance = 0.12f;
inline constexpr float kContactMinSqrSpeed = 1e-6f;
inline constexpr float kContactCos = -0.5f;
}

const char *vault_kind_name(VaultKind p_kind);
bool vault_input_allows(float p_forward);
float vault_probe_reach(float p_speed);
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

Vec3 vault_gravity(const SpecialMoveParams &p_move, uint32_t p_tick);
Vec3 vault_fall_velocity(const SpecialMoveParams &p_move, uint32_t p_ticks, uint32_t p_airborne);

float vault_height_at(
		const CollisionWorld &p_world, const Vec3 &p_feet, const Vec3 &p_forward, float p_ahead, uint32_t p_mask);
std::optional<float> vault_face_distance(
		const CollisionWorld &p_world, const Vec3 &p_origin, const Vec3 &p_forward, uint32_t p_mask, float p_reach);

bool vault_contact_aborts(const Vec3 &p_contact_offset, const Vec3 &p_normal, const Vec3 &p_motion);

std::optional<VaultStart> probe_vault(const CollisionWorld &p_world, const Vec3 &p_feet, const Vec3 &p_forward,
		uint32_t p_mask, float p_reach = vault_rules::kProbeReach);

}
