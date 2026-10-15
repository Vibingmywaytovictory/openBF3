#pragma once
// VehicleWorld: one owner's Jolt world with driven vehicles over the shared static world (docs/design/s2.md §6;
// plan D5, §3.4; research/topics/04-vehicles.md §1.3-§1.5, §5.4). The server's world and the client's prediction
// world are two VehicleWorlds over one std::shared_ptr<const StaticWorldShapes>.
//
// Bodies: create() adds the static bodies exactly as CollisionWorld does (one per tile and layer present, tile order,
// user data tile x 4 + layer, then OptimizeBroadPhase), so the static BodyIDs equal CollisionWorld's. Vehicles go to
// their own broadphase layer ("moving"), so adding one never rebuilds the static tree, and use object layer
// kVehicleObjectLayer, which collides with StaticLayer::Collision only (not Raycast shells or character-only
// geometry) and with other vehicles. add_vehicle(slot) creates the chassis body with BodyInterface::CreateBodyWithID
// (static count + slot) and then its VehicleConstraint. Jolt restores constraints by index (= add order), so every
// world must add the same slots in the same order (the replay harness does; restore() checks the slot list).
//
// Step: one PhysicsSystem::Update(kTickSeconds, tuning.collision_steps) per tick on a JobSystemSingleThreaded owned
// by the world (no shared job threads: the step's order is the world's own; s2.md §6.3). Before the update, per
// vehicle in slot order: the driver (our code; vehicle_model.h numbers) turns the tick's VehicleCommand and the
// driver state into the controller's four inputs, adds the air drag force with EActivation::DontActivate (a zero
// force must not wake a sleeping vehicle; BodyInterface.cpp:699), and advances the stand-still timer. Contact
// listener callbacks only count (chassis-static contacts per tick), so their order cannot matter.
//
// Threads: every method on the owner's thread. Two worlds over the same shapes may step at the same time on
// different threads (sim.vehicle.unit runs them so).
//
// Save / restore:
//   save()    the full state: PhysicsSystem::SaveState(EStateRecorderState::All) with a filter that keeps every
//             non-static body (statics never change), i.e. global state, the vehicle bodies, the contact cache and
//             every constraint (VehicleConstraint, controller), plus our driver states and the tick. In-process and
//             trusted: it is never sent or stored (the network carries VehicleGhostCodes, vehicle_ghost.h).
//             Not in it, because Jolt saves neither: the broadphase trees' layout and
//             BroadPhaseQuadTree::mNextLayerToUpdate. That is harmless while candidate order cannot reach the
//             result (contacts are sorted, s2.md §6.3); the multi-vehicle check G1m (s2.md §1) is where it would
//             show.
//   restore() the full state back (any world created the same way with the same slots); bit-identical stepping
//             afterwards is gate G1 (s2.md §1). The slot list and the stream length are checked before Jolt reads
//             anything. Jolt's non-validating RestoreState applies bodies before it can fail
//             (BodyManager.cpp:777-803), so restore() saves the current state first and puts that backup back if
//             Jolt refuses the stream: a failed restore leaves the world as it was.
//   restore_vehicle_state() one vehicle from a VehicleState POD, groups chosen by vehicle_restore:: bits, through
//             Jolt's own restore functions only, each fed a stream in Jolt's save layout: the vehicle's current
//             state is saved (PhysicsSystem::SaveBodyState, VehicleConstraint::SaveState), the selected groups are
//             patched over it, and the result is read back with PhysicsSystem::RestoreBodyState (under a
//             BodyLockWrite; it moves the body between the active lists and calls NotifyBodiesAABBChanged itself,
//             PhysicsSystem.cpp:2971) and VehicleConstraint::RestoreState. Groups not selected keep their bits and
//             nothing is recomputed (no p + R * CoM, no sleep-timer reset), so kAll of a world's own state(slot) is
//             a bit-exact no-op. Wheel has no setters for suspension length or lambdas, hence the streams. The
//             contact policy says what happens to the chassis's cached contact manifolds (gate G2 restore modes,
//             s2.md §7.3).

