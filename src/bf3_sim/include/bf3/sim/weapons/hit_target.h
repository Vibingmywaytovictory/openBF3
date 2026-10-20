#pragma once

#include "bf3/core/gameplay/material_grid.h"
#include "bf3/core/gameplay/soldier_def.h"
#include "bf3/core/result.h"
#include "bf3/sim/sim_types.h"
#include "bf3/sim/weapons/sim_time.h"

#include <array>
#include <cstdint>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace bf3::sim::weapons {

class SurfaceMaterialTable;

enum class HitZone : uint8_t { Head = 0, Body = 1, Chest = 2, Legs = 3 };
inline constexpr size_t kHitZoneCount = 4;
const char *hit_zone_name(HitZone p_zone);

struct ZoneCapsule {
	Vec3 a{};
	Vec3 b{};
	float radius = 0;
	HitZone zone = HitZone::Body;
	uint8_t capsule = 0;
	uint8_t property = 0;
	uint8_t flags = 0;
};
namespace zone_capsule_flags {
inline constexpr uint8_t kDeactivateIfBehindWall = 1u << 0;
}

inline constexpr size_t kMaxZoneCapsules = 16;

struct HitTarget {
	uint32_t id = 0;
	uint32_t team = 0;
	Vec3 bound_center{};
	float bound_radius = 0;
	uint8_t capsule_count = 0;
	bool spawn_protected = false;
	std::array<ZoneCapsule, kMaxZoneCapsules> capsules{};
};

struct TargetHit {
	float fraction = 1.0f;
	Vec3 position{};
	Vec3 normal{};
	uint32_t target = 0;
	uint8_t capsule = 0;
	HitZone zone = HitZone::Body;
	uint8_t property = 0;
	uint8_t capsule_flags = 0;
};

enum class TargetQueryKind : uint8_t {
	Flight = 0,
	Blast = 1,
	Refly = 2,
};

struct TargetQuery {
	SimTime time_start = 0;
	SimTime time_end = 0;
	uint32_t player_slot = 0;
	uint32_t shot_index = 0;
	uint16_t def = 0xFFFF;
	uint8_t pellet = 0;
	TargetQueryKind kind = TargetQueryKind::Flight;

	Vec3 from{};
	Vec3 to{};
	float radius = 0;
	bool has_segment = false;
};
static_assert(std::is_trivially_copyable_v<TargetQuery>);

class HitTargetProvider {
public:
	virtual ~HitTargetProvider() = default;
	virtual std::span<const HitTarget> targets_for(const TargetQuery &p_query) const = 0;
};

class StaticTargets final : public HitTargetProvider {
public:
	explicit StaticTargets(std::vector<HitTarget> p_targets) :
			targets_(std::move(p_targets)) {}
	std::span<const HitTarget> targets_for(const TargetQuery &) const override { return targets_; }
	std::vector<HitTarget> &targets() { return targets_; }

private:
	std::vector<HitTarget> targets_;
};

bool segment_capsule(const Vec3 &p_from, const Vec3 &p_to, const ZoneCapsule &p_capsule, float &r_fraction, Vec3 &r_normal);

bool segment_target(const Vec3 &p_from, const Vec3 &p_to, const HitTarget &p_target, TargetHit &r_hit);

core::Result<HitTarget> standin_target(const core::gameplay::SoldierDef &p_soldier, std::span<const core::gameplay::HitCapsule> p_capsules,
		const SurfaceMaterialTable &p_materials, Pose p_pose, const Vec3 &p_feet, uint16_t p_yaw, uint32_t p_id, uint32_t p_team = 0);

bool zone_aim_point(const HitTarget &p_target, HitZone p_zone, Vec3 &r_point);

}
