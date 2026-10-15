#pragma once
// Projectiles: bullets, pellets, grenades and missiles as queries, never bodies (docs/design/m2a-weapons.md §5.7,
// §5.8; plan §3.2 "Projectiles step p += v dt + 1/2 g dt^2 and test segments", D5; topics/01 §2.3, §5.3).
//
// Flight: exact constant-acceleration steps (gravity, and a missile's engine and drag piecewise per sub-step), so
// the position at any sample time is the closed form within float rounding; the segment between two samples is
// a chord, whose sag below the parabola is g h^2 / 8 (0.52 mm for a whole 60 Hz tick at g 15), so one sub-step
// per tick keeps the hit test within a millimetre of the true path; a missile's thrust phase sub-steps at 4 per
// tick. A shot spawns at its exact sub-tick spawn time and flies the rest of that tick at once.
// Each segment is tested, earliest hit first, against
//   * the static world: CollisionWorld::ray with query_mask::kProjectile (Collision + Raycast; the Character
//     layer is soldiers-only collision). Invisible blockers (kSoupInvisible: player clip) are passed through by
//     re-casting from just past the hit; so are penetrable surfaces (SurfacePair::penetrable without the cell's
//     NeverPenetrate; the damage scales by the cell's DamagePenetrationMultiplier) [inferred rule, topics/01 C9];
//   * hit targets: HitTargetProvider::targets_at(segment start time), analytic capsules (hit_target.h). A leg
//     capsule (DeactivateIfBehindWall) behind static geometry seen from the shot origin is skipped.
// Results go out as events for the server rules (damage), VFX and audio (impacts, detonations) and the HUD.
//
// Threads: one ProjectileWorld per owner thread (the server; later a client's cosmetic own-shot world), like
// CollisionWorld. Bounds (§9): at most BallisticsLimits::max_projectiles alive (a spawn beyond is refused and
// counted), at most max_recasts re-casts and max_bounces bounces per projectile per tick, and one WorkBudget per
// step charged per segment, per re-cast and per target capsule tested, so a hostile def or a crowd of targets
// cannot make a tick unbounded.

#include "bf3/core/gameplay/work_budget.h"
#include "bf3/core/result.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/static_world.h"
#include "bf3/sim/weapons/damage.h"
#include "bf3/sim/weapons/hit_target.h"
#include "bf3/sim/weapons/sim_time.h"
#include "bf3/sim/weapons/weapon_sim.h"
#include "bf3/sim/weapons/weapon_sim_def.h"

#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>
#include <vector>

