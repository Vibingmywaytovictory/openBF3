#pragma once

#include "bf3/core/gameplay/work_budget.h"
#include "bf3/core/result.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/static_world.h"
#include "bf3/sim/weapons/damage.h"
#include "bf3/sim/weapons/hit_target.h"
#include "bf3/sim/weapons/sim_time.h"
#include "bf3/sim/weapons/weapon_sim.h"
#include "bf3/sim/weapons/weapon_sim_def.h"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>
#include <vector>

namespace bf3::sim::weapons {

namespace projectile_flags {
inline constexpr uint8_t kArmed = 1u << 0;
inline constexpr uint8_t kResting = 1u << 1;
inline constexpr uint8_t kEngineOn = 1u << 2;
}

using DVec3 = std::array<double, 3>;

struct Projectile {
	SimTime time = 0;
	SimTime spawn_time = 0;

	SimTime base_time = 0;
	DVec3 base_position{};
	DVec3 base_velocity{};
	DVec3 position{};
	DVec3 velocity{};
	DVec3 origin{};
	double path_length = 0;
	uint64_t seed = 0;
	uint32_t player_slot = 0;
	uint32_t shot_index = 0;
	uint16_t def = kNoWeapon;
	uint8_t pellet = 0;
	uint8_t flags = 0;
	float damage_scale = 1;
	uint8_t bounces = 0;
	uint8_t passes = 0;
	uint16_t reserved = 0;
};
static_assert(std::is_trivially_copyable_v<Projectile>);

enum class ImpactKind : uint8_t { Static = 0, Target = 1, Expired = 2, Bounce = 3 };

struct ImpactEvent {
	SimTime time = 0;
	Vec3 position{};
	Vec3 normal{};
	Vec3 velocity{};
	uint32_t player_slot = 0;
	uint32_t shot_index = 0;
	uint16_t def = kNoWeapon;
	uint8_t pellet = 0;
	ImpactKind kind = ImpactKind::Static;
	uint32_t grid_pair = 0;
	uint32_t material_value = 0;
	uint8_t physics_material = 0;
	uint8_t physics_property = 0;
	uint8_t surface = 0;
	StaticLayer layer = StaticLayer::Collision;
	bool penetrated = false;
	uint32_t target = 0;
	HitZone zone = HitZone::Body;
};

struct DetonationEvent {
	SimTime time = 0;
	Vec3 position{};
	Vec3 normal{};
	uint32_t player_slot = 0;
	uint32_t shot_index = 0;
	uint16_t def = kNoWeapon;
	bool dud = false;
	bool timeout = false;
};

struct BallisticsLimits {
	size_t max_projectiles = 8192;
	uint8_t max_recasts = 8;
	uint8_t max_bounces = 4;
	uint8_t missile_substeps = 4;
	size_t budget_per_step = 1u << 22;
	float world_margin = 200.0f;
};

struct ProjectileStepOutput {
	std::vector<ImpactEvent> impacts;
	std::vector<DamageEvent> damage;
	std::vector<DetonationEvent> detonations;
	size_t removed = 0;
	size_t budget_used = 0;
	bool budget_exhausted = false;

	size_t invisible_passes = 0;
	size_t penetrations = 0;
	size_t collision_before_raycast = 0;

	void clear() {
		impacts.clear();
		damage.clear();
		detonations.clear();
		removed = 0;
		budget_used = 0;
		budget_exhausted = false;
		invisible_passes = 0;
		penetrations = 0;
		collision_before_raycast = 0;
	}
};

class ProjectileWorld {
public:
	ProjectileWorld(std::shared_ptr<const WeaponTable> p_table, const BallisticsLimits &p_limits = {});

	core::Status spawn(const ShotEvent &p_shot);

	void step(Tick p_tick, const CollisionWorld &p_world, const HitTargetProvider &p_targets, ProjectileStepOutput &r_out);

	std::span<const Projectile> live() const { return live_; }
	size_t refused() const { return refused_; }
	const WeaponTable &table() const { return *table_; }

private:
	std::shared_ptr<const WeaponTable> table_;
	BallisticsLimits limits_;
	std::vector<Projectile> live_;
	size_t refused_ = 0;
};

DVec3 ballistic_position(const DVec3 &p_origin, const DVec3 &p_velocity, double p_gravity, double p_seconds);

Projectile fly_free(const WeaponSimDef &p_def, const Projectile &p_start, SimTime p_until);

bool refly_projectile(const WeaponTable &p_table, const ShotEvent &p_shot, uint8_t p_pellet, SimTime p_until, const CollisionWorld &p_world,
		const HitTargetProvider &p_targets, core::gameplay::WorkBudget &p_budget, ImpactEvent &r_hit, core::Status &r_status);

}