#include "bf3/core/result.h"
#include "bf3/sim/sim_types.h"
#include "bf3/sim/static_world.h"
#include "bf3/sim/vehicle_model.h"
#include "bf3/sim/vehicle_types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace bf3::sim {

// Object layers: 0..2 the static layers (StaticLayer), 3 CollisionWorld's query layer, 4 the vehicles.
inline constexpr uint8_t kVehicleObjectLayer = 4;
inline constexpr uint32_t kMaxVehicleSlots = 64;

struct VehicleWorldOptions {
	uint32_t max_vehicles = 16; // slots 0..max_vehicles-1 (<= kMaxVehicleSlots)
	uint32_t temp_allocator_bytes = 16u << 20;
	uint32_t max_body_pairs = 65536;
	uint32_t max_contact_constraints = 16384;
	// World-level numbers (gravity, ground friction, step counts) come from this tuning, not from each model's; a
	// model built with different world keys is refused by add_vehicle.
	VehicleTuning tuning;
};

using VehicleSlot = uint32_t;

struct VehiclePlacement {
	Vec3 position{}; // vehicle frame origin, world space
	std::array<float, 4> rotation{ 0, 0, 0, 1 }; // quaternion (x, y, z, w)
};

namespace vehicle_restore {
inline constexpr uint32_t kBody = 1u << 0; // centre-of-mass position, rotation, velocities, active
inline constexpr uint32_t kController = 1u << 1; // rpm, gear, clutch, gearbox timers, previous dt
inline constexpr uint32_t kControllerInputs = 1u << 2; // forward / right / brake / hand brake inputs
inline constexpr uint32_t kWheels = 1u << 3; // per wheel: spin, angle, suspension length
inline constexpr uint32_t kWheelContacts = 1u << 4; // per wheel: contact body, position, normal, lateral
inline constexpr uint32_t kLambdas = 1u << 5; // per wheel: the four totalLambdas
inline constexpr uint32_t kPitchRoll = 1u << 6; // pitch-roll axis and lambda
inline constexpr uint32_t kStep = 1u << 7; // mCurrentStep, constraint enabled
inline constexpr uint32_t kDriver = 1u << 8; // our driver state
inline constexpr uint32_t kBodySleep = 1u << 9; // force, torque, sleep test spheres and timer, allow sleeping
// What a decoded ghost carries (vehicle_ghost.h decode_vehicle_ghost). The sleep test is not sent: after a ghost
// restore it is the client's own (from its own history), so the client falls asleep by its own test.
inline constexpr uint32_t kGhost = kBody | kController | kWheels | kDriver;
inline constexpr uint32_t kAll = (1u << 10) - 1u;
} // namespace vehicle_restore

enum class ContactCachePolicy : uint8_t {
	Keep = 0, // the world's own cached manifolds stay (the "own history" mode restores them first)
	Invalidate = 1, // BodyInterface::InvalidateContactCache(chassis): no warm start against statics next step
};

// What one vehicle touched during the last step.
// Listener counts, world state outside the snapshot: after a restore they describe the last step taken, so the G2
// tags come from the server's recorded counts, never from a client world (s2.md §8.4).
struct VehicleContacts {
	uint8_t wheels_in_contact = 0;
	uint16_t chassis_static_contacts = 0; // contact manifolds chassis vs static bodies added or persisted this step
	float max_penetration = 0; // m, over those manifolds
};

// The full state of a world at a tick (see save()). Opaque bytes.
struct VehicleWorldSnapshot {
	Tick tick = 0;
	std::vector<uint8_t> jolt; // JPH::StateRecorderImpl stream
	std::vector<VehicleDriverState> drivers; // per slot in use, slot order
	uint64_t hash = 0; // FNV-1a over jolt + drivers (equal hashes at a tick = gate G1's "state hash")
};

