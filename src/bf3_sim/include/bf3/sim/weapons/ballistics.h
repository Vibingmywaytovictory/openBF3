#pragma once
// Projectiles: bullets, pellets, grenades and missiles as queries, never bodies (docs/design/m2a-weapons.md §5.7,
// §5.8; plan §3.2 "Projectiles step p += v dt + 1/2 g dt^2 and test segments", D5; topics/01 §2.3, §5.3).
//
// Flight state is double (DVec3). Unpowered flight (bullets, pellets, grenades between bounces, a missile after
// its engine) is the CLOSED FORM from the projectile's base state: p(t) = base_position + base_velocity tau +
// 1/2 a tau^2, tau = units_to_seconds(t - base_time), re-based only at a discontinuity (spawn, bounce, engine phase
// change). So a bullet's position at any time is exact to double rounding however long it flies (the M98B's
// 3,250 m at 5 s: below a micrometre; a per-tick float step accumulates about 6 mm there, which W2's 1 mm max-range
// row cannot absorb: review of revision 1). A missile's thrust phase is stepped (4 sub-steps per tick, in double,
// re-basing each sub-step). The segment between two samples is a chord, whose sag below the parabola is g h^2 / 8
// (0.52 mm for a whole 60 Hz tick at g 15), so one segment per tick keeps the hit test within a millimetre of the
// true path. Only the CollisionWorld ray is float (its ulp at 3 km is 0.24 mm). A shot spawns at its exact sub-tick
// spawn time and flies the rest of that tick at once.
// Each segment is tested, earliest hit first, against
//   * the static world: CollisionWorld::ray with query_mask::kProjectile (Collision + Raycast; the Character
//     layer is soldiers-only collision). Invisible blockers (kSoupInvisible: player clip) are passed through by
//     re-casting from just past the hit; so are penetrable surfaces (SurfacePair::penetrable without the cell's
//     NeverPenetrate; the damage scales by the cell's DamagePenetrationMultiplier) [inferred rule, topics/01 C9];
//   * hit targets: HitTargetProvider::targets_for(TargetQuery{segment times, the projectile's shooter, shot,
//     pellet, def}), analytic capsules (hit_target.h); the identity lets the provider rewind per shooter. A leg
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

#include <array>
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

// Projectile flight state in double (see above).
using DVec3 = std::array<double, 3>;

struct Projectile {
	SimTime time = 0; // the projectile's own clock: position / velocity are at this time
	SimTime spawn_time = 0;
	// The base of the current closed-form piece: the state at the last discontinuity (spawn, bounce, engine phase
	// change, a thrust sub-step). position / velocity are evaluated from it, never accumulated.
	SimTime base_time = 0;
	DVec3 base_position{};
	DVec3 base_velocity{};
	DVec3 position{}; // at `time`
	DVec3 velocity{};
	DVec3 origin{}; // where it left (the falloff distance is measured from here)
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
	// Diagnostics the range reports (docs §5.8, R-W2, R-W10): static rays that met a kSoupInvisible triangle and
	// passed it, surfaces penetrated, and Collision-layer hits with a Raycast-layer hit within 10 cm behind them.
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

	// Adds a shot's projectiles (one per pellet: pellet k > 0 draws its direction inside ShotEvent::pellet_cone
	// around ShotEvent::direction from the shot's seed, so every end expands the pellets alike from the event
	// alone). Refused, with a count, when the world is full or the def has no projectile. An instant-hit shot
	// (knife) is resolved on the next step at its spawn time.
	core::Status spawn(const ShotEvent &p_shot);
	// Flies every projectile from its own time to tick_end(p_tick) (a projectile spawned later than the tick's
	// start flies only its part), tests every segment, detonates fuses and explosive impacts (blasts are tested
	// against the provider's answer to a Blast query, with occlusion rays), and appends the tick's events to r_out
	// (cleared by the caller). Deterministic: projectiles step in spawn order, events come out in that order. Each
	// segment asks p_targets with the projectile's own TargetQuery, so the provider may rewind per shooter.
	// A projectile is removed when it leaves the static world's bounds by more than BallisticsLimits::world_margin
	// (a synthetic test world must span every flight it gates: docs §10).
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

// Closed-form flight under constant acceleration (the arithmetic the drop gates compare the flight to).
DVec3 ballistic_position(const DVec3 &p_origin, const DVec3 &p_velocity, double p_gravity, double p_seconds);
// The projectile's state at p_until with no collision (ProjectileWorld's own flight code: closed form while
// unpowered, stepped under thrust), exposed so the tests and bf3dump weapon-sim tabulate it: drop, time of flight,
// missile speed.
Projectile fly_free(const WeaponSimDef &p_def, const Projectile &p_start, SimTime p_until);

// Late HitReport validation (docs §7.1 step 6; plan §3.2 "the server flies its own bullet ... and tests that
// target against the pose reconstructed at exactly that sample"). A client's report usually arrives after the
// server has already flown the bullet live. The server then re-flies pellet p_pellet of the retained p_shot
// alone, from its spawn to at most p_until (bounded by the def's time to live), against p_world and p_targets
// (queries of kind Refly: the provider answers with the reported target posed at the reported sample), with the
// same static passes and penetration as step. Flight is a pure function of (ShotEvent, def, static world), so the
// re-flown path is the live one. Returns true and the first target hit in r_hit; false when nothing is hit or
// p_budget runs out (r_status says which). Charges p_budget per segment, re-cast and capsule, as step does.
bool refly_projectile(const WeaponTable &p_table, const ShotEvent &p_shot, uint8_t p_pellet, SimTime p_until, const CollisionWorld &p_world,
		const HitTargetProvider &p_targets, core::gameplay::WorkBudget &p_budget, ImpactEvent &r_hit, core::Status &r_status);

} // namespace bf3::sim::weapons
