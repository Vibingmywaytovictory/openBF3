#pragma once
// CapsulePoseTable: the soldier's 9 hi-LOD hit capsules in its local frame per stance, pitch bucket and transition
// fraction, built once per level from the 3p rig, its stance clips and the additive aim (plan R14; docs/design/
// m2a.md §6.5; owner lc). Plain data with a hash, no runtime state: the server poses its hit targets from it
// (sim::weapons::posed_target, lag compensation) and the client poses the same targets for its cosmetic test, so
// both ends must hold the identical table (its hash is part of MatchInfo::weapon_hash; a client with another table
// is refused). Built by bf3_anim::build_capsule_pose_table (server_pose.h), cached under capsules/ (§13.1).
//
// Local frame: the soldier's feet at the origin, facing +Z (yaw 0, command.h), Y up, x to the soldier's left.
// Entry layout (row-major, capsule fastest):
//   steady      [stance kCapsuleStances][pitch kCapsulePitchBuckets][capsule capsule_count]
//   transition  [transition kCapsuleTransitions][fraction kCapsuleTransitionFractions][pitch][capsule]
// Pitch buckets span pitch_deg[0] = -60.7 (full down aim) to pitch_deg[32] = +78.1 (full up aim, S3's measured
// LocalPost limits); a transition row is sampled at kCapsuleTransitionFractions fractions of its clip's own length
// with the source stance's aim. Rows whose clip is missing, fails to bind or is not additive where required are
// built from wp's standin_target layout and tagged in row_source (E18 prints the tags; 0 stand-ins expected).

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace bf3::core::gameplay {

inline constexpr uint32_t kCapsuleTableVersion = 1; // part of the capsules/ cache key
inline constexpr size_t kCapsuleStances = 3; // stand, crouch, prone (sim::Pose order)
inline constexpr size_t kCapsulePitchBuckets = 33;
inline constexpr size_t kCapsuleTransitions = 6;
inline constexpr size_t kCapsuleTransitionFractions = 8;
inline constexpr size_t kCapsuleRows = kCapsuleStances + kCapsuleTransitions; // row_source entries
inline constexpr size_t kMaxTableCapsules = 16; // = sim::weapons::kMaxZoneCapsules (hi-LOD set: 9)

// The six stance transitions, in the order of their rows (the controllers of m2a.md §6.5's table).
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
} // namespace capsule_row_source

// One capsule segment a..b swept by `radius`, local frame. 32 bytes, no padding (hashed field by field anyway).
struct CapsuleLocal {
	float a[3];
	float b[3];
	float radius;
	uint8_t zone; // sim::weapons::HitZone
	uint8_t capsule; // index into the BoneCollisionData list (0..12)
	uint8_t property; // the capsule pair's PhysicsPropertyIndex & 0xFF (the grid column)
	uint8_t flags; // sim::weapons::zone_capsule_flags
};
static_assert(sizeof(CapsuleLocal) == 32);
static_assert(std::is_trivially_copyable_v<CapsuleLocal>);

struct CapsulePoseTable {
	uint32_t version = 0; // kCapsuleTableVersion
	uint64_t hash = 0; // capsule_pose_table_hash
	uint8_t capsule_count = 0; // 9 on the retail install
	std::array<float, kCapsulePitchBuckets> pitch_deg{}; // ascending, -60.7 .. +78.1
	std::vector<CapsuleLocal> steady; // [stance][pitch][capsule]
	std::vector<CapsuleLocal> transition; // [transition][fraction][pitch][capsule]
	std::array<uint8_t, kCapsuleRows> row_source{}; // stance rows 0..2, then transition rows: capsule_row_source::
	float bound_radius = 0; // a sphere around every capsule of every entry, about the feet (the broad phase's growth)

	// Entry offsets (the first capsule of the entry). Callers check the indices; out of range is a caller bug.
	size_t steady_index(size_t p_stance, size_t p_pitch) const { return (p_stance * kCapsulePitchBuckets + p_pitch) * capsule_count; }
	size_t transition_index(size_t p_transition, size_t p_fraction, size_t p_pitch) const {
		return ((p_transition * kCapsuleTransitionFractions + p_fraction) * kCapsulePitchBuckets + p_pitch) * capsule_count;
	}
};

// The transition row from p_from to p_to (sim::Pose values 0..2); false for p_from == p_to or an index out of range.
bool capsule_transition_of(uint8_t p_from, uint8_t p_to, CapsuleTransition &r_transition);
// FNV-1a 64 over every field in declaration order, floats by their bit pattern, the vectors as length + entries
// (field by field). The table's `hash` is this value; MatchInfo's weapon_hash mixes it in.
uint64_t capsule_pose_table_hash(const CapsulePoseTable &p_table);
// Structural checks a cached or built table must pass before use (version, sizes = the layout above with
// capsule_count, every float finite, radii in (0, 2], ascending pitches, the row tags in range): a hostile cache
// entry fails here instead of reaching posed_target.
bool capsule_pose_table_valid(const CapsulePoseTable &p_table);

// ---- lc additions (as built; docs/design/m2a-lc.md) ----------------------------------------------------------------

// The capsules/ cache entry (m2a.md §13.1): magic "BF3CAPS1", kCapsuleTableVersion, then every field; decode fails
// (no allocation beyond the blob's own size) on a short, oversized or inconsistent blob, and accepts only a table that
// passes capsule_pose_table_valid and whose stored hash equals the recomputed one.
std::vector<uint8_t> encode_capsule_pose_table(const CapsulePoseTable &p_table);
bool decode_capsule_pose_table(std::span<const uint8_t> p_blob, CapsulePoseTable &r_table, std::string *r_error = nullptr);

} // namespace bf3::core::gameplay
