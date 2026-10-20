#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace bf3::core::gameplay {

inline constexpr uint32_t kCapsuleTableVersion = 2;
inline constexpr size_t kCapsuleStances = 3;
inline constexpr size_t kCapsulePitchBuckets = 33;
inline constexpr size_t kCapsuleTransitions = 6;
inline constexpr size_t kCapsuleTransitionFractions = 16;
inline constexpr size_t kCapsuleRows = kCapsuleStances + kCapsuleTransitions;
inline constexpr size_t kMaxTableCapsules = 16;

enum class CapsuleTransition : uint8_t {
	StandToCrouch = 0,
	StandToProne = 1,
	CrouchToStand = 2,
	CrouchToProne = 3,
	ProneToStand = 4,
	ProneToCrouch = 5,
};

namespace capsule_row_source {
inline constexpr uint8_t kClipDerived = 0;
inline constexpr uint8_t kStandIn = 1;
}

struct CapsuleLocal {
	float a[3];
	float b[3];
	float radius;
	uint8_t zone;
	uint8_t capsule;
	uint8_t property;
	uint8_t flags;
};
static_assert(sizeof(CapsuleLocal) == 32);
static_assert(std::is_trivially_copyable_v<CapsuleLocal>);

struct CapsulePoseTable {
	uint32_t version = 0;
	uint64_t hash = 0;
	uint8_t capsule_count = 0;
	std::array<float, kCapsulePitchBuckets> pitch_deg{};
	std::vector<CapsuleLocal> steady;
	std::vector<CapsuleLocal> transition;
	std::array<uint8_t, kCapsuleRows> row_source{};
	float bound_radius = 0;

	size_t steady_index(size_t p_stance, size_t p_pitch) const { return (p_stance * kCapsulePitchBuckets + p_pitch) * capsule_count; }
	size_t transition_index(size_t p_transition, size_t p_fraction, size_t p_pitch) const {
		return ((p_transition * kCapsuleTransitionFractions + p_fraction) * kCapsulePitchBuckets + p_pitch) * capsule_count;
	}
};

bool capsule_transition_of(uint8_t p_from, uint8_t p_to, CapsuleTransition &r_transition);

uint64_t capsule_pose_table_hash(const CapsulePoseTable &p_table);

bool capsule_pose_table_valid(const CapsulePoseTable &p_table);

std::vector<uint8_t> encode_capsule_pose_table(const CapsulePoseTable &p_table);
bool decode_capsule_pose_table(std::span<const uint8_t> p_blob, CapsulePoseTable &r_table, std::string *r_error = nullptr);

}
