#pragma once
// Hit targets: what a projectile segment is tested against besides the static world (docs/design/m2a-weapons.md
// §5.7; plan §3.2 "analytic soldier capsules", R14; topics/01 §1.7, §5.3).
//
// A target is a set of zone capsules in world space. Where they come from is the provider's business:
//   * this wave: stationary stand-ins built from SoldierDef dimensions per stance (standin_target below), for the
//     shooting range and the unit tests;
//   * M2a integration: the server's soldiers at the present tick (bots, and humans without a hit report);
//   * M2a integration / R14: capsules posed from bf3_anim's reduced server skeleton, sampled from the
//     lag-compensation history at the (tick_a, tick_b, alpha) a client's HitReport names (plan §3.2 "Hit
//     registration"). ProjectileWorld asks the provider for the targets at the segment's time, so rewind is the
//     provider's choice and needs no change here.
// The 13 BoneCollisionData capsules (characters/soldiers/defaultsoldierbonecollision; 9 hi-LOD) carry their radius,
// length and material pair in data [verified: core.gameplay.weapons]; their zone is the pair's property name
// (Head, Body, Chest, Foot -> legs), as gameplay::body_multipliers reads it.

#include "bf3/core/gameplay/material_grid.h"
#include "bf3/core/gameplay/soldier_def.h"
#include "bf3/core/result.h"
#include "bf3/sim/sim_types.h"
#include "bf3/sim/weapons/sim_time.h"

#include <array>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace bf3::sim::weapons {

class SurfaceMaterialTable;

// The four damage zones of the grid (capsule material pairs' property names).
enum class HitZone : uint8_t { Head = 0, Body = 1, Chest = 2, Legs = 3 };
inline constexpr size_t kHitZoneCount = 4;
const char *hit_zone_name(HitZone p_zone);

// One capsule: the segment a..b (world space) swept by a sphere of `radius` (a == b: a sphere).
struct ZoneCapsule {
	Vec3 a{};
	Vec3 b{};
	float radius = 0;
	HitZone zone = HitZone::Body;
	uint8_t capsule = 0; // index into the BoneCollisionData list (0..12)
	uint8_t property = 0; // the capsule pair's PhysicsPropertyIndex & 0xFF (the grid column)
	uint8_t flags = 0; // zone_capsule_flags::
};
namespace zone_capsule_flags {
inline constexpr uint8_t kDeactivateIfBehindWall = 1u << 0; // BoneCollisionData.DeactivateIfBehindWall (legs)
} // namespace zone_capsule_flags

// The hi-LOD set has 9 capsules; 16 leaves room for a richer set without a layout change.
inline constexpr size_t kMaxZoneCapsules = 16;

struct HitTarget {
	uint32_t id = 0; // the provider's name for it (the server: the player slot)
	uint32_t team = 0;
	Vec3 bound_center{}; // a sphere around every capsule (broad phase)
	float bound_radius = 0;
	uint8_t capsule_count = 0;
	bool spawn_protected = false; // damage is refused (DamageRules::spawn_protection_blocks)
	std::array<ZoneCapsule, kMaxZoneCapsules> capsules{};
};

// Where a segment first meets a target.
struct TargetHit {
	float fraction = 1.0f; // along the segment, 0..1
	Vec3 position{};
	Vec3 normal{}; // outward capsule normal at the hit
	uint32_t target = 0; // HitTarget::id
	uint8_t capsule = 0;
	HitZone zone = HitZone::Body;
	uint8_t property = 0;
	uint8_t capsule_flags = 0;
};

// The targets a segment flown at p_time is tested against. ProjectileWorld calls it once per projectile
// sub-step on the owner's thread; the returned span must stay valid until the next call. Implementations must be
// deterministic (same inputs, same span contents in the same order): the earliest hit wins and ties go to the
// lower index.
class HitTargetProvider {
public:
	virtual ~HitTargetProvider() = default;
	virtual std::span<const HitTarget> targets_at(SimTime p_time) const = 0;
};

// A fixed list (the shooting range's stationary stand-ins; tests).
class StaticTargets final : public HitTargetProvider {
public:
	explicit StaticTargets(std::vector<HitTarget> p_targets) :
			targets_(std::move(p_targets)) {}
	std::span<const HitTarget> targets_at(SimTime) const override { return targets_; }
	std::vector<HitTarget> &targets() { return targets_; }

private:
	std::vector<HitTarget> targets_;
};

// First hit of the segment p_from -> p_to on p_capsule (exact quadratic in double; a start inside the capsule
// hits at fraction 0 with the normal opposite the segment).
bool segment_capsule(const Vec3 &p_from, const Vec3 &p_to, const ZoneCapsule &p_capsule, float &r_fraction, Vec3 &r_normal);
// First hit on any capsule of p_target (bound sphere first); the lowest fraction wins, ties go to the lower
// capsule index.
bool segment_target(const Vec3 &p_from, const Vec3 &p_to, const HitTarget &p_target, TargetHit &r_hit);

// Stand-in capsules for a stationary soldier (until R14's server pose exists): the hi-LOD capsules' radius,
// length, zone and pair from data, placed by a fixed per-stance layout [inferred] scaled to the SoldierDef pose
// (stand: height 1.8, eye 1.6; crouch 1.15 / 1.0; prone: lying along the facing inside the prone footprint
// box_min/max_expand, data (0.3, 0, 1.3) / (0.3, 0, 1.1)). The head capsule's centre is at the pose's eye
// height + 0.05 m. Facing: p_yaw (Command quantisation), feet at p_feet. The layout table is in
// src/weapons/hit_target.cpp; docs §5.7 lists it.
core::Result<HitTarget> standin_target(const core::gameplay::SoldierDef &p_soldier, std::span<const core::gameplay::HitCapsule> p_capsules,
		const SurfaceMaterialTable &p_materials, Pose p_pose, const Vec3 &p_feet, uint16_t p_yaw, uint32_t p_id, uint32_t p_team = 0);

// The centre of the first capsule of p_zone (the range harness aims there); nullopt-like false when absent.
bool zone_aim_point(const HitTarget &p_target, HitZone p_zone, Vec3 &r_point);

} // namespace bf3::sim::weapons
