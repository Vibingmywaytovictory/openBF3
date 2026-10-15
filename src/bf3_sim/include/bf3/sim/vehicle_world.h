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
// Static bodies are byte-identical to CollisionWorld's (Jolt's default friction 0.2 included). Friction against
// them comes from combine callbacks instead (s2.md §6.1): VehicleConstraint::SetCombineFriction gives a wheel
// tyre friction x tuning.ground_friction on a static body (Jolt's default sqrt(tyre x 0.2) otherwise), and
// PhysicsSystem::SetCombineFriction gives a chassis-static contact the chassis friction (RigidBodyData 0.5) and
// keeps the geometric mean between two dynamic bodies. Restitution stays Jolt's max(a, b) = 0.4.
//
// Step: one PhysicsSystem::Update(kTickSeconds, tuning.collision_steps) per tick on a JobSystemSingleThreaded owned
// by the world (no shared job threads: the step's order is the world's own; s2.md §6.3). Before the update, per
// vehicle in slot order (the driver pass, identical in every world, s2.md §5.4):
//   1. the driver (our code; vehicle_model.h numbers) turns the tick's VehicleCommand and the driver state into the
//      controller's four inputs and advances the stand-still timer. Steering sign: VehicleCommand::steer > 0 is
//      "right" in the project's axes (command.h / soldier_motor.cpp: right = (-cos yaw, 0, sin yaw), -X at yaw 0),
//      which is Jolt's right (forward x up = (0,0,1) x (0,1,0) = -X; WheeledVehicleController.cpp:262 sets the
//      steer angle to -right x max about +Y), so the Jolt right input is +steer_filter x the falloff, no flip;
//   2. the wake rule: if the command's throttle, brake or steer is non-zero or its handbrake bit is set,
//      BodyInterface::ActivateBody(chassis). SetDriverInput only stores the inputs and a sleeping body is never
//      woken by them (WheeledVehicleController.h:105); Jolt's own vehicle samples do the same
//      (VehicleConstraintTest.cpp:228). ActivateBody also resets the sleep timer of an awake body
//      (BodyManager.cpp:511), so a vehicle with input never sleeps. The rule reads the command, not the driver's
//      derived inputs: the stand-still brake alone does not wake or hold a vehicle awake, so an idle vehicle can
//      sleep (WheeledVehicleController::AllowSleep: no forward input, idling engine, no shift);
//   3. SetDriverInput, then the air drag force with EActivation::DontActivate (drag alone must not wake a sleeping
//      vehicle; BodyInterface.cpp:699).
// Contact listener callbacks only count (chassis-static and chassis-chassis contacts per tick), so their order
// cannot matter.
//
// Threads: every method on the owner's thread. Two worlds over the same shapes may step at the same time on
// different threads (sim.vehicle.unit runs them so).
//
// Save / restore:
//   save()    the full state: PhysicsSystem::SaveState(EStateRecorderState::All) with a filter that keeps every
//             non-static body (statics never change), i.e. global state, the vehicle bodies, the contact cache and
//             every constraint (VehicleConstraint, controller), plus our driver states and the tick. In-process and
//             trusted: it is never sent or stored (the network carries VehicleGhostCodes, vehicle_ghost.h).
//             Not in it, because Jolt saves none of them: the broadphase trees' layout and their fattened
//             bounds (Docs/Architecture.md:804: broadphase queries are not deterministic until the next
//             maintenance update), BroadPhaseQuadTree::mNextLayerToUpdate, and the order of the active-body list
//             (RestoreState re-adds bodies by append / swap-remove, BodyManager.cpp:815-823). That is harmless
//             while candidate order cannot reach the result (contacts and constraints are sorted before solving,
//             PhysicsSystem.cpp:1503-1506, s2.md §6.3); the multi-vehicle check G1m and the in-place rollback of
//             G1r (s2.md §1) are where it would show, and their divergence report names it.
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
	uint16_t chassis_vehicle_contacts = 0; // the same against other vehicles' chassis (G1m)
	float max_penetration = 0; // m, over those manifolds
};

// One part of PhysicsSystem::SaveState (JPH::EStateRecorderState Global / Bodies / Contacts / Constraints), for
// the G1 divergence report: Jolt's combined stream has no section markers, so on a mismatch both worlds are saved
// again one section at a time and compared section by section.
enum class VehicleSnapshotSection : uint8_t { Global = 0, Bodies = 1, Contacts = 2, Constraints = 3 };
inline constexpr size_t kVehicleSnapshotSectionCount = 4;
const char *vehicle_snapshot_section_name(VehicleSnapshotSection p_section); // "global", "bodies", "contacts", "constraints"

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
	// One section of save()'s Jolt stream on its own (same static-skipping filter); diagnostics only.
	void save_section(VehicleSnapshotSection p_section, std::vector<uint8_t> &r_bytes) const;
	// Fails, leaving the world as it was, when the snapshot's slots differ from this world's, the stream length does
	// not match, or Jolt refuses the stream (the pre-restore backup is put back; see the file comment).
	core::Status restore(const VehicleWorldSnapshot &p_snapshot);
	// Writes the p_groups parts of p_state into the vehicle in p_slot (see the file comment); fields outside p_groups
	// are not read. A caller holding a partial state (a decoded ghost) decodes into a copy of state(p_slot), so the
	// fields the ghost leaves alone (previous_delta_time, the controller inputs, lambdas, contacts, the sleep test)
	// are the world's own. Fails, before anything is written, on an unknown slot, p_state.wheel_count != the model's
	// wheel count, a non-finite float in a selected group, a rotation whose squared norm is outside 1 +- 1e-3, a gear
	// outside the model's range or a contact body that is neither kNoContactBody nor a body of this world: a decoded
	// network ghost (M3) fails with an error, never a NaN in Jolt's broadphase.
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