class VehicleWorld {
public:
	static core::Result<std::unique_ptr<VehicleWorld>> create(std::shared_ptr<const StaticWorldShapes> p_shapes,
			const VehicleWorldOptions &p_options = {});
	~VehicleWorld();
	VehicleWorld(const VehicleWorld &) = delete;
	VehicleWorld &operator=(const VehicleWorld &) = delete;

	const std::shared_ptr<const StaticWorldShapes> &shapes() const;
	size_t static_body_count() const; // = CollisionWorld's static body count over the same shapes
	// The BodyID value (index and sequence bits) of static body p_index < static_body_count(). sim.vehicle.unit checks
	// that it equals CollisionWorld's over the same shapes, which catches drift in either creation path after a merge.
	uint32_t static_body_id(size_t p_index) const;
	uint32_t vehicle_body_id(VehicleSlot p_slot) const; // static_body_count() + p_slot

	// Adds a vehicle in p_slot (unused, < max_vehicles) at p_placement, at rest, engine at min rpm, neutral driver.
	// Fails on a used slot, a model whose world keys differ from the world's tuning, or when Jolt cannot add it.
	core::Status add_vehicle(VehicleSlot p_slot, std::shared_ptr<const VehicleModel> p_model, const VehiclePlacement &p_placement);
	bool has_vehicle(VehicleSlot p_slot) const;

	// One tick: p_commands[i] drives slot p_slots[i]; slots not named get a zero command (engine idles, stand-still
	// brake per model). Commands' tick, sequence and flags are not read. Fails on an unknown slot or a duplicate.
	core::Status step(std::span<const VehicleSlot> p_slots, std::span<const VehicleCommand> p_commands);
	Tick tick() const; // ticks stepped since create (restore() sets it)

	VehicleState state(VehicleSlot p_slot) const;
	VehicleContacts contacts(VehicleSlot p_slot) const;

	void save(VehicleWorldSnapshot &r_snapshot) const;
	// Fails, leaving the world as it was, when the snapshot's slots differ from this world's, the stream length does
	// not match, or Jolt refuses the stream (the pre-restore backup is put back; see the file comment).
	core::Status restore(const VehicleWorldSnapshot &p_snapshot);
	// Writes the p_groups parts of p_state into the vehicle in p_slot (see the file comment); fields outside p_groups
	// are not read. A caller holding a partial state (a decoded ghost) decodes into a copy of state(p_slot), so the
	// fields the ghost leaves alone (previous_delta_time, the controller inputs, lambdas, contacts, the sleep test)
	// are the world's own. Fails on an unknown slot or p_state.wheel_count != the model's wheel count.
	core::Status restore_vehicle_state(VehicleSlot p_slot, const VehicleState &p_state, uint32_t p_groups, ContactCachePolicy p_contacts);
	uint64_t state_hash() const; // save() then its hash (gate G1 compares this and vehicle_state_hash per tick)

	struct Impl; // bf3_sim internal (src/vehicle_world_internal.h): PhysicsSystem, filters, job system, vehicles
	Impl &impl() const { return *impl_; }

private:
	explicit VehicleWorld(std::unique_ptr<Impl> p_impl);
	std::unique_ptr<Impl> impl_;
};

// A flat synthetic static world for tests and the handling numbers: a p_half_size m square at height p_y split into
// 64 m tiles (two triangles each, StaticLayer::Collision), plus the optional features of s2.md §8.1 (a 1.5 m wall, a
// row of 0.15 m bumps, a 0.6 m kicker ramp, a 12 degree slope) placed away from the origin.
struct SyntheticVehicleGround {
	float half_size = 512.0f;
	float y = 0.0f;
	bool features = true;
};
core::Result<std::shared_ptr<const StaticWorldShapes>> build_synthetic_vehicle_ground(const SyntheticVehicleGround &p_ground = {});

} // namespace bf3::sim