namespace bf3::sim::weapons {

namespace projectile_flags {
inline constexpr uint8_t kArmed = 1u << 0; // past time_to_arm
inline constexpr uint8_t kResting = 1u << 1; // a grenade that stopped bouncing
inline constexpr uint8_t kEngineOn = 1u << 2;
} // namespace projectile_flags

struct Projectile {
	SimTime time = 0; // the projectile's own clock: position / velocity are at this time
	SimTime spawn_time = 0;
	Vec3 position{};
	Vec3 velocity{};
	Vec3 origin{}; // where it left (the falloff distance is measured from here)
	double path_length = 0; // metres flown (FalloffBasis::PathLength)
	uint64_t seed = 0;
	uint32_t player_slot = 0;
	uint32_t shot_index = 0;
	uint16_t def = kNoWeapon;
	uint8_t pellet = 0;
	uint8_t flags = 0; // projectile_flags::
	float damage_scale = 1; // product of the penetration multipliers so far
	uint8_t bounces = 0;
	uint8_t passes = 0; // surfaces penetrated so far
	uint16_t reserved = 0;
};
static_assert(std::is_trivially_copyable_v<Projectile>);

enum class ImpactKind : uint8_t { Static = 0, Target = 1, Expired = 2, Bounce = 3 };

// The impact descriptor the later VFX and audio impact selection reads (plan §3.2, §3.14: by PhysicsMaterialIndex).
struct ImpactEvent {
	SimTime time = 0;
	Vec3 position{};
	Vec3 normal{};
	Vec3 velocity{}; // at impact
	uint32_t player_slot = 0;
	uint32_t shot_index = 0;
	uint16_t def = kNoWeapon;
	uint8_t pellet = 0;
	ImpactKind kind = ImpactKind::Static;
	uint32_t grid_pair = 0; // the static triangle's pair index (SurfaceTag::material & 0xFFFFFF); 0 for targets
	uint32_t material_value = 0; // the triangle's whole MaterialFlagsAndIndices value
	uint8_t physics_material = 0; // the effects row (impact effect, sound, decal)
	uint8_t physics_property = 0; // the damage row of what was hit
	uint8_t surface = 0; // the soup's surface class (footstep / impact families)
	StaticLayer layer = StaticLayer::Collision;
	bool penetrated = false; // the projectile went on through
	uint32_t target = 0; // HitTarget::id for kind Target
	HitZone zone = HitZone::Body;
};

struct DetonationEvent {
	SimTime time = 0;
	Vec3 position{};
	Vec3 normal{}; // of the surface it went off on ((0,1,0) in the air)
	uint32_t player_slot = 0;
	uint32_t shot_index = 0;
	uint16_t def = kNoWeapon;
	bool dud = false; // before time_to_arm: DudExplosion
	bool timeout = false; // the fuse (DetonateOnTimeout)
};

struct BallisticsLimits {
	size_t max_projectiles = 8192;
	uint8_t max_recasts = 8; // per projectile per tick (invisible blockers, penetrated surfaces)
	uint8_t max_bounces = 4; // per projectile per tick
	uint8_t missile_substeps = 4; // per tick while the engine runs
	size_t budget_per_step = 1u << 22; // WorkBudget units per ProjectileWorld::step
	float world_margin = 200.0f; // m: below the static world's min y or outside its bounds by this -> removed
};

struct ProjectileStepOutput {
	std::vector<ImpactEvent> impacts;
	std::vector<DamageEvent> damage;
	std::vector<DetonationEvent> detonations;
	size_t removed = 0; // expired, hit, out of the world
	size_t budget_used = 0;
	bool budget_exhausted = false; // the rest of the projectiles were not stepped this tick (they stay)

	void clear() {
		impacts.clear();
		damage.clear();
		detonations.clear();
		removed = 0;
		budget_used = 0;
		budget_exhausted = false;
	}
};

class ProjectileWorld {
public:
	ProjectileWorld(std::shared_ptr<const WeaponTable> p_table, const BallisticsLimits &p_limits = {});

	// Adds a shot's projectiles (one per pellet, each pellet's cone draw from the shot's seed). Refused, with a
	// count, when the world is full or the def has no projectile. An instant-hit shot (knife) is resolved on
	// the next step at its spawn time.
	core::Status spawn(const ShotEvent &p_shot);
	// Flies every projectile from its own time to tick_end(p_tick) (a projectile spawned later than the tick's
	// start flies only its part), tests every segment, detonates fuses and explosive impacts (blasts are tested
	// against the same targets with occlusion rays), and appends the tick's events to r_out (cleared by the
	// caller). Deterministic: projectiles step in spawn order, events come out in that order.
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

// Closed-form flight under constant acceleration (the arithmetic the drop gates compare the stepped flight to).
Vec3 ballistic_position(const Vec3 &p_origin, const Vec3 &p_velocity, float p_gravity, double p_seconds);
// The projectile's state p_seconds after spawn with no collision (ProjectileWorld's own integration, exposed so
// the tests and bf3dump weapon-sim tabulate the stepped path: drop, time of flight, missile speed).
Projectile fly_free(const WeaponSimDef &p_def, const Projectile &p_start, SimTime p_until);

} // namespace bf3::sim::weapons
